#!/usr/bin/env bash
# bench_latency.sh — 6-scenario × kernel/DPDK × multi-payload latency benchmark.
#
# Scenarios:
#   1. tcp_echo  — Raw TCP echo (multi-payload)
#   2. udp_echo  — Raw UDP echo (multi-payload)
#   3. ws_echo   — WebSocket echo (multi-payload)
#   4. market_rx — WS market data RX (pipeline latency)
#   5. order_rtt — WS order submit + execution report RTT
#   6. udp_relay — UDP send+recv latency on a different port (echo, port 9996)
#
# Each scenario runs in two transports:
#   - kernel: bench client in network namespace, mock in host namespace
#   - dpdk:   bench client uses DPDK PMD on NIC-B, in-process mock on NIC-A
#
# Output:
#   - .bench/latency_results_${ts}.jsonl  (machine-readable, one record per leg)
#   - .bench/env_${ts}.txt                (environment metadata)
#   - stdout (human-readable spdlog output)
#
# Prerequisites:
#   - Two NICs: NIC-A (mock server, kernel) and NIC-B (bench client)
#   - Build: xmake build -- (script will list missing binaries)
#   - Root or sudo (network namespace + DPDK NIC binding)
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

# ── Defaults ───────────────────────────────────────────────────────────────
NIC_A="${BENCH_NIC_A:-}"
NIC_B="${BENCH_NIC_B:-}"
SERVER_IP="${BENCH_SERVER_IP:-}"
LOCAL_IP="${BENCH_LOCAL_IP:-}"
GATEWAY_IP="${BENCH_GATEWAY_IP:-}"
DURATION="${BENCH_DURATION:-10}"
WARMUP="${BENCH_WARMUP:-2}"
SYMBOLS="${BENCH_SYMBOLS:-BTCUSDT,ETHUSDT,SOLUSDT}"
TICK_US="${BENCH_TICK_US:-100}"
ORDER_INTERVAL_US="${BENCH_ORDER_INTERVAL_US:-1000}"
POLL_CPU="${BENCH_POLL_CPU:-2}"
MOCK_CPU="${BENCH_MOCK_CPU:-4}"
EAL_CORES="${BENCH_EAL_CORES:-0,1}"
BUILD_DIR="${BENCH_BUILD_DIR:-}"
OUTPUT_DIR="${BENCH_OUTPUT_DIR:-${PROJECT_DIR}/.bench}"
SCENARIOS="${BENCH_SCENARIOS:-all}"
TRANSPORTS="${BENCH_TRANSPORTS:-all}"
SKIP_KERNEL=false
SKIP_DPDK=false
DRY_RUN=false
VERBOSE=false

# ── Payload sweeps per scenario ────────────────────────────────────────────
TCP_PAYLOADS="64,128,256,512,1024,1460"
UDP_PAYLOADS="64,128,512,1024,1472"
WS_PAYLOADS="64,128,256,512,1024"

# ── Usage ──────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
${BOLD}bench_latency.sh${NC} — 6-scenario × kernel/DPDK × multi-payload latency bench

${BOLD}Usage${NC}: sudo ./scripts/bench_latency.sh [OPTIONS]

${BOLD}Required${NC}:
  --nic-a IFACE       NIC-A interface (mock server side)
  --nic-b IFACE       NIC-B interface (bench client side)
  --server-ip IP      NIC-A IP address (mock server binds here)
  --gateway-ip IP     Network gateway for NIC-B routing

${BOLD}Optional${NC}:
  --local-ip IP       NIC-B IP (auto-detected if omitted)
  --duration SEC      Measurement duration (default: ${DURATION})
  --warmup SEC        Warmup duration (default: ${WARMUP})
  --scenarios LIST    Comma-separated: tcp_echo,udp_echo,ws_echo,market_rx,order_rtt,udp_relay,all (default: all)
  --transports LIST   Comma-separated: kernel,dpdk,all (default: all)
  --output-dir DIR    Output directory (default: ${OUTPUT_DIR})
  --symbols SYM,...   Symbols for WS bench (default: ${SYMBOLS})
  --tick-us US        Mock tick interval (default: ${TICK_US})
  --order-interval-us US  Order send interval (default: ${ORDER_INTERVAL_US})
  --poll-cpu N        Bench client CPU (default: ${POLL_CPU})
  --mock-cpu N        Mock server CPU (default: ${MOCK_CPU})
  --eal-cores LIST    DPDK EAL lcore list (default: ${EAL_CORES})
  --skip-kernel       Skip kernel benchmarks
  --skip-dpdk         Skip DPDK benchmarks
  --dry-run           Show what would run without executing
  -v, --verbose       Verbose output
  -h, --help          Show this help

