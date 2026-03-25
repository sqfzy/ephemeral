#!/usr/bin/env bash
# dpdk_setup.sh — Detect or configure DPDK environment
#
# Default (no args):  read-only check, prints status and suggested commands
# --setup:            actually configure hugepages + bind NIC (needs sudo)
#
# Usage:
#   bash dpdk_setup.sh                              # check only
#   sudo bash dpdk_setup.sh --setup --pci 0000:28:00.0   # configure
#   source .dpdk_env && sudo ./dpdk_echo $EAL_ARGS -- $APP_ARGS

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
HUGEPAGES=256    # 256 x 2MB = 512MB, enough for most use cases
ENV_FILE=".dpdk_env"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --setup)    MODE="setup"; shift ;;
        --pci)      PCI_ADDR="$2"; shift 2 ;;
        --hugepages) HUGEPAGES="$2"; shift 2 ;;
        --env-file) ENV_FILE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Default: read-only check (prints environment status)"
            echo ""
            echo "Options:"
            echo "  --setup              Configure hugepages + bind NIC (needs sudo)"
            echo "  --pci <addr>         PCI address to bind (e.g. 0000:28:00.0)"
            echo "  --hugepages <n>      Number of 2MB hugepages (default: 256)"
            echo "  --env-file <path>    Output env file path (default: .dpdk_env)"
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

# Query EC2 instance metadata (IMDSv2)
ec2_metadata() {
    local path="$1"
    local token
    token=$(curl -sf --max-time 2 -X PUT \
        "http://169.254.169.254/latest/api/token" \
        -H "X-aws-ec2-metadata-token-ttl-seconds: 60" 2>/dev/null) || return 1
    curl -sf --max-time 2 \
        -H "X-aws-ec2-metadata-token: $token" \
        "http://169.254.169.254/latest/meta-data/$path" 2>/dev/null
}

# Get IP for a given MAC via EC2 metadata
ec2_ip_for_mac() {
    local mac="$1"
    ec2_metadata "network/interfaces/macs/${mac}/local-ipv4s"
}

# Get MAC address of a PCI device's DPDK port
get_dpdk_mac() {
    local pci="$1"
    # Try reading from sysfs (works even when bound to vfio-pci on some systems)
    local mac_file="/sys/bus/pci/devices/${pci}/net/*/address"
    if ls $mac_file &>/dev/null; then
        cat $mac_file 2>/dev/null | head -1
        return
    fi
    # If bound to DPDK driver, no net/ dir — try EC2 metadata to match
    # by listing all MACs and finding the one NOT used by kernel interfaces
    local kernel_macs
    kernel_macs=$(ip -o link show | awk -F'link/ether ' '{print $2}' | awk '{print $1}' | sort)
    local all_macs
    all_macs=$(ec2_metadata "network/interfaces/macs/" 2>/dev/null | tr -d '/')
    for m in $all_macs; do
        if ! echo "$kernel_macs" | grep -qi "$m"; then
            echo "$m"
            return
        fi
    done
}

# ── 1. Hugepages ──────────────────────────────────────────────────────────────

check_hugepages() {
    header "Hugepages"
    local total free
    total=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
    free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    local size_kb
    size_kb=$(grep Hugepagesize /proc/meminfo | awk '{print $2}')
    local total_mb=$(( total * size_kb / 1024 ))

    if [[ "$total" -gt 0 ]]; then
        info "Hugepages: ${total} x ${size_kb}KB = ${total_mb}MB (${free} free)"
    else
        fail "No hugepages configured"
        echo "    Fix: echo ${HUGEPAGES} | sudo tee /sys/kernel/mm/hugepages/hugepages-${size_kb}kB/nr_hugepages"
    fi
}

setup_hugepages() {
    header "Configuring hugepages"
    local size_kb
    size_kb=$(grep Hugepagesize /proc/meminfo | awk '{print $2}')
    local current
    current=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')

    if [[ "$current" -ge "$HUGEPAGES" ]]; then
        info "Already have ${current} hugepages (>= requested ${HUGEPAGES})"
        return
    fi

    echo "$HUGEPAGES" > "/sys/kernel/mm/hugepages/hugepages-${size_kb}kB/nr_hugepages"
    local actual
    actual=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
    if [[ "$actual" -ge "$HUGEPAGES" ]]; then
        info "Configured ${actual} hugepages (${size_kb}KB each)"
    else
        warn "Requested ${HUGEPAGES} but got ${actual} (memory fragmentation?)"
    fi
}

# ── 2. NIC status ─────────────────────────────────────────────────────────────

