#!/usr/bin/env bash
# bench_latency.sh — Fair Socket vs DPDK latency benchmark on dual-NIC setup.
#
# Socket bench runs in a network namespace (NIC-B) to prevent kernel
# loopback optimization. DPDK bench uses NIC-B via PMD. Both paths
# go through the same physical network: NIC-B → network → NIC-A.
#
# Prerequisites:
#   - Two NICs: NIC-A (mock server, kernel) and NIC-B (bench client)
#   - Build: xmake build bench_mock_server bench_market bench_order_rtt bench_market_dpdk bench_order_rtt_dpdk
#   - Root or sudo (namespace + DPDK NIC binding)
#
# Usage:
#   sudo ./scripts/bench_latency.sh --nic-a ens34 --nic-b ens35 \
#       --server-ip 172.31.21.173 --gateway-ip 172.31.16.1

set -euo pipefail

# ── Color & format ──────────────────────────────────────────────────────────
if [[ -t 1 ]] && [[ -z "${NO_COLOR:-}" ]]; then
    RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
    BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
else
    RED='' YELLOW='' GREEN='' BLUE='' CYAN='' BOLD='' NC=''
fi

log_info()  { echo -e "${GREEN}✓${NC}  $*"; }
log_warn()  { echo -e "${YELLOW}⚠${NC}  $*"; }
log_error() { echo -e "${RED}✗${NC}  ${BOLD}$*${NC}" >&2; }
log_phase() { echo -e "\n${BOLD}${BLUE}▶ Phase $1: $2${NC}\n"; }
log_verb()  { [[ "$VERBOSE" == true ]] && echo -e "${CYAN}ℹ${NC}  $*" || true; }
separator() { echo -e "${BOLD}────────────────────────────────────────${NC}"; }

# Save original args for rerun hint
ORIGINAL_ARGS=("$@")

die() {
    log_error "$1"
    [[ -n "${2:-}" ]] && echo -e "  ${2}" >&2
    echo -e "\n  Rerun: sudo $0 ${ORIGINAL_ARGS[*]:-}" >&2
    exit 1
}

# ── Locate project & scripts ───────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── Defaults (override via CLI args or env vars) ──────────────────────────
NIC_A="${BENCH_NIC_A:-}"
NIC_B="${BENCH_NIC_B:-}"
SERVER_IP="${BENCH_SERVER_IP:-}"
LOCAL_IP="${BENCH_LOCAL_IP:-}"
GATEWAY_IP="${BENCH_GATEWAY_IP:-}"
DURATION="${BENCH_DURATION:-10}"
SYMBOLS="${BENCH_SYMBOLS:-BTCUSDT,ETHUSDT,SOLUSDT}"
TICK_US="${BENCH_TICK_US:-100}"
POLL_CPU="${BENCH_POLL_CPU:-2}"
MOCK_CPU="${BENCH_MOCK_CPU:-4}"
EAL_CORES="${BENCH_EAL_CORES:-0,1}"
BUILD_DIR="${BENCH_BUILD_DIR:-}"
SKIP_SOCKET=false
SKIP_DPDK=false
DRY_RUN=false
VERBOSE=false

# ── Usage ───────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
${BOLD}bench_latency.sh${NC} — Fair Socket vs DPDK latency benchmark

${BOLD}Usage${NC}: sudo ./scripts/bench_latency.sh [OPTIONS]

${BOLD}Required${NC}:
  --nic-a IFACE       NIC-A interface (mock server side, e.g. ens34)
  --nic-b IFACE       NIC-B interface (bench client side, e.g. ens35)
  --server-ip IP      NIC-A IP address (mock server binds here)
  --gateway-ip IP     Network gateway for NIC-B routing (e.g. 172.31.16.1)

${BOLD}Optional${NC}:
  --local-ip IP       NIC-B IP (auto-detected from NIC-B if omitted)
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
  -v, --verbose       Verbose output (explains each step)
  -h, --help          Show this help

${BOLD}Environment variables${NC} (override defaults):
  BENCH_NIC_A, BENCH_NIC_B, BENCH_SERVER_IP, BENCH_GATEWAY_IP,
  BENCH_LOCAL_IP, BENCH_DURATION, BENCH_SYMBOLS, BENCH_TICK_US,
  BENCH_POLL_CPU, BENCH_MOCK_CPU, BENCH_EAL_CORES, BENCH_BUILD_DIR