${BOLD}Examples${NC}:
  # Full sweep:
  sudo ./scripts/bench_latency.sh --nic-a ens34 --nic-b ens35 \\
      --server-ip 172.31.21.173 --gateway-ip 172.31.16.1

  # Only UDP echo, kernel only, 5s duration:
  sudo ./scripts/bench_latency.sh --nic-a ens34 --nic-b ens35 \\
      --server-ip 172.31.21.173 --gateway-ip 172.31.16.1 \\
      --scenarios udp_echo --transports kernel --duration 5

  # WS benchmarks only:
  sudo ./scripts/bench_latency.sh --nic-a ens34 --nic-b ens35 \\
      --server-ip 172.31.21.173 --gateway-ip 172.31.16.1 \\
      --scenarios ws_echo,market_rx,order_rtt
EOF
    exit 0
}

# ── Argument parsing ───────────────────────────────────────────────────────
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --nic-a)            NIC_A="$2"; shift 2 ;;
            --nic-b)            NIC_B="$2"; shift 2 ;;
            --server-ip)        SERVER_IP="$2"; shift 2 ;;
            --local-ip)         LOCAL_IP="$2"; shift 2 ;;
            --gateway-ip)       GATEWAY_IP="$2"; shift 2 ;;
            --duration)         DURATION="$2"; shift 2 ;;
            --warmup)           WARMUP="$2"; shift 2 ;;
            --scenarios)        SCENARIOS="$2"; shift 2 ;;
            --transports)       TRANSPORTS="$2"; shift 2 ;;
            --output-dir)       OUTPUT_DIR="$2"; shift 2 ;;
            --symbols)          SYMBOLS="$2"; shift 2 ;;
            --tick-us)          TICK_US="$2"; shift 2 ;;
            --order-interval-us) ORDER_INTERVAL_US="$2"; shift 2 ;;
            --poll-cpu)         POLL_CPU="$2"; shift 2 ;;
            --mock-cpu)         MOCK_CPU="$2"; shift 2 ;;
            --eal-cores)        EAL_CORES="$2"; shift 2 ;;
            --build-dir)        BUILD_DIR="$2"; shift 2 ;;
            --skip-kernel)      SKIP_KERNEL=true; shift ;;
            --skip-dpdk)        SKIP_DPDK=true; shift ;;
            --dry-run)          DRY_RUN=true; shift ;;
            -v|--verbose)       VERBOSE=true; shift ;;
            -h|--help)          usage ;;
            *) die "Unknown option: $1" "Run with --help for usage" ;;
        esac
    done

    [[ -z "$NIC_A" ]]      && die "--nic-a is required"
    [[ -z "$NIC_B" ]]      && die "--nic-b is required"
    [[ -z "$SERVER_IP" ]]  && die "--server-ip is required"
    [[ -z "$GATEWAY_IP" ]] && die "--gateway-ip is required"

    if [[ -z "$LOCAL_IP" ]]; then
        LOCAL_IP=$(ip -4 addr show "$NIC_B" 2>/dev/null | grep -oE 'inet [0-9.]+' | awk '{print $2}' || true)
        [[ -z "$LOCAL_IP" ]] && die "Cannot auto-detect NIC-B IP" "Specify --local-ip explicitly"
    fi

    LOCAL_PREFIX=$(ip -4 addr show "$NIC_B" 2>/dev/null | grep -oE 'inet [0-9./]+' | head -1 | awk '{print $2}' | grep -oE '/[0-9]+' || true)
    LOCAL_PREFIX="${LOCAL_PREFIX:-/20}"
    LOCAL_CIDR="${LOCAL_IP}${LOCAL_PREFIX}"

    if [[ -f /usr/lib/gcc/aarch64-amazon-linux/14/libstdc++.so ]]; then
        export LD_LIBRARY_PATH="/usr/lib/gcc/aarch64-amazon-linux/14:${LD_LIBRARY_PATH:-}"
    fi

    # Resolve scenarios list
    if [[ "$SCENARIOS" == "all" ]]; then
        SCENARIO_LIST=(tcp_echo udp_echo ws_echo market_rx order_rtt udp_relay)
    else
        IFS=',' read -ra SCENARIO_LIST <<< "$SCENARIOS"
    fi

    # Resolve transports list
    if [[ "$TRANSPORTS" == "all" ]]; then
        TRANSPORT_LIST=(kernel dpdk)
    else
        IFS=',' read -ra TRANSPORT_LIST <<< "$TRANSPORTS"
    fi

    if [[ "$SKIP_KERNEL" == true ]]; then
        TRANSPORT_LIST=("${TRANSPORT_LIST[@]/kernel}")
    fi
    if [[ "$SKIP_DPDK" == true ]]; then
        TRANSPORT_LIST=("${TRANSPORT_LIST[@]/dpdk}")
    fi
    return 0
}

