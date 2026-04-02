#!/usr/bin/env bash
# bench_latency.sh — Run latency benchmarks (socket + DPDK) on dual-NIC setup.
#
# Orchestrates: dpdk-setup.sh → socket bench → DPDK bench → dpdk-teardown.sh
# All benchmarks use in-process mock WS server + DirectTransport (no queues).
#
# Prerequisites:
#   - Two NICs: NIC-A (mock server, kernel) and NIC-B (bench client)
#   - Build: xmake build bench_market bench_order_rtt bench_market_dpdk bench_order_rtt_dpdk
#   - Root or sudo (for SO_BINDTODEVICE + DPDK NIC binding)
#
# Usage:
#   sudo ./scripts/bench_latency.sh --nic-a ens34 --nic-b ens35 --server-ip 172.31.21.173
#
# The script:
#   1. Runs socket benchmarks (NIC-B via SO_BINDTODEVICE, no loopback)
#   2. Binds NIC-B to DPDK via dpdk-setup.sh
#   3. Runs DPDK benchmarks
#   4. Restores NIC-B via dpdk-teardown.sh

set -euo pipefail

# ── Color & format ──────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
    BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
else
    RED='' YELLOW='' GREEN='' BLUE='' CYAN='' BOLD='' NC=''
fi

log_info()  { echo -e "${GREEN}✓${NC}  $*"; }
log_warn()  { echo -e "${YELLOW}⚠${NC}  $*"; }
log_error() { echo -e "${RED}✗${NC}  ${BOLD}$*${NC}" >&2; }
log_phase() { echo -e "\n${BOLD}${BLUE}▶ Phase $1: $2${NC}\n"; }
separator() { echo -e "\n${BOLD}────────────────────────────────────────${NC}"; }

die() {
    log_error "$1"
    [[ -n "${2:-}" ]] && echo -e "\n${2}" >&2
    echo -e "\n  Rerun: sudo $0 $*" >&2
    exit 1
}

# ── Locate project & scripts ───────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Defaults ────────────────────────────────────────────────────────────────
NIC_A=""
NIC_B=""
SERVER_IP=""            # required: NIC-A IP
LOCAL_IP=""             # for DPDK: NIC-B IP (auto-detected if not specified)
GATEWAY_IP=""           # for DPDK: gateway (defaults to SERVER_IP)
DURATION=10
SYMBOLS="BTCUSDT,ETHUSDT,SOLUSDT"
TICK_US=100
POLL_CPU=2
MOCK_CPU=4
EAL_CORES="0,1"
BUILD_DIR=""
SKIP_SOCKET=false
SKIP_DPDK=false
DRY_RUN=false
VERBOSE=false

# ── Usage ───────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
${BOLD}Usage${NC}: sudo ./scripts/bench_latency.sh [OPTIONS]

${BOLD}Required${NC}:
  --nic-a IFACE       NIC-A interface (mock server side, e.g. ens34)
  --nic-b IFACE       NIC-B interface (bench client side, e.g. ens35)
  --server-ip IP      NIC-A IP address (mock server binds here)

${BOLD}Optional${NC}:
  --local-ip IP       NIC-B IP for DPDK (auto-detected from NIC-B if omitted)
  --gateway-ip IP     Gateway IP for DPDK ARP (defaults to server-ip)
  --duration SEC      Benchmark duration in seconds (default: ${DURATION})
  --symbols SYM,...   Comma-separated symbols (default: ${SYMBOLS})
  --tick-us US        Mock server tick interval in microseconds (default: ${TICK_US})
  --poll-cpu N        CPU core for bench poll loop (default: ${POLL_CPU})
  --mock-cpu N        CPU core for mock server thread (default: ${MOCK_CPU})
  --eal-cores LIST    DPDK EAL lcore list (default: ${EAL_CORES})
  --build-dir DIR     Build directory (auto-detected if omitted)
  --skip-socket       Skip socket benchmarks
  --skip-dpdk         Skip DPDK benchmarks
  --dry-run           Show what would be done without executing
  -v, --verbose       Verbose output
  -h, --help          Show this help

${BOLD}Example${NC}:
  # AWS EC2 with two ENA NICs:
  sudo ./scripts/bench_latency.sh \\
      --nic-a ens34 --nic-b ens35 \\
      --server-ip 172.31.21.173 \\
      --duration 10

  # With custom cores and symbols:
  sudo ./scripts/bench_latency.sh \\
      --nic-a eth0 --nic-b eth1 \\
      --server-ip 10.0.0.1 \\
      --poll-cpu 4 --mock-cpu 6 --eal-cores 0,1,2,3 \\
      --symbols BTCUSDT,ETHUSDT
EOF
    exit 0
}