${BOLD}Examples${NC}:
  # AWS EC2 with two ENA NICs:
  sudo ./scripts/bench_latency.sh \\
      --nic-a ens34 --nic-b ens35 \\
      --server-ip 172.31.21.173 --gateway-ip 172.31.16.1

  # Socket only (skip DPDK):
  sudo ./scripts/bench_latency.sh \\
      --nic-a ens34 --nic-b ens35 \\
      --server-ip 172.31.21.173 --gateway-ip 172.31.16.1 \\
      --skip-dpdk

  # Dry run (see what would happen):
  sudo ./scripts/bench_latency.sh \\
      --nic-a ens34 --nic-b ens35 \\
      --server-ip 172.31.21.173 --gateway-ip 172.31.16.1 \\
      --dry-run
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
            *) die "Unknown option: $1" "Run with --help for usage" ;;
        esac
    done

    # Validate required args
    [[ -z "$NIC_A" ]]      && die "--nic-a is required" "NIC-A interface for mock server (e.g. --nic-a ens34)"
    [[ -z "$NIC_B" ]]      && die "--nic-b is required" "NIC-B interface for bench client (e.g. --nic-b ens35)"
    [[ -z "$SERVER_IP" ]]  && die "--server-ip is required" "NIC-A IP where mock server binds (e.g. --server-ip 172.31.21.173)"
    [[ -z "$GATEWAY_IP" ]] && die "--gateway-ip is required" "Network gateway for NIC-B routing (e.g. --gateway-ip 172.31.16.1)"

    # Auto-detect LOCAL_IP from NIC-B
    if [[ -z "$LOCAL_IP" ]]; then
        LOCAL_IP=$(ip -4 addr show "$NIC_B" 2>/dev/null | grep -oE 'inet [0-9.]+' | awk '{print $2}' || true)
        [[ -z "$LOCAL_IP" ]] && die "Cannot auto-detect NIC-B IP" "Specify --local-ip explicitly"
    fi

    # Detect prefix length for namespace IP configuration
    LOCAL_PREFIX=$(ip -4 addr show "$NIC_B" 2>/dev/null | grep -oE 'inet [0-9./]+' | head -1 | awk '{print $2}' | grep -oE '/[0-9]+' || true)
    LOCAL_PREFIX="${LOCAL_PREFIX:-/20}"
    LOCAL_CIDR="${LOCAL_IP}${LOCAL_PREFIX}"

    # LD_LIBRARY_PATH for GCC 14 libstdc++ (bench binaries need it)
    if [[ -f /usr/lib/gcc/aarch64-amazon-linux/14/libstdc++.so ]]; then
        export LD_LIBRARY_PATH="/usr/lib/gcc/aarch64-amazon-linux/14:${LD_LIBRARY_PATH:-}"
    fi
}

# ── Preflight checks ───────────────────────────────────────────────────────
preflight() {
    log_phase 0 "Preflight Checks"

    # Root
    [[ $EUID -ne 0 ]] && die "Must run as root" "sudo $0 ${ORIGINAL_ARGS[*]:-}"
    log_info "Root privileges"

    # NIC existence
    ip link show "$NIC_A" &>/dev/null || die "NIC-A '$NIC_A' not found" "Check: ip link show"
    ip link show "$NIC_B" &>/dev/null || die "NIC-B '$NIC_B' not found" "Check: ip link show"
    log_info "NICs: $NIC_A (server) + $NIC_B (client)"

    # SSH safety: warn if NIC-B carries the default route (likely SSH)
    local default_dev
    default_dev=$(ip route show default | awk '{print $5}' | head -1)
    if [[ "$default_dev" == "$NIC_B" ]]; then
        log_warn "NIC-B ($NIC_B) carries the default route — moving it to a namespace"
        log_warn "will disconnect SSH if you're connected through this NIC!"
        log_warn "Make sure SSH goes through NIC-A ($NIC_A) before proceeding."
        if [[ "$DRY_RUN" == false ]]; then
            printf "  Continue? [y/N] "
            read -r -t 15 ans || ans=""
            case "$ans" in [yY]*) ;; *) echo "Cancelled."; exit 0 ;; esac
        fi
    fi

    # Build directory
    if [[ -z "$BUILD_DIR" ]]; then
        for candidate in \
            "$PROJECT_DIR/build/linux/arm64/release" \
            "$PROJECT_DIR/build/linux/aarch64/release" \
            "$PROJECT_DIR/build/linux/x86_64/release"; do
            [[ -d "$candidate" ]] && { BUILD_DIR="$candidate"; break; }
        done
        [[ -z "$BUILD_DIR" ]] && die "Cannot find build directory" "Specify --build-dir or run: xmake build"
    fi
    log_info "Build dir: $BUILD_DIR"

    # Required binaries
    local missing=false
    if [[ "$SKIP_SOCKET" == false ]]; then
        for bin in bench_mock_server bench_market bench_order_rtt; do
            [[ -x "$BUILD_DIR/$bin" ]] || { log_error "Missing: $BUILD_DIR/$bin"; missing=true; }
        done
    fi
    if [[ "$SKIP_DPDK" == false ]]; then
        for bin in bench_market_dpdk bench_order_rtt_dpdk; do
            [[ -x "$BUILD_DIR/$bin" ]] || { log_error "Missing: $BUILD_DIR/$bin"; missing=true; }
        done
        command -v dpdk-devbind.py &>/dev/null || { log_error "dpdk-devbind.py not in PATH"; missing=true; }
        [[ -x "$SCRIPT_DIR/dpdk-setup.sh" ]]   || { log_error "Missing: $SCRIPT_DIR/dpdk-setup.sh"; missing=true; }
        [[ -x "$SCRIPT_DIR/dpdk-teardown.sh" ]] || { log_error "Missing: $SCRIPT_DIR/dpdk-teardown.sh"; missing=true; }
    fi
    [[ "$missing" == true ]] && die "Missing required files" \
        "Build: xmake build bench_mock_server bench_market bench_order_rtt bench_market_dpdk bench_order_rtt_dpdk"
    log_info "All binaries found"

    # Connectivity
    log_verb "Pinging $SERVER_IP from $NIC_B to verify network path..."
    if ping -c 1 -W 1 -I "$NIC_B" "$SERVER_IP" &>/dev/null; then
        log_info "Connectivity: $NIC_B → $SERVER_IP OK"
    else
        log_warn "Cannot ping $SERVER_IP from $NIC_B"
        log_warn "For AWS EC2: disable source/dest check on both ENIs"
    fi

    log_verb "Socket bench will run in namespace (NIC-B=$NIC_B) to prevent kernel loopback"
    log_verb "DPDK bench will use NIC-B via PMD with built-in mock server on NIC-A"
}