# ── Payload sizes per scenario ────────────────────────────────────────────
payload_sizes_for() {
    case "$1" in
        tcp_echo)             echo "$TCP_PAYLOADS" ;;
        udp_echo|udp_relay)   echo "$UDP_PAYLOADS" ;;
        ws_echo)              echo "$WS_PAYLOADS" ;;
        market_rx|order_rtt)  echo "" ;;  # No payload sweep, fixed JSON
        *) echo "" ;;
    esac
}

# ── Preflight checks ──────────────────────────────────────────────────────
preflight() {
    log_phase 0 "Preflight Checks"

    [[ $EUID -ne 0 ]] && die "Must run as root" "sudo $0 ${ORIGINAL_ARGS[*]:-}"
    log_info "Root privileges"

    ip link show "$NIC_A" &>/dev/null || die "NIC-A '$NIC_A' not found"
    if ip link show "$NIC_B" &>/dev/null; then
        log_info "NICs: $NIC_A (server) + $NIC_B (client, kernel)"
    else
        # NIC-B may already be bound to DPDK (vfio-pci) — check via .dpdk_state
        # or assume DPDK-only mode is intended.
        if [[ " ${TRANSPORT_LIST[*]} " == *" kernel "* ]]; then
            die "NIC-B '$NIC_B' not found in kernel" \
                "Kernel mode requires NIC-B to be kernel-managed. Run dpdk-teardown.sh first or use --transports dpdk."
        fi
        log_info "NICs: $NIC_A (server) + $NIC_B (client, already DPDK-bound)"
    fi

    local default_dev=""
    if ip link show "$NIC_B" &>/dev/null; then
        default_dev=$(ip route show default | awk '{print $5}' | head -1)
    fi
    if [[ "$default_dev" == "$NIC_B" ]]; then
        log_warn "NIC-B ($NIC_B) carries the default route"
        log_warn "Moving it to a namespace will disconnect SSH if you're using this NIC!"
        if [[ "$DRY_RUN" == false ]]; then
            printf "  Continue? [y/N] "
            read -r -t 15 ans || ans=""
            case "$ans" in [yY]*) ;; *) echo "Cancelled."; exit 0 ;; esac
        fi
    fi

    if [[ -z "$BUILD_DIR" ]]; then
        for candidate in \
            "$PROJECT_DIR/build/linux/arm64/release" \
            "$PROJECT_DIR/build/linux/aarch64/release" \
            "$PROJECT_DIR/build/linux/x86_64/release"; do
            [[ -d "$candidate" ]] && { BUILD_DIR="$candidate"; break; }
        done
        [[ -z "$BUILD_DIR" ]] && die "Cannot find build directory" "Specify --build-dir"
    fi
    log_info "Build dir: $BUILD_DIR"

    # Check required binaries based on selected scenarios
    local missing=false
    local needed=()
    for s in "${SCENARIO_LIST[@]}"; do
        for t in "${TRANSPORT_LIST[@]}"; do
            local bin="bench_${s}"
            [[ "$t" == "dpdk" ]] && bin="${bin}_dpdk"
            needed+=("$bin")
        done
    done
    # Always need standalone mock servers for kernel mode
    if [[ " ${TRANSPORT_LIST[*]} " == *" kernel "* ]]; then
        for s in "${SCENARIO_LIST[@]}"; do
            case "$s" in
                udp_echo)   needed+=("bench_udp_echo_server") ;;
                tcp_echo)   needed+=("bench_tcp_echo_server") ;;
                udp_relay)  needed+=("bench_udp_echo_server") ;;
                ws_echo|market_rx|order_rtt) needed+=("bench_mock_server") ;;
            esac
        done
    fi

    for bin in "${needed[@]}"; do
        [[ -x "$BUILD_DIR/$bin" ]] || { log_error "Missing: $BUILD_DIR/$bin"; missing=true; }
    done
    [[ "$missing" == true ]] && die "Missing binaries" "Build with: xmake build <target>"
    log_info "All required binaries found"

    # Connectivity check
    if ping -c 1 -W 1 -I "$NIC_B" "$SERVER_IP" &>/dev/null; then
        log_info "Connectivity: $NIC_B → $SERVER_IP OK"
    else
        log_warn "Cannot ping $SERVER_IP from $NIC_B (continuing)"
    fi
}

