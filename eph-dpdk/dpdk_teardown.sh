#!/usr/bin/env bash
# dpdk_teardown.sh — Detect or clean up DPDK environment
#
# Default (no args):  read-only check, prints current DPDK state
# --teardown:         actually unbind NIC + release hugepages (needs sudo)
#
# Usage:
#   bash dpdk_teardown.sh                              # check only
#   sudo bash dpdk_teardown.sh --teardown              # full cleanup (auto-detect PCI)
#   sudo bash dpdk_teardown.sh --teardown --pci 0000:28:00.0 --keep-hugepages
#   sudo bash dpdk_teardown.sh --teardown --keep-module --keep-env-file

set -euo pipefail

# ── Colors (auto-disabled in non-terminal / CI) ──────────────────────────────
if [[ -t 1 ]] && [[ -z "${NO_COLOR:-}" ]] && [[ -z "${CI:-}" ]]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'
    BOLD='\033[1m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; NC=''
fi

info()  { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!!]${NC} $*"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; }
header(){ echo -e "\n${BOLD}${CYAN}── $* ──${NC}"; }

# ── Args ──────────────────────────────────────────────────────────────────────
MODE="check"
PCI_ADDR=""
ENV_FILE=".dpdk_env"
DO_HUGEPAGES=true
DO_MODULE=true
DO_ENV_FILE=true

while [[ $# -gt 0 ]]; do
    case "$1" in
        --teardown)        MODE="teardown"; shift ;;
        --pci)             PCI_ADDR="$2"; shift 2 ;;
        --env-file)        ENV_FILE="$2"; shift 2 ;;
        --keep-hugepages)  DO_HUGEPAGES=false; shift ;;
        --keep-module)     DO_MODULE=false; shift ;;
        --keep-env-file)   DO_ENV_FILE=false; shift ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Default: read-only check (prints current DPDK environment state)"
            echo ""
            echo "Options:"
            echo "  --teardown           Unbind NIC + clean up environment (needs sudo)"
            echo "  --pci <addr>         PCI address to unbind (auto-detected if omitted)"
            echo "  --env-file <path>    Env file to remove (default: .dpdk_env)"
            echo "  --keep-hugepages     Skip releasing hugepages"
            echo "  --keep-module        Skip unloading vfio-pci kernel module"
            echo "  --keep-env-file      Skip deleting the .dpdk_env file"
            echo "  -h, --help           Show this help"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Helpers ───────────────────────────────────────────────────────────────────

find_devbind() {
    command -v dpdk-devbind.py 2>/dev/null \
        || find /usr/local -name "dpdk-devbind.py" 2>/dev/null | head -1
}

# Find all PCI addresses currently bound to DPDK-compatible drivers (vfio-pci, uio_pci_generic)
find_dpdk_bound_nics() {
    local devbind
    devbind=$(find_devbind) || true
    [[ -z "$devbind" ]] && return

    $devbind --status-dev net 2>/dev/null \
        | awk '/DPDK-compatible driver/,/^$/' \
        | grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]' \
        || true
}

# Infer the appropriate kernel driver from PCI vendor:device ID.
#
# DPDK binds devices away from their kernel driver. To hand the NIC back we
# need to know what driver the kernel *would* use for this device.  The
# approach below queries `lspci -n` for the vendor:device pair and walks a
# lookup table of well-known DPDK-capable NICs.  Unknown devices fall back to
# the generic `uio_pci_generic` — callers that need more precision should
# pass --pci explicitly and extend the table below.
infer_kernel_driver() {
    local pci="$1"

    # vendor:device string, e.g. "8086:1572"
    local vid_did
    vid_did=$(lspci -n -s "$pci" 2>/dev/null | awk '{print $3}')

    if [[ -z "$vid_did" ]]; then
        warn "Cannot read PCI ID for ${pci} — defaulting to 'ixgbe'"
        echo "ixgbe"
        return
    fi

    # Lookup table: vendor:device → kernel driver
    # Covers the most common DPDK-capable NICs on EC2 and bare metal.
    # Uses case instead of declare -A for Bash 3.x compatibility.
    local driver=""
    case "$vid_did" in
        # Intel 10GbE (82599, X520, X540, X550)
        8086:10fb|8086:1528|8086:1560|8086:15aa) driver="ixgbe" ;;
        # Intel 25/40/100GbE (X710, XXV710, XL710)
        8086:1572|8086:158b|8086:1583|8086:1584) driver="i40e" ;;
        # Intel E810
        8086:1592|8086:1593) driver="ice" ;;
        # Intel 1GbE (igb)
        8086:10c9|8086:10e6|8086:1521|8086:1522) driver="igb" ;;
        # Mellanox / NVIDIA (mlx4, mlx5)
        15b3:1013|15b3:1015) driver="mlx4_core" ;;
        15b3:1017|15b3:1019) driver="mlx5_core" ;;
        # Amazon ENA
        1d0f:ec20|1d0f:ec21) driver="ena" ;;
        # Virtio (KVM guests)
        1af4:1000|1af4:1041) driver="virtio-pci" ;;
    esac

    if [[ -n "$driver" ]]; then
        echo "$driver"
    else
        warn "Unknown PCI ID ${vid_did} for ${pci} — defaulting to 'ixgbe'"
        echo "ixgbe"
    fi
}