# ── Socket benchmarks (namespace isolation) ────────────────────────────────
run_socket_benchmarks() {
    [[ "$SKIP_SOCKET" == true ]] && return

    log_phase 1 "Socket Benchmarks (namespace isolation)"
    log_verb "Why namespace: kernel's local routing table (priority 0) intercepts"
    log_verb "traffic between two local IPs and delivers via loopback. Namespace"
    log_verb "puts NIC-B in a separate network stack, forcing real NIC traffic."

    local common_args=(
        --server-ip "$SERVER_IP"
        --symbols "$SYMBOLS"
        --duration "$DURATION"
        --poll-cpu "$POLL_CPU"
    )

    if [[ "$DRY_RUN" == true ]]; then
        echo "[DRY RUN] ip netns add bench_ns; ip link set $NIC_B netns bench_ns"
        echo "[DRY RUN] bench_mock_server --bind-ip $SERVER_IP --port 9999"
        echo "[DRY RUN] ip netns exec bench_ns bench_market ${common_args[*]} --port 9999"
        echo "[DRY RUN] bench_mock_server --bind-ip $SERVER_IP --port 9998 --order-mode"
        echo "[DRY RUN] ip netns exec bench_ns bench_order_rtt ${common_args[*]} --port 9998"
        echo "[DRY RUN] restore $NIC_B to host namespace"
        return
    fi

    # Create namespace and move NIC-B
    log_info "Creating namespace bench_ns, moving $NIC_B..."
    ip netns add bench_ns 2>/dev/null || true
    ip link set "$NIC_B" netns bench_ns
    ip netns exec bench_ns ip addr add "$LOCAL_CIDR" dev "$NIC_B"
    ip netns exec bench_ns ip link set "$NIC_B" up
    ip netns exec bench_ns ip route add default via "$GATEWAY_IP" dev "$NIC_B"

    # Market data bench
    log_info "Starting mock server for market data (port 9999)..."
    "$BUILD_DIR/bench_mock_server" \
        --bind-ip "$SERVER_IP" --port 9999 \
        --symbols "$SYMBOLS" --tick-us "$TICK_US" --cpu "$MOCK_CPU" &
    local mock_pid=$!
    sleep 1

    log_info "Running bench_market (socket)..."
    ip netns exec bench_ns \
        "$BUILD_DIR/bench_market" "${common_args[@]}" --port 9999 2>&1
    echo
    kill "$mock_pid" 2>/dev/null || true; wait "$mock_pid" 2>/dev/null || true

    # Order RTT bench
    log_info "Starting mock server for orders (port 9998)..."
    "$BUILD_DIR/bench_mock_server" \
        --bind-ip "$SERVER_IP" --port 9998 \
        --symbols "$SYMBOLS" --tick-us "$TICK_US" --cpu "$MOCK_CPU" --order-mode &
    mock_pid=$!
    sleep 1

    log_info "Running bench_order_rtt (socket)..."
    ip netns exec bench_ns \
        "$BUILD_DIR/bench_order_rtt" "${common_args[@]}" --port 9998 --order-interval-us 1000 2>&1
    echo
    kill "$mock_pid" 2>/dev/null || true; wait "$mock_pid" 2>/dev/null || true

    # Restore NIC-B
    log_info "Restoring $NIC_B to host namespace..."
    ip netns exec bench_ns ip link set "$NIC_B" netns 1
    ip netns del bench_ns 2>/dev/null || true
    ip link set "$NIC_B" up
    ip addr add "$LOCAL_CIDR" dev "$NIC_B" 2>/dev/null || true
    log_info "Socket benchmarks complete, $NIC_B restored"
}