# ── Environment metadata ──────────────────────────────────────────────────
record_env_metadata() {
    mkdir -p "$OUTPUT_DIR"
    local env_file="$OUTPUT_DIR/env_${TIMESTAMP}.txt"
    {
        echo "# Bench environment metadata - $(date '+%Y-%m-%d %H:%M:%S')"
        echo
        echo "## System"
        echo "kernel:    $(uname -r)"
        echo "arch:      $(uname -m)"
        echo "hostname:  $(hostname)"
        echo
        echo "## CPU"
        lscpu | grep -E '^(Model name|CPU\(s\)|Thread|Socket|MHz|Cache)' || true
        echo
        echo "## Memory"
        free -h | head -2 || true
        echo
        echo "## NICs"
        for nic in "$NIC_A" "$NIC_B"; do
            echo "### $nic"
            ip -4 addr show "$nic" 2>/dev/null | grep inet || true
            ethtool -i "$nic" 2>/dev/null | grep -E 'driver|version|firmware' || true
            echo
        done
        echo "## Hugepages"
        grep -i huge /proc/meminfo || true
        echo
        echo "## Bench config"
        echo "duration:    ${DURATION}s"
        echo "warmup:      ${WARMUP}s"
        echo "poll_cpu:    $POLL_CPU"
        echo "mock_cpu:    $MOCK_CPU"
        echo "eal_cores:   $EAL_CORES"
        echo "scenarios:   ${SCENARIO_LIST[*]}"
        echo "transports:  ${TRANSPORT_LIST[*]}"
    } > "$env_file"
    log_info "Environment metadata: $env_file"
}

# ── Mock server management ────────────────────────────────────────────────
MOCK_PID=""

start_mock_for_scenario() {
    local scenario=$1
    case "$scenario" in
        udp_echo)
            "$BUILD_DIR/bench_udp_echo_server" \
                --bind-ip "$SERVER_IP" --port 9997 --cpu "$MOCK_CPU" &>>"$OUTPUT_DIR/mock.log" &
            MOCK_PID=$!
            ;;
        tcp_echo)
            # tcp_echo_server needs msg_size — start in payload loop instead
            ;;
        ws_echo)
            "$BUILD_DIR/bench_mock_server" \
                --bind-ip "$SERVER_IP" --port 9999 \
                --symbols "$SYMBOLS" --tick-us "$TICK_US" --cpu "$MOCK_CPU" \
                --echo-mode &>>"$OUTPUT_DIR/mock.log" &
            MOCK_PID=$!
            ;;
        market_rx)
            "$BUILD_DIR/bench_mock_server" \
                --bind-ip "$SERVER_IP" --port 9999 \
                --symbols "$SYMBOLS" --tick-us "$TICK_US" --cpu "$MOCK_CPU" \
                &>>"$OUTPUT_DIR/mock.log" &
            MOCK_PID=$!
            ;;
        order_rtt)
            "$BUILD_DIR/bench_mock_server" \
                --bind-ip "$SERVER_IP" --port 9999 \
                --symbols "$SYMBOLS" --tick-us "$TICK_US" --cpu "$MOCK_CPU" \
                --order-mode &>>"$OUTPUT_DIR/mock.log" &
            MOCK_PID=$!
            ;;
        udp_relay)
            # udp_relay is now a single-port echo (same fair-comparison shape as
            # udp_echo, just on a different port). Reuses bench_udp_echo_server.
            "$BUILD_DIR/bench_udp_echo_server" \
                --bind-ip "$SERVER_IP" --port 9996 --cpu "$MOCK_CPU" &>>"$OUTPUT_DIR/mock.log" &
            MOCK_PID=$!
            ;;
    esac
    [[ -n "$MOCK_PID" ]] && sleep 0.5
}

