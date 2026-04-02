#!/usr/bin/env bash
# bench_setup.sh — Dual-NIC back-to-back benchmark setup and runner.
#
# Configures two NICs for back-to-back benchmarking, runs socket benchmarks
# (kernel TCP), switches NIC-B to DPDK (vfio-pci), runs DPDK benchmarks,
# then restores NIC-B to its original kernel driver.
#
# Topology:
#   NIC-A (kernel, 10.0.0.1) ──physical cable──> NIC-B (10.0.0.2)
#   Mock Server: kernel TCP server on NIC-A:9999 (in-process thread)
#   Socket Bench: NIC-B (kernel driver) → connect(10.0.0.1:9999)
#   DPDK Bench:   NIC-B (DPDK PMD)     → TcpSession connect(10.0.0.1:9999)
#
# Usage:
#   sudo ./scripts/bench_setup.sh --nic-a eth0 --nic-b eth1 \
#       --duration 10 --symbols BTCUSDT,ETHUSDT,SOLUSDT
#
# Requirements:
#   - Root or CAP_NET_ADMIN
#   - Two NICs connected back-to-back
#   - DPDK installed (dpdk-devbind.py in PATH)
#   - vfio-pci kernel module available
#   - Benchmark binaries built (in build/ directory)

set -euo pipefail

# ── Constants ────────────────────────────────────────────────────────────────
readonly IP_A="10.0.0.1"
readonly IP_B="10.0.0.2"
readonly SUBNET="/24"
readonly MOCK_PORT=9999

# ── Defaults ─────────────────────────────────────────────────────────────────
NIC_A=""
NIC_B=""
DURATION=10
SYMBOLS="BTCUSDT,ETHUSDT,SOLUSDT"
BUILD_DIR=""
EAL_ARGS=""
SKIP_SOCKET=false
SKIP_DPDK=false

# ── State for cleanup ───────────────────────────────────────────────────────
NIC_B_PCI=""
NIC_B_ORIG_DRIVER=""
NIC_B_BOUND_DPDK=false
IP_A_CONFIGURED=false
IP_B_CONFIGURED=false

# ── Color output ─────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    BLUE='\033[0;34m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' BLUE='' BOLD='' NC=''
fi

log_info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }
log_phase() { echo -e "\n${BOLD}${BLUE}=== Phase $1: $2 ===${NC}\n"; }

# ── Usage ────────────────────────────────────────────────────────────────────
usage() {
    cat <<'EOF'
Usage: sudo ./scripts/bench_setup.sh [OPTIONS]

Required:
  --nic-a IFACE       Network interface for NIC-A (mock server side)
  --nic-b IFACE       Network interface for NIC-B (benchmark client side)

Optional:
  --duration SEC      Benchmark duration in seconds (default: 10)
  --symbols SYM,...   Comma-separated symbols (default: BTCUSDT,ETHUSDT,SOLUSDT)
  --build-dir DIR     Build directory (auto-detected if omitted)
  --eal-args "ARGS"   Extra EAL arguments for DPDK benchmarks
  --skip-socket       Skip socket benchmarks
  --skip-dpdk         Skip DPDK benchmarks
  --help              Show this help

Example:
  sudo ./scripts/bench_setup.sh \
      --nic-a ens5 --nic-b ens6 \
      --duration 30 --symbols BTCUSDT,ETHUSDT
EOF
    exit 0
}

# ── Argument parsing ─────────────────────────────────────────────────────────
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --nic-a)       NIC_A="$2"; shift 2 ;;
            --nic-b)       NIC_B="$2"; shift 2 ;;
            --duration)    DURATION="$2"; shift 2 ;;
            --symbols)     SYMBOLS="$2"; shift 2 ;;
            --build-dir)   BUILD_DIR="$2"; shift 2 ;;
            --eal-args)    EAL_ARGS="$2"; shift 2 ;;
            --skip-socket) SKIP_SOCKET=true; shift ;;
            --skip-dpdk)   SKIP_DPDK=true; shift ;;
            --help)        usage ;;
            *)
                log_error "Unknown option: $1"
                usage
                ;;
        esac
    done

    if [[ -z "$NIC_A" || -z "$NIC_B" ]]; then
        log_error "--nic-a and --nic-b are required"
        usage
    fi
}