# ── Argument parsing ────────────────────────────────────────────────────────
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --nic-a)       NIC_A="$2"; shift 2 ;;
            --nic-b)       NIC_B="$2"; shift 2 ;;
            --server-ip)   SERVER_IP="$2"; shift 2 ;;
            --local-ip)    LOCAL_IP="$2"; shift 2 ;;
            --gateway-ip)  GATEWAY_IP="$2"; shift 2 ;;
            --duration)    DURATION="$2"; shift 2 ;;
            --symbols)     SYMBOLS="$2"; shift 2 ;;
            --tick-us)     TICK_US="$2"; shift 2 ;;
            --poll-cpu)    POLL_CPU="$2"; shift 2 ;;
            --mock-cpu)    MOCK_CPU="$2"; shift 2 ;;
            --eal-cores)   EAL_CORES="$2"; shift 2 ;;
            --build-dir)   BUILD_DIR="$2"; shift 2 ;;
            --skip-socket) SKIP_SOCKET=true; shift ;;
            --skip-dpdk)   SKIP_DPDK=true; shift ;;
            --dry-run)     DRY_RUN=true; shift ;;
            -v|--verbose)  VERBOSE=true; shift ;;
            -h|--help)     usage ;;
            *) die "Unknown option: $1" "  Run with --help for usage" ;;
        esac
    done

    # Validate required args
    [[ -z "$NIC_A" ]]     && die "--nic-a is required (NIC-A interface for mock server)"
    [[ -z "$NIC_B" ]]     && die "--nic-b is required (NIC-B interface for bench client)"
    [[ -z "$SERVER_IP" ]] && die "--server-ip is required (NIC-A IP where mock server binds)"

    # Auto-detect LOCAL_IP from NIC-B if not specified
    if [[ -z "$LOCAL_IP" ]]; then
        LOCAL_IP=$(ip -4 addr show "$NIC_B" 2>/dev/null | grep -oE 'inet [0-9.]+' | awk '{print $2}' || true)
        [[ -z "$LOCAL_IP" ]] && die "Cannot auto-detect NIC-B IP. Specify --local-ip"
    fi

    # Default gateway to server IP
    GATEWAY_IP="${GATEWAY_IP:-$SERVER_IP}"
}

# ── Preflight checks ───────────────────────────────────────────────────────
preflight() {
    log_phase 0 "Preflight Checks"

    # Root check
    [[ $EUID -ne 0 ]] && die "Must run as root (sudo)" "  sudo $0 $*"
    log_info "Root privileges"

    # NIC existence
    ip link show "$NIC_A" &>/dev/null || die "NIC-A '$NIC_A' not found" "  Check: ip link show"
    ip link show "$NIC_B" &>/dev/null || die "NIC-B '$NIC_B' not found" "  Check: ip link show"
    log_info "NICs: $NIC_A (server) + $NIC_B (client)"

    # Auto-detect build directory
    if [[ -z "$BUILD_DIR" ]]; then
        for candidate in \
            "$PROJECT_DIR/build/linux/arm64/release" \
            "$PROJECT_DIR/build/linux/aarch64/release" \
            "$PROJECT_DIR/build/linux/x86_64/release"; do
            if [[ -d "$candidate" ]]; then
                BUILD_DIR="$candidate"
                break
            fi
        done
        [[ -z "$BUILD_DIR" ]] && die "Cannot find build directory" "  Specify --build-dir or run: xmake build"
    fi
    log_info "Build dir: $BUILD_DIR"

    # Check required binaries
    local missing=false
    if [[ "$SKIP_SOCKET" == false ]]; then
        for bin in bench_market bench_order_rtt; do
            [[ -x "$BUILD_DIR/$bin" ]] || { log_error "Missing: $BUILD_DIR/$bin"; missing=true; }
        done
    fi
    if [[ "$SKIP_DPDK" == false ]]; then
        for bin in bench_market_dpdk bench_order_rtt_dpdk; do
            [[ -x "$BUILD_DIR/$bin" ]] || { log_error "Missing: $BUILD_DIR/$bin"; missing=true; }
        done
        command -v dpdk-devbind.py &>/dev/null || { log_error "dpdk-devbind.py not in PATH"; missing=true; }
        [[ -x "$SCRIPT_DIR/dpdk-setup.sh" ]]    || { log_error "Missing: $SCRIPT_DIR/dpdk-setup.sh"; missing=true; }
        [[ -x "$SCRIPT_DIR/dpdk-teardown.sh" ]]  || { log_error "Missing: $SCRIPT_DIR/dpdk-teardown.sh"; missing=true; }
    fi
    [[ "$missing" == true ]] && die "Missing required files" "  Build: xmake build bench_market bench_order_rtt bench_market_dpdk bench_order_rtt_dpdk"
    log_info "All binaries found"

    # Connectivity check
    if ! ping -c 1 -W 1 -I "$NIC_B" "$SERVER_IP" &>/dev/null; then
        log_warn "Cannot ping $SERVER_IP from $NIC_B — connectivity may fail"
        log_warn "For AWS EC2: disable source/dest check on both ENIs"
    else
        log_info "Connectivity: $NIC_B → $SERVER_IP OK"
    fi
}