stop_mock() {
    if [[ -n "$MOCK_PID" ]]; then
        kill "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
        MOCK_PID=""
    fi
}

# ── Run a single bench ────────────────────────────────────────────────────
run_bench() {
    local scenario=$1 transport=$2 payloads=$3
    local bin="$BUILD_DIR/bench_${scenario}"
    [[ "$transport" == "dpdk" ]] && bin="${bin}_dpdk"

    local common_args=(
        --server-ip "$SERVER_IP"
        --duration "$DURATION"
        --warmup "$WARMUP"
        --poll-cpu "$POLL_CPU"
        --mock-cpu "$MOCK_CPU"
        --output "$JSONL_FILE"
    )

    [[ -n "$payloads" ]] && common_args+=(--payload-sizes "$payloads")

    case "$scenario" in
        ws_echo|market_rx|order_rtt)
            common_args+=(--port 9999 --symbols "$SYMBOLS" --tick-interval "$TICK_US")
            [[ "$scenario" == "order_rtt" ]] && common_args+=(--order-interval "$ORDER_INTERVAL_US")
            ;;
        udp_echo) common_args+=(--port 9997) ;;
        tcp_echo) common_args+=(--port 9998) ;;
        udp_relay) ;;  # port hardcoded to 9996 in bench_udp_relay.cpp
    esac

    if [[ "$transport" == "kernel" ]]; then
        log_info "Running bench_${scenario} (kernel)"
        if [[ "$DRY_RUN" == true ]]; then
            echo "[DRY RUN] ip netns exec bench_ns $bin ${common_args[*]}"
        else
            ip netns exec bench_ns "$bin" "${common_args[@]}" 2>&1 || log_warn "bench_${scenario} (kernel) returned $?"
        fi
    else
        log_info "Running bench_${scenario} (DPDK)"
        local nic_b_pci="${NIC_B_PCI:-}"
        if [[ -z "$nic_b_pci" ]]; then
            nic_b_pci=$(basename "$(readlink -f "/sys/class/net/${NIC_B}/device" 2>/dev/null)" 2>/dev/null || echo "")
        fi
        if [[ -z "$nic_b_pci" || "$nic_b_pci" == "device" ]]; then
            log_error "Cannot determine PCI for $NIC_B"; return
        fi
        local eal_args="-a ${nic_b_pci} -l ${EAL_CORES}"
        local dpdk_args=("${common_args[@]}" --local-ip "$LOCAL_IP" --gateway-ip "$GATEWAY_IP")

        if [[ "$DRY_RUN" == true ]]; then
            # shellcheck disable=SC2086
            echo "[DRY RUN] $bin $eal_args -- ${dpdk_args[*]}"
        else
            # shellcheck disable=SC2086
            "$bin" $eal_args -- "${dpdk_args[@]}" 2>&1 || log_warn "bench_${scenario} (DPDK) returned $?"
        fi
    fi
    echo
}