# ── DPDK benchmarks ────────────────────────────────────────────────────────
run_dpdk_benchmarks() {
    [[ "$SKIP_DPDK" == true ]] && return

    local nic_b_pci
    nic_b_pci=$(basename "$(readlink -f "/sys/class/net/$NIC_B/device" 2>/dev/null)" 2>/dev/null || true)
    [[ -z "$nic_b_pci" || "$nic_b_pci" == "device" ]] && \
        die "Cannot detect PCI address for NIC-B '$NIC_B'" "Check: ls /sys/class/net/$NIC_B/device"
    log_info "NIC-B PCI: $nic_b_pci"

    log_phase 2 "Bind NIC-B to DPDK"
    log_verb "DPDK bench uses built-in mock server (kernel TCP on NIC-A)."
    log_verb "NIC-B is bound to DPDK PMD for kernel-bypass receive."

    if [[ "$DRY_RUN" == true ]]; then
        echo "[DRY RUN] $SCRIPT_DIR/dpdk-setup.sh (bind $nic_b_pci to vfio-pci)"
        echo "[DRY RUN] bench_market_dpdk -a $nic_b_pci -l $EAL_CORES -- ..."
        echo "[DRY RUN] bench_order_rtt_dpdk -a $nic_b_pci -l $EAL_CORES -- ..."
        echo "[DRY RUN] $SCRIPT_DIR/dpdk-teardown.sh (restore $NIC_B)"
        return
    fi

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

# ── Cleanup on exit ─────────────────────────────────────────────────────────
cleanup_on_exit() {
    local exit_code=$?
    # Restore NIC-B from namespace if interrupted mid-run
    if ip netns list 2>/dev/null | grep -q bench_ns; then
        log_warn "Cleaning up: restoring $NIC_B from namespace..."
        ip netns exec bench_ns ip link set "$NIC_B" netns 1 2>/dev/null || true
        ip netns del bench_ns 2>/dev/null || true
        ip link set "$NIC_B" up 2>/dev/null || true
        ip addr add "${LOCAL_CIDR:-$LOCAL_IP/20}" dev "$NIC_B" 2>/dev/null || true
    fi
    # Kill lingering mock servers
    pkill -f bench_mock_server 2>/dev/null || true
    return "$exit_code"
}

# ── Main ────────────────────────────────────────────────────────────────────
main() {
    parse_args "$@"
    trap cleanup_on_exit EXIT

    separator
    echo -e "${BOLD}bench_latency.sh — Socket vs DPDK Latency Benchmark${NC}"
    echo -e "  NIC-A (server):  $NIC_A ($SERVER_IP)"
    echo -e "  NIC-B (client):  $NIC_B ($LOCAL_IP)"
    echo -e "  Gateway:         $GATEWAY_IP"
    echo -e "  Duration:        ${DURATION}s"
    echo -e "  Symbols:         $SYMBOLS"
    echo -e "  Cores:           poll=$POLL_CPU mock=$MOCK_CPU eal=$EAL_CORES"
    echo -e "  Kernel:          $(uname -r)"
    echo -e "  Arch:            $(uname -m)"
    separator

    preflight
    run_socket_benchmarks
    run_dpdk_benchmarks

    separator
    echo -e "${BOLD}${GREEN}✓ All benchmarks complete${NC}"
    echo -e ""
    echo -e "${BOLD}💡 Tips:${NC}"
    echo -e "  • Re-run socket only:  add --skip-dpdk"
    echo -e "  • Re-run DPDK only:    add --skip-socket"
    echo -e "  • Explain each step:   add --verbose"
    separator
}

main "$@"