# ── Socket benchmarks ──────────────────────────────────────────────────────
run_socket_benchmarks() {
    [[ "$SKIP_SOCKET" == true ]] && return

    log_phase 1 "Socket Benchmarks (kernel TCP + SO_BINDTODEVICE)"

    local common_args=(
        --server-ip "$SERVER_IP"
        --bind-dev "$NIC_B"
        --port 9999
        --symbols "$SYMBOLS"
        --duration "$DURATION"
        --tick-us "$TICK_US"
        --poll-cpu "$POLL_CPU"
        --mock-cpu "$MOCK_CPU"
    )

    if [[ "$DRY_RUN" == true ]]; then
        echo "[DRY RUN] $BUILD_DIR/bench_market ${common_args[*]}"
        echo "[DRY RUN] $BUILD_DIR/bench_order_rtt ${common_args[*]}"
        return
    fi

    log_info "Running bench_market (socket)..."
    "$BUILD_DIR/bench_market" "${common_args[@]}" 2>&1
    echo

    log_info "Running bench_order_rtt (socket)..."
    "$BUILD_DIR/bench_order_rtt" "${common_args[@]}" --order-interval-us 1000 2>&1
    echo
}

# ── DPDK benchmarks ────────────────────────────────────────────────────────
run_dpdk_benchmarks() {
    [[ "$SKIP_DPDK" == true ]] && return

    # Get NIC-B PCI address before binding to DPDK
    local nic_b_pci
    nic_b_pci=$(ethtool -i "$NIC_B" 2>/dev/null | awk '/bus-info:/ {print $2}')
    [[ -z "$nic_b_pci" ]] && die "Cannot detect PCI address for NIC-B '$NIC_B'"
    log_info "NIC-B PCI: $nic_b_pci"

    log_phase 2 "Bind NIC-B to DPDK"

    if [[ "$DRY_RUN" == true ]]; then
        echo "[DRY RUN] $SCRIPT_DIR/dpdk-setup.sh -y"
        echo "[DRY RUN] bench_market_dpdk / bench_order_rtt_dpdk"
        echo "[DRY RUN] $SCRIPT_DIR/dpdk-teardown.sh"
        return
    fi

    # Use dpdk-setup.sh to bind NIC-B (it handles hugepages, vfio, etc.)
    DPDK_PCI="$nic_b_pci" DPDK_IFACE="$NIC_B" \
        "$SCRIPT_DIR/dpdk-setup.sh" -y
    log_info "NIC-B bound to DPDK"

    log_phase 3 "DPDK Benchmarks (kernel-bypass)"

    local eal_args="-a $nic_b_pci -l $EAL_CORES"
    local common_args=(
        --server-ip "$SERVER_IP"
        --local-ip "$LOCAL_IP"
        --gateway-ip "$GATEWAY_IP"
        --port 9999
        --symbols "$SYMBOLS"
        --duration "$DURATION"
        --tick-us "$TICK_US"
        --poll-cpu "$POLL_CPU"
        --mock-cpu "$MOCK_CPU"
    )

    log_info "Running bench_market_dpdk..."
    # shellcheck disable=SC2086
    "$BUILD_DIR/bench_market_dpdk" $eal_args -- "${common_args[@]}" 2>&1
    echo

    log_info "Running bench_order_rtt_dpdk..."
    # shellcheck disable=SC2086
    "$BUILD_DIR/bench_order_rtt_dpdk" $eal_args -- \
        "${common_args[@]}" --order-interval-us 1000 2>&1
    echo

    log_phase 4 "Restore NIC-B"

    DPDK_PCI="$nic_b_pci" DPDK_IFACE="$NIC_B" \
        "$SCRIPT_DIR/dpdk-teardown.sh"
    log_info "NIC-B restored to kernel driver"
}

# ── Main ────────────────────────────────────────────────────────────────────
main() {
    parse_args "$@"

    separator
    echo -e "${BOLD}bench_latency.sh — Socket vs DPDK Latency Benchmark${NC}"
    echo -e "  NIC-A (server): $NIC_A ($SERVER_IP)"
    echo -e "  NIC-B (client): $NIC_B ($LOCAL_IP)"
    echo -e "  Duration:       ${DURATION}s"
    echo -e "  Symbols:        $SYMBOLS"
    echo -e "  Cores:          poll=$POLL_CPU mock=$MOCK_CPU eal=$EAL_CORES"
    separator

    preflight
    run_socket_benchmarks
    run_dpdk_benchmarks

    separator
    echo -e "${BOLD}${GREEN}✓ All benchmarks complete${NC}"
    separator
}

main "$@"