check_nics() {
    header "Network interfaces"
    local devbind
    devbind=$(find_devbind)

    if [[ -z "$devbind" ]]; then
        warn "dpdk-devbind.py not found — cannot list NIC binding status"
        echo "    Install DPDK tools or check PATH"
        return
    fi

    echo ""
    $devbind --status-dev net 2>/dev/null
    echo ""

    # Count DPDK-bound devices
    local dpdk_count
    dpdk_count=$($devbind --status-dev net 2>/dev/null \
        | awk '/DPDK-compatible driver/,/^$/' | grep -c "drv=" || true)

    if [[ "$dpdk_count" -gt 0 ]]; then
        info "${dpdk_count} NIC(s) bound to DPDK driver"
    else
        warn "No NICs bound to DPDK driver"
        echo ""
        echo "    Available NICs for binding:"
        lspci | grep -i 'ethernet\|network' | while read -r line; do
            local addr="${line%% *}"
            # Skip if it's the active kernel interface
            if [[ -d "/sys/bus/pci/devices/0000:${addr}/net" ]]; then
                local iface
                iface=$(ls "/sys/bus/pci/devices/0000:${addr}/net/" 2>/dev/null | head -1)
                echo "    0000:${addr} — kernel interface: ${iface} (DO NOT bind if this is your SSH connection)"
            else
                echo "    0000:${addr} — no kernel interface (safe to bind)"
            fi
        done
        echo ""
        echo "    To bind: sudo dpdk-devbind.py -b vfio-pci <pci_addr>"
    fi
}

setup_nic() {
    header "Binding NIC to DPDK"

    if [[ -z "$PCI_ADDR" ]]; then
        fail "--pci <addr> is required with --setup"
        echo "    Example: sudo $0 --setup --pci 0000:28:00.0"
        exit 1
    fi

    local devbind
    devbind=$(find_devbind)
    if [[ -z "$devbind" ]]; then
        fail "dpdk-devbind.py not found"
        exit 1
    fi

    # Check if already bound
    if $devbind --status-dev net 2>/dev/null | grep "$PCI_ADDR" | grep -q "drv=vfio-pci"; then
        info "${PCI_ADDR} already bound to vfio-pci"
        return
    fi

    # Safety: refuse to bind the active SSH interface
    if [[ -d "/sys/bus/pci/devices/${PCI_ADDR}/net" ]]; then
        local iface
        iface=$(ls "/sys/bus/pci/devices/${PCI_ADDR}/net/" 2>/dev/null | head -1)
        if ip route get 1.1.1.1 2>/dev/null | grep -q "dev ${iface}"; then
            fail "${PCI_ADDR} (${iface}) is the default route interface — binding it would kill SSH!"
            echo "    Use a secondary NIC instead."
            exit 1
        fi

        # dpdk-devbind.py refuses to bind an interface that is still UP.
        # Bring it down first so the bind succeeds.
        if ip link show "$iface" 2>/dev/null | grep -q "state UP"; then
            ip link set "$iface" down
            info "Brought ${iface} down before binding"
        fi
    fi

    # Load vfio-pci in the correct order for no-IOMMU environments (e.g. EC2):
    #   1. Load base vfio module first — this creates the sysfs parameter file
    #   2. Enable no-IOMMU mode before vfio-pci is loaded
    #   3. Load vfio-pci
    # NOTE: use grep -w to match exactly "vfio_pci" — "vfio_pci_core" is a
    # different module and must not be mistaken for vfio-pci already being loaded.
    if ! lsmod | grep -qw vfio_pci; then
        # Step 1: base vfio module (creates /sys/module/vfio/parameters/)
        if ! lsmod | grep -qw vfio; then
            modprobe vfio
            info "Loaded vfio module"
        fi

        # Step 2: enable no-IOMMU (sysfs node only exists after vfio is loaded)
        local noiommu_path="/sys/module/vfio/parameters/enable_unsafe_noiommu_mode"
        if [[ -f "$noiommu_path" && "$(cat "$noiommu_path")" != "Y" ]]; then
            echo 1 > "$noiommu_path"
            info "Enabled vfio no-IOMMU mode (required on EC2 / machines without IOMMU)"
        fi

        # Step 3: now vfio-pci can be loaded cleanly
        modprobe vfio-pci
        info "Loaded vfio-pci module"
    fi

    # Bind and verify — devbind exits 0 even on some warnings, so check status
    $devbind -b vfio-pci "$PCI_ADDR"
    if $devbind --status-dev net 2>/dev/null | grep "$PCI_ADDR" | grep -q "drv=vfio-pci"; then
        info "Bound ${PCI_ADDR} to vfio-pci"
    else
        fail "Bind command ran but ${PCI_ADDR} is still not using vfio-pci"
        echo "    Check: sudo dpdk-devbind.py --status-dev net"
        exit 1
    fi
}

# ── 3. IP / Gateway detection ─────────────────────────────────────────────────