# ── Run scenarios ─────────────────────────────────────────────────────────
run_all_scenarios() {
    JSONL_FILE="$OUTPUT_DIR/latency_results_${TIMESTAMP}.jsonl"
    : > "$JSONL_FILE"
    log_info "JSONL output: $JSONL_FILE"
    : > "$OUTPUT_DIR/mock.log"

    # Set up netns for kernel mode (if needed)
    local need_netns=false
    for t in "${TRANSPORT_LIST[@]}"; do
        [[ "$t" == "kernel" ]] && need_netns=true
    done

    if [[ "$need_netns" == true && "$DRY_RUN" == false ]]; then
        log_phase 1 "Setup netns for kernel benchmarks"
        ip netns add bench_ns 2>/dev/null || true
        ip link set "$NIC_B" netns bench_ns
        ip netns exec bench_ns ip addr add "$LOCAL_CIDR" dev "$NIC_B"
        ip netns exec bench_ns ip link set "$NIC_B" up
        ip netns exec bench_ns ip link set lo up
        ip netns exec bench_ns ip route add default via "$GATEWAY_IP" dev "$NIC_B"
        log_info "Namespace bench_ns ready"
    fi

    # Setup DPDK if needed
    local need_dpdk=false
    local nic_b_pci=""
    for t in "${TRANSPORT_LIST[@]}"; do
        [[ "$t" == "dpdk" ]] && need_dpdk=true
    done

    if [[ "$need_dpdk" == true && "$DRY_RUN" == false ]]; then
        log_phase 2 "Setup DPDK on $NIC_B"
        # First restore from netns if we did kernel first
        if [[ "$need_netns" == true ]]; then
            ip netns exec bench_ns ip link set "$NIC_B" netns 1
            ip netns del bench_ns 2>/dev/null || true
            ip link set "$NIC_B" up
            ip addr add "$LOCAL_CIDR" dev "$NIC_B" 2>/dev/null || true
            sleep 1
        fi

        # Try to detect PCI from kernel sysfs first; if NIC is already bound
        # to DPDK, fall back to .dpdk_state file.
        WE_BOUND_DPDK=false
        nic_b_pci=$(basename "$(readlink -f "/sys/class/net/${NIC_B}/device" 2>/dev/null)" 2>/dev/null || echo "")
        if [[ -z "$nic_b_pci" || "$nic_b_pci" == "device" ]]; then
            if [[ -f "$PROJECT_DIR/.dpdk_state" ]]; then
                # shellcheck source=/dev/null
                source "$PROJECT_DIR/.dpdk_state"
                nic_b_pci="$DPDK_PCI"
                log_info "NIC-B already bound to DPDK (PCI: $nic_b_pci from .dpdk_state)"
            else
                die "Cannot detect PCI for $NIC_B" "Bind manually with dpdk-setup.sh first"
            fi
        else
            log_info "NIC-B PCI: $nic_b_pci"
            if [[ -x "$SCRIPT_DIR/dpdk-setup.sh" ]]; then
                DPDK_PCI="$nic_b_pci" DPDK_IFACE="$NIC_B" "$SCRIPT_DIR/dpdk-setup.sh" -y
                WE_BOUND_DPDK=true
                log_info "NIC-B bound to DPDK"
            else
                log_warn "scripts/dpdk-setup.sh not found, assuming NIC-B already bound"
            fi
        fi
        # Export for run_bench
        export NIC_B_PCI="$nic_b_pci"
    fi

    # Run scenarios — kernel first if in list
    if [[ " ${TRANSPORT_LIST[*]} " == *" kernel "* ]] && [[ "$DRY_RUN" == false || "$DRY_RUN" == true ]]; then
        log_phase 3 "Kernel benchmarks"

        for scenario in "${SCENARIO_LIST[@]}"; do
            local payloads
            payloads=$(payload_sizes_for "$scenario")
            log_info "── Scenario: ${scenario} (kernel) ──"

            # tcp_echo: per-payload mock since msg_size is fixed
            if [[ "$scenario" == "tcp_echo" ]]; then
                local payload_arr
                IFS=',' read -ra payload_arr <<< "$payloads"
                for p in "${payload_arr[@]}"; do
                    "$BUILD_DIR/bench_tcp_echo_server" \
                        --bind-ip "$SERVER_IP" --port 9998 \
                        --msg-size "$p" --cpu "$MOCK_CPU" &>>"$OUTPUT_DIR/mock.log" &
                    MOCK_PID=$!
                    sleep 0.3
                    run_bench "$scenario" "kernel" "$p"
                    stop_mock
                    sleep 0.3
                done
            else
                start_mock_for_scenario "$scenario"
                run_bench "$scenario" "kernel" "$payloads"
                stop_mock
                sleep 0.3
            fi
        done
    fi

    # DPDK benchmarks
    if [[ " ${TRANSPORT_LIST[*]} " == *" dpdk "* ]]; then
        log_phase 4 "DPDK benchmarks"

        for scenario in "${SCENARIO_LIST[@]}"; do
            local payloads
            payloads=$(payload_sizes_for "$scenario")
            log_info "── Scenario: ${scenario} (DPDK) ──"
            run_bench "$scenario" "dpdk" "$payloads"
            sleep 0.5
        done
    fi

    # Restore NIC from DPDK only if WE bound it (leave it alone if pre-bound)
    if [[ "$need_dpdk" == true && "$DRY_RUN" == false && -n "$nic_b_pci" && "${WE_BOUND_DPDK:-false}" == true ]]; then
        log_phase 5 "Restore NIC-B"
        if [[ -x "$SCRIPT_DIR/dpdk-teardown.sh" ]]; then
            DPDK_PCI="$nic_b_pci" DPDK_IFACE="$NIC_B" "$SCRIPT_DIR/dpdk-teardown.sh" || true
            log_info "NIC-B restored"
        fi
    fi
}