# ── 1. Check / report current state ──────────────────────────────────────────

check_state() {
    header "Hugepages"
    local total free size_kb total_mb
    total=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
    free=$(grep HugePages_Free  /proc/meminfo | awk '{print $2}')
    size_kb=$(grep Hugepagesize /proc/meminfo | awk '{print $2}')
    total_mb=$(( total * size_kb / 1024 ))

    if [[ "$total" -gt 0 ]]; then
        info "Hugepages: ${total} x ${size_kb}KB = ${total_mb}MB (${free} free)"
    else
        info "No hugepages configured (already clean)"
    fi

    header "DPDK-bound NICs"
    local devbind
    devbind=$(find_devbind) || true
    if [[ -z "$devbind" ]]; then
        warn "dpdk-devbind.py not found — cannot check NIC binding status"
        return
    fi

    local bound_nics
    bound_nics=$(find_dpdk_bound_nics)
    if [[ -n "$bound_nics" ]]; then
        warn "NICs currently bound to DPDK driver:"
        for nic in $bound_nics; do
            local driver
            driver=$(infer_kernel_driver "$nic")
            echo "    ${nic}  →  would restore to: ${driver}"
        done
        echo ""
        echo "    Run:  sudo bash dpdk_teardown.sh --teardown"
    else
        info "No NICs bound to DPDK driver (already clean)"
    fi

    header "vfio-pci module"
    if lsmod | grep -q vfio_pci; then
        warn "vfio-pci module is loaded"
    else
        info "vfio-pci module is not loaded (already clean)"
    fi

    header "Env file"
    if [[ -f "$ENV_FILE" ]]; then
        warn "${ENV_FILE} exists"
    else
        info "${ENV_FILE} not found (already clean)"
    fi
}

# ── 2. Unbind NIC ─────────────────────────────────────────────────────────────

teardown_nic() {
    header "Unbinding DPDK NIC(s)"

    local devbind
    devbind=$(find_devbind) || true
    if [[ -z "$devbind" ]]; then
        fail "dpdk-devbind.py not found — cannot unbind NIC"
        return 1
    fi

    # Build list of PCI addresses to process
    local targets=()
    if [[ -n "$PCI_ADDR" ]]; then
        targets=("$PCI_ADDR")
    else
        # Auto-detect all DPDK-bound NICs
        local auto_detected
        auto_detected=$(find_dpdk_bound_nics)
        if [[ -z "$auto_detected" ]]; then
            info "No NICs currently bound to DPDK driver — nothing to unbind"
            return
        fi
        mapfile -t targets <<< "$auto_detected"
    fi

    for pci in "${targets[@]}"; do
        # Verify the device is actually bound to a DPDK driver before touching it
        if ! $devbind --status-dev net 2>/dev/null | grep "$pci" | grep -qE "drv=(vfio-pci|uio_pci_generic|igb_uio)"; then
            warn "${pci} does not appear to be bound to a DPDK driver — skipping"
            continue
        fi

        # Safety: refuse to unbind in a way that would affect the default route
        # (though this is less dangerous for teardown than setup, be explicit)
        local kernel_driver
        kernel_driver=$(infer_kernel_driver "$pci")
        info "Restoring ${pci} → ${kernel_driver}"

        if ! modinfo "$kernel_driver" &>/dev/null; then
            warn "Kernel module '${kernel_driver}' not found — skipping bind-back for ${pci}"
            warn "You may need to manually: sudo dpdk-devbind.py -b <driver> ${pci}"
            continue
        fi

        # Load the kernel module if needed
        if ! lsmod | grep -q "^${kernel_driver//-/_}"; then
            modprobe "$kernel_driver" && info "Loaded kernel module: ${kernel_driver}"
        fi

        $devbind -b "$kernel_driver" "$pci"
        info "Unbound ${pci} from DPDK, restored to ${kernel_driver}"

        # Confirm the interface came back
        sleep 0.5  # brief settle time for udev
        if [[ -d "/sys/bus/pci/devices/${pci}/net" ]]; then
            local iface
            iface=$(ls "/sys/bus/pci/devices/${pci}/net/" 2>/dev/null | head -1)
            info "Kernel interface restored: ${iface}"
        else
            warn "No kernel net interface appeared for ${pci} — may need manual 'ip link set up'"
        fi
    done
}