detect_ip_gateway() {
    header "DPDK NIC IP & Gateway"

    local dpdk_pci=""
    local devbind
    devbind=$(find_devbind)

    # Find DPDK-bound PCI address
    if [[ -n "$PCI_ADDR" ]]; then
        dpdk_pci="$PCI_ADDR"
    elif [[ -n "$devbind" ]]; then
        dpdk_pci=$($devbind --status-dev net 2>/dev/null \
            | awk '/DPDK-compatible driver/,/^$/' | grep "drv=" \
            | head -1 | awk '{print $1}')
    fi

    if [[ -z "$dpdk_pci" ]]; then
        warn "No DPDK-bound NIC found — cannot detect IP"
        return
    fi

    # Gateway from default route
    local gateway
    gateway=$(ip route show default | awk '{print $3}' | head -1)
    [[ -n "$gateway" ]] && info "Gateway: ${gateway}" || warn "Cannot detect gateway"

    # Try EC2 metadata
    local dpdk_mac dpdk_ip
    dpdk_mac=$(get_dpdk_mac "$dpdk_pci")

    if [[ -n "$dpdk_mac" ]]; then
        dpdk_ip=$(ec2_ip_for_mac "$dpdk_mac" 2>/dev/null)
        if [[ -n "$dpdk_ip" ]]; then
            info "DPDK NIC (${dpdk_pci}): MAC=${dpdk_mac}, IP=${dpdk_ip}"

            # Export for env file generation
            DETECTED_PCI="$dpdk_pci"
            DETECTED_IP="$dpdk_ip"
            DETECTED_GW="${gateway:-}"
            DETECTED_MAC="$dpdk_mac"
            return
        fi
    fi

    warn "Cannot auto-detect DPDK NIC IP (not on EC2, or metadata unavailable)"
    echo "    You'll need to specify --local-ip manually when running your app"

    DETECTED_PCI="$dpdk_pci"
    DETECTED_IP=""
    DETECTED_GW="${gateway:-}"
    DETECTED_MAC="${dpdk_mac:-}"
}

# ── 4. Generate .dpdk_env ─────────────────────────────────────────────────────

generate_env_file() {
    header "Generating ${ENV_FILE}"

    if [[ -z "${DETECTED_IP:-}" ]]; then
        warn "IP not detected — env file will have placeholders"
        DETECTED_IP="<YOUR_DPDK_IP>"
    fi
    if [[ -z "${DETECTED_GW:-}" ]]; then
        DETECTED_GW="<YOUR_GATEWAY_IP>"
    fi

    cat > "$ENV_FILE" <<EOF
# DPDK environment — generated by dpdk_setup.sh at $(date '+%Y-%m-%d %H:%M:%S')
# Source this file:  source ${ENV_FILE}

# EAL arguments (before '--')
EAL_ARGS="-a ${DETECTED_PCI:-0000:00:00.0}"

# Application arguments (after '--')
DPDK_LOCAL_IP="${DETECTED_IP}"
DPDK_GATEWAY_IP="${DETECTED_GW}"
APP_ARGS="--local-ip \${DPDK_LOCAL_IP} --gateway-ip \${DPDK_GATEWAY_IP}"

# Run example:
#   source ${ENV_FILE}
#   sudo ./dpdk_echo \$EAL_ARGS -- \$APP_ARGS --host fstream.binance.com --path /ws/btcusdt@trade
EOF

    info "Written to ${ENV_FILE}"
    echo ""
    cat "$ENV_FILE"
}

# ── 5. Summary ────────────────────────────────────────────────────────────────

print_summary() {
    header "Quick start"

    local pci="${DETECTED_PCI:-<PCI_ADDR>}"
    local ip="${DETECTED_IP:-<LOCAL_IP>}"
    local gw="${DETECTED_GW:-<GATEWAY_IP>}"

    echo ""
    echo "  # Build the example"
    echo "  cd dpdk/examples && make"
    echo ""
    echo "  # Run"
    echo "  sudo ./dpdk_echo -a ${pci} -- \\"
    echo "      --local-ip ${ip} --gateway-ip ${gw} \\"
    echo "      --host fstream.binance.com --path /ws/btcusdt@trade"
    echo ""

    if [[ "$MODE" == "check" ]] && [[ "${DETECTED_IP:-}" == "" || "${DETECTED_IP:-}" == "<YOUR_DPDK_IP>" ]]; then
        echo "  !! Run with --setup to configure environment:"
        echo "  sudo bash dpdk_setup.sh --setup --pci <PCI_ADDR>"
        echo ""
    fi
}

# ── Main ──────────────────────────────────────────────────────────────────────

echo -e "${BOLD}DPDK Environment ${MODE}${NC}"
echo "=============================="

DETECTED_PCI="" DETECTED_IP="" DETECTED_GW="" DETECTED_MAC=""

if [[ "$MODE" == "setup" ]]; then
    if [[ $EUID -ne 0 ]]; then
        fail "--setup requires root (sudo)"
        exit 1
    fi
    setup_hugepages
    setup_nic
    check_nics
    detect_ip_gateway
    generate_env_file
    print_summary
else
    check_hugepages
    check_nics
    detect_ip_gateway
    print_summary
fi