# ── Preflight checks ────────────────────────────────────────────────────────
preflight() {
    # Must be root
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root (sudo)"
        exit 1
    fi

    # Verify NICs exist
    if ! ip link show "$NIC_A" &>/dev/null; then
        log_error "NIC-A '$NIC_A' not found"
        exit 1
    fi
    if ! ip link show "$NIC_B" &>/dev/null; then
        log_error "NIC-B '$NIC_B' not found"
        exit 1
    fi

    # Auto-detect build directory
    if [[ -z "$BUILD_DIR" ]]; then
        local script_dir
        script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
        local project_dir
        project_dir="$(dirname "$script_dir")"
        # Try common xmake build paths
        for candidate in \
            "$project_dir/build/linux/aarch64/release" \
            "$project_dir/build/linux/x86_64/release" \
            "$project_dir/build/linux/aarch64/debug" \
            "$project_dir/build/linux/x86_64/debug" \
            "$project_dir/build"; do
            if [[ -d "$candidate" ]]; then
                BUILD_DIR="$candidate"
                break
            fi
        done
        if [[ -z "$BUILD_DIR" ]]; then
            log_error "Cannot auto-detect build directory. Use --build-dir"
            exit 1
        fi
    fi
    log_info "Build directory: $BUILD_DIR"

    # Verify benchmark binaries exist
    local missing=false
    if [[ "$SKIP_SOCKET" == false ]]; then
        for bin in bench_market_single bench_order_rtt; do
            if [[ ! -x "$BUILD_DIR/$bin" ]]; then
                log_error "Missing binary: $BUILD_DIR/$bin"
                missing=true
            fi
        done
    fi
    if [[ "$SKIP_DPDK" == false ]]; then
        for bin in bench_market_single_dpdk bench_order_rtt_dpdk; do
            if [[ ! -x "$BUILD_DIR/$bin" ]]; then
                log_error "Missing binary: $BUILD_DIR/$bin"
                missing=true
            fi
        done
        # dpdk-devbind.py must be available
        if ! command -v dpdk-devbind.py &>/dev/null; then
            log_error "dpdk-devbind.py not found in PATH"
            missing=true
        fi
    fi
    if [[ "$missing" == true ]]; then
        exit 1
    fi

    # Auto-detect NIC-B PCI address and current driver
    NIC_B_PCI=$(ethtool -i "$NIC_B" 2>/dev/null | awk '/bus-info:/ {print $2}')
    if [[ -z "$NIC_B_PCI" ]]; then
        log_error "Cannot detect PCI address for NIC-B '$NIC_B'"
        exit 1
    fi
    NIC_B_ORIG_DRIVER=$(ethtool -i "$NIC_B" 2>/dev/null | awk '/driver:/ {print $2}')
    if [[ -z "$NIC_B_ORIG_DRIVER" ]]; then
        log_error "Cannot detect current driver for NIC-B '$NIC_B'"
        exit 1
    fi
    log_info "NIC-B PCI: $NIC_B_PCI, driver: $NIC_B_ORIG_DRIVER"
}