# ── Cleanup on exit ────────────────────────────────────────────────────────
cleanup_on_exit() {
    local exit_code=$?
    stop_mock
    if ip netns list 2>/dev/null | grep -q bench_ns; then
        log_warn "Cleanup: restoring $NIC_B from namespace..."
        ip netns exec bench_ns ip link set "$NIC_B" netns 1 2>/dev/null || true
        ip netns del bench_ns 2>/dev/null || true
        ip link set "$NIC_B" up 2>/dev/null || true
        ip addr add "${LOCAL_CIDR:-$LOCAL_IP/20}" dev "$NIC_B" 2>/dev/null || true
    fi
    pkill -f bench_mock_server 2>/dev/null || true
    pkill -f bench_udp_echo_server 2>/dev/null || true
    pkill -f bench_tcp_echo_server 2>/dev/null || true
    return "$exit_code"
}

# ── Generate summary ──────────────────────────────────────────────────────
generate_summary() {
    [[ ! -f "$JSONL_FILE" ]] && return
    [[ ! -s "$JSONL_FILE" ]] && { log_warn "JSONL output is empty"; return; }

    log_phase 6 "Summary"

    if command -v jq &>/dev/null; then
        echo
        printf "%-12s %-8s %-8s %-6s %12s %12s %12s %12s\n" \
            "scenario" "transport" "leg" "payload" "p50_us" "p99_us" "p999_us" "max_us"
        echo "─────────────────────────────────────────────────────────────────────────────────────"
        jq -r '. | "\(.scenario)\t\(.transport)\t\(.leg)\t\(.payload)\t\(.p50_us)\t\(.p99_us)\t\(.p999_us)\t\(.max_us)"' \
            "$JSONL_FILE" | \
            awk -F'\t' '{ printf "%-12s %-8s %-8s %-6d %12.1f %12.1f %12.1f %12.1f\n", $1, $2, $3, $4, $5, $6, $7, $8 }'
        echo
    else
        log_warn "jq not installed; raw JSONL at $JSONL_FILE"
    fi

    log_info "Full JSONL: $JSONL_FILE"
}

# ── Main ──────────────────────────────────────────────────────────────────
main() {
    parse_args "$@"
    trap cleanup_on_exit EXIT

    TIMESTAMP=$(date '+%Y%m%d_%H%M%S')

    separator
    echo -e "${BOLD}bench_latency.sh — 6-scenario × kernel/DPDK × multi-payload${NC}"
    echo -e "  NIC-A:       $NIC_A ($SERVER_IP)"
    echo -e "  NIC-B:       $NIC_B ($LOCAL_IP)"
    echo -e "  Gateway:     $GATEWAY_IP"
    echo -e "  Duration:    ${DURATION}s, Warmup: ${WARMUP}s"
    echo -e "  Scenarios:   ${SCENARIO_LIST[*]}"
    echo -e "  Transports:  ${TRANSPORT_LIST[*]}"
    echo -e "  Output:      $OUTPUT_DIR"
    separator

    preflight
    record_env_metadata
    run_all_scenarios
    generate_summary

    separator
    echo -e "${BOLD}${GREEN}✓ Bench complete${NC}"
    echo -e "  Results: $OUTPUT_DIR/latency_results_${TIMESTAMP}.jsonl"
    echo -e "  Env:     $OUTPUT_DIR/env_${TIMESTAMP}.txt"
    separator
}

main "$@"