# ── 3. Release hugepages ──────────────────────────────────────────────────────

teardown_hugepages() {
    header "Releasing hugepages"

    local total size_kb
    total=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
    size_kb=$(grep Hugepagesize /proc/meminfo | awk '{print $2}')

    if [[ "$total" -eq 0 ]]; then
        info "No hugepages to release"
        return
    fi

    local free
    free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    local in_use=$(( total - free ))

    if [[ "$in_use" -gt 0 ]]; then
        # Some hugepages are pinned — refuse to release to avoid corrupting a
        # live DPDK process or any other application using them.
        warn "${in_use} hugepage(s) are currently in use — refusing to release"
        warn "Stop all DPDK processes first, then re-run teardown"
        return 1
    fi

    echo 0 > "/sys/kernel/mm/hugepages/hugepages-${size_kb}kB/nr_hugepages"

    local remaining
    remaining=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
    if [[ "$remaining" -eq 0 ]]; then
        info "Released all hugepages (was ${total} x ${size_kb}KB)"
    else
        warn "Expected 0 hugepages remaining but got ${remaining} — kernel may keep some allocated"
    fi
}

# ── 4. Unload vfio-pci ────────────────────────────────────────────────────────

teardown_module() {
    header "Unloading vfio-pci module"

    if ! lsmod | grep -q vfio_pci; then
        info "vfio-pci not loaded — nothing to do"
        return
    fi

    # Check if any device is still using the module before rmmod
    local users
    users=$(lsmod | awk '/^vfio_pci/ {print $3}')
    if [[ "${users:-0}" -gt 0 ]]; then
        warn "vfio-pci still has ${users} user(s) — skipping unload"
        warn "All DPDK NICs must be unbound before the module can be removed"
        return 1
    fi

    rmmod vfio-pci 2>/dev/null && info "Unloaded vfio-pci" \
        || warn "rmmod vfio-pci failed (may still be referenced) — skipping"
}

# ── 5. Remove env file ────────────────────────────────────────────────────────

teardown_env_file() {
    header "Removing ${ENV_FILE}"

    if [[ ! -f "$ENV_FILE" ]]; then
        info "${ENV_FILE} not found — nothing to remove"
        return
    fi

    rm -f "$ENV_FILE"
    info "Deleted ${ENV_FILE}"
}

# ── 6. Final status ───────────────────────────────────────────────────────────

print_final_status() {
    header "Teardown complete"

    local total
    total=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
    [[ "$total" -eq 0 ]] && info "Hugepages: none" \
        || warn "Hugepages remaining: ${total}"

    local devbind
    devbind=$(find_devbind) || true
    if [[ -n "$devbind" ]]; then
        local bound_nics
        bound_nics=$(find_dpdk_bound_nics)
        [[ -z "$bound_nics" ]] && info "DPDK NICs: none bound" \
            || warn "DPDK NICs still bound: ${bound_nics}"
    fi

    lsmod | grep -q vfio_pci \
        && warn "vfio-pci module: still loaded" \
        || info "vfio-pci module: unloaded"

    [[ -f "$ENV_FILE" ]] \
        && warn "${ENV_FILE}: still exists" \
        || info "${ENV_FILE}: removed"
}

# ── Main ──────────────────────────────────────────────────────────────────────

echo -e "${BOLD}DPDK Environment teardown${NC}"
echo "=============================="

if [[ "$MODE" == "teardown" ]]; then
    if [[ $EUID -ne 0 ]]; then
        fail "--teardown requires root (sudo)"
        exit 1
    fi

    teardown_nic
    $DO_HUGEPAGES && teardown_hugepages || warn "Skipping hugepage release (--keep-hugepages)"
    $DO_MODULE    && teardown_module    || warn "Skipping module unload (--keep-module)"
    $DO_ENV_FILE  && teardown_env_file  || warn "Skipping env file deletion (--keep-env-file)"
    print_final_status
else
    check_state
fi