# ── Cleanup: restore NIC-B on failure/interrupt ──────────────────────────────
cleanup() {
    local exit_code=$?
    log_info "Cleaning up..."

    # Restore NIC-B to kernel driver if we switched it to DPDK
    if [[ "$NIC_B_BOUND_DPDK" == true ]]; then
        log_info "Restoring NIC-B ($NIC_B_PCI) to kernel driver ($NIC_B_ORIG_DRIVER)..."
        dpdk-devbind.py -b "$NIC_B_ORIG_DRIVER" "$NIC_B_PCI" 2>/dev/null || true
        # Wait for the interface to reappear
        for i in $(seq 1 10); do
            if ip link show "$NIC_B" &>/dev/null; then
                break
            fi
            sleep 0.5
        done
        NIC_B_BOUND_DPDK=false
    fi

    # Remove IP addresses we configured
    if [[ "$IP_B_CONFIGURED" == true ]]; then
        ip addr del "${IP_B}${SUBNET}" dev "$NIC_B" 2>/dev/null || true
        IP_B_CONFIGURED=false
    fi
    if [[ "$IP_A_CONFIGURED" == true ]]; then
        ip addr del "${IP_A}${SUBNET}" dev "$NIC_A" 2>/dev/null || true
        IP_A_CONFIGURED=false
    fi

    if [[ $exit_code -ne 0 ]]; then
        log_error "bench_setup.sh failed (exit code $exit_code)"
    else
        log_info "Cleanup complete"
    fi
}
trap cleanup EXIT INT TERM

# ── Phase 1: Configure network ──────────────────────────────────────────────
configure_network() {
    log_phase 1 "Configure Network"

    # Bring interfaces up
    ip link set "$NIC_A" up
    ip link set "$NIC_B" up
    log_info "Interfaces $NIC_A and $NIC_B are up"

    # Assign IPs (flush existing first to be idempotent)
    ip addr flush dev "$NIC_A" 2>/dev/null || true
    ip addr add "${IP_A}${SUBNET}" dev "$NIC_A"
    IP_A_CONFIGURED=true
    log_info "NIC-A ($NIC_A): $IP_A$SUBNET"

    ip addr flush dev "$NIC_B" 2>/dev/null || true
    ip addr add "${IP_B}${SUBNET}" dev "$NIC_B"
    IP_B_CONFIGURED=true
    log_info "NIC-B ($NIC_B): $IP_B$SUBNET"

    # Disable ARP offload / verify connectivity
    # Wait for link and ARP to settle
    sleep 1
    if ! ping -c 1 -W 2 -I "$NIC_B" "$IP_A" &>/dev/null; then
        log_warn "Cannot ping $IP_A from NIC-B -- check cable connection"
        log_warn "Continuing anyway (mock server is in-process, not cross-NIC)"
    else
        log_info "Connectivity verified: NIC-B -> NIC-A ($IP_A)"
    fi
}

# ── Phase 2: Socket benchmarks ──────────────────────────────────────────────
run_socket_benchmarks() {
    if [[ "$SKIP_SOCKET" == true ]]; then
        log_info "Skipping socket benchmarks (--skip-socket)"
        return
    fi

    log_phase 2 "Socket Benchmarks (kernel TCP)"

    log_info "Running bench_market_single (socket)..."
    "$BUILD_DIR/bench_market_single" \
        --server-ip "$IP_A" \
        --port "$MOCK_PORT" \
        --symbols "$SYMBOLS" \
        --duration "$DURATION" \
        2>&1 | tee /dev/stderr
    echo

    log_info "Running bench_order_rtt (socket)..."
    "$BUILD_DIR/bench_order_rtt" \
        --server-ip "$IP_A" \
        --port "$MOCK_PORT" \
        --symbols "$SYMBOLS" \
        --duration "$DURATION" \
        2>&1 | tee /dev/stderr
    echo
}

# ── Phase 3: Switch NIC-B to DPDK ───────────────────────────────────────────
bind_nicb_dpdk() {
    if [[ "$SKIP_DPDK" == true ]]; then
        return
    fi

    log_phase 3 "Bind NIC-B to DPDK (vfio-pci)"

    # Remove IP before unbinding
    ip addr del "${IP_B}${SUBNET}" dev "$NIC_B" 2>/dev/null || true
    IP_B_CONFIGURED=false

    # Load vfio-pci module if needed
    if ! lsmod | grep -q vfio_pci; then
        log_info "Loading vfio-pci module..."
        modprobe vfio-pci 2>/dev/null || {
            log_error "Failed to load vfio-pci module"
            exit 1
        }
    fi

    # Enable unsafe IOMMU mode if no IOMMU is available
    # (common on cloud instances without hardware IOMMU)
    if [[ ! -d /sys/kernel/iommu_groups/0 ]]; then
        log_warn "No IOMMU detected, enabling vfio unsafe no-iommu mode"
        echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || true
    fi

    log_info "Unbinding NIC-B ($NIC_B_PCI) from $NIC_B_ORIG_DRIVER..."
    dpdk-devbind.py -u "$NIC_B_PCI"

    log_info "Binding NIC-B ($NIC_B_PCI) to vfio-pci..."
    dpdk-devbind.py -b vfio-pci "$NIC_B_PCI"
    NIC_B_BOUND_DPDK=true

    log_info "NIC-B bound to vfio-pci"
    dpdk-devbind.py --status-dev net | grep "$NIC_B_PCI" || true
}

# ── Phase 4: DPDK benchmarks ────────────────────────────────────────────────
run_dpdk_benchmarks() {
    if [[ "$SKIP_DPDK" == true ]]; then
        log_info "Skipping DPDK benchmarks (--skip-dpdk)"
        return
    fi

    log_phase 4 "DPDK Benchmarks (kernel-bypass)"

    # Build EAL args: bind to NIC-B PCI address
    local eal
    eal="-a $NIC_B_PCI"
    if [[ -n "$EAL_ARGS" ]]; then
        eal="$eal $EAL_ARGS"
    fi

    log_info "Running bench_market_single_dpdk (DPDK)..."
    # shellcheck disable=SC2086
    "$BUILD_DIR/bench_market_single_dpdk" $eal -- \
        --server-ip "$IP_A" \
        --port "$MOCK_PORT" \
        --local-ip "$IP_B" \
        --gateway-ip "$IP_A" \
        --symbols "$SYMBOLS" \
        --duration "$DURATION" \
        2>&1 | tee /dev/stderr
    echo

    log_info "Running bench_order_rtt_dpdk (DPDK)..."
    # shellcheck disable=SC2086
    "$BUILD_DIR/bench_order_rtt_dpdk" $eal -- \
        --server-ip "$IP_A" \
        --port "$MOCK_PORT" \
        --local-ip "$IP_B" \
        --gateway-ip "$IP_A" \
        --symbols "$SYMBOLS" \
        --duration "$DURATION" \
        2>&1 | tee /dev/stderr
    echo
}

# ── Phase 5: Restore NIC-B ──────────────────────────────────────────────────
restore_nicb() {
    if [[ "$NIC_B_BOUND_DPDK" == false ]]; then
        return
    fi

    log_phase 5 "Restore NIC-B to Kernel Driver"

    log_info "Unbinding NIC-B ($NIC_B_PCI) from vfio-pci..."
    dpdk-devbind.py -b "$NIC_B_ORIG_DRIVER" "$NIC_B_PCI"
    NIC_B_BOUND_DPDK=false

    # Wait for the interface to reappear
    for i in $(seq 1 10); do
        if ip link show "$NIC_B" &>/dev/null; then
            break
        fi
        sleep 0.5
    done

    # Restore IP
    ip addr add "${IP_B}${SUBNET}" dev "$NIC_B" 2>/dev/null || true
    IP_B_CONFIGURED=true
    ip link set "$NIC_B" up

    log_info "NIC-B ($NIC_B) restored to $NIC_B_ORIG_DRIVER with $IP_B$SUBNET"
}

# ── Main ─────────────────────────────────────────────────────────────────────
main() {
    parse_args "$@"
    preflight

    log_info "Benchmark configuration:"
    log_info "  NIC-A:     $NIC_A ($IP_A)"
    log_info "  NIC-B:     $NIC_B ($IP_B) [PCI: $NIC_B_PCI, driver: $NIC_B_ORIG_DRIVER]"
    log_info "  Duration:  ${DURATION}s"
    log_info "  Symbols:   $SYMBOLS"
    log_info "  Build dir: $BUILD_DIR"

    configure_network
    run_socket_benchmarks
    bind_nicb_dpdk
    run_dpdk_benchmarks
    restore_nicb

    log_info "All benchmarks complete"
}

main "$@"
