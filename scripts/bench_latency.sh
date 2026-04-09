#!/usr/bin/env bash
# bench_latency.sh — orchestrate the 6-scenario × 2-transport latency suite.
#
# Modes (mutually exclusive — choose one):
#   --loopback              Run mock + client on the same host bound to 127.0.0.1.
#                           No NICs, no netns, no DPDK. The fastest path to a smoke
#                           test on a dev machine.
#   --nic-a IFACE           "Real wire" mode: mock binds NIC-A in the host namespace,
#   --nic-b IFACE           bench client runs in a `bench_ns` namespace that owns
#   --server-ip IP          NIC-B. Required for honest kernel-vs-DPDK comparison.
#   --gateway-ip IP         The DPDK transport additionally binds NIC-B to vfio-pci
#                           via scripts/dpdk-setup.sh.
#
# Scenarios (--scenarios CSV, default all 6):
#   tcp udp ws exchange/market exchange/order exchange/md_udp
#
# Transports (--transports CSV, default kernel + dpdk):
#   kernel dpdk
#
# Bench client output is written to stdout. The script DOES NOT create any files
# under .bench/ — if you want to keep a record, redirect or tee the output:
#
#     sudo ./scripts/bench_latency.sh --loopback | tee my-run.log
#
# Pre-existing dependencies:
#   - xmake build of all 22 latency targets (mock_lat_* and bench_lat_*)
#   - For --nic-* modes: root, ip(8), two NICs, and a default isolcpus layout
#   - For --transports dpdk: scripts/dpdk-setup.sh / dpdk-teardown.sh
#
# This script is intentionally a thin shell. All measurement / pinning / TSC
# stamping happens inside the C++ binaries.
#
# Plan reference: .artifacts/plan-bench-latency-rewrite-20260409-023700.md stage 5

set -euo pipefail

# ── Color & log helpers ────────────────────────────────────────────────────
if [[ -t 1 ]] && [[ -z "${NO_COLOR:-}" ]]; then
    RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
    BLUE=$'\033[0;34m'; CYAN=$'\033[0;36m'; BOLD=$'\033[1m'; NC=$'\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BLUE=''; CYAN=''; BOLD=''; NC=''
fi

log_phase() { printf '\n%s▶ %s%s\n\n' "$BOLD$BLUE" "$*" "$NC"; }
log_info()  { printf '%s✓%s  %s\n' "$GREEN" "$NC" "$*"; }
log_warn()  { printf '%s⚠%s  %s\n' "$YELLOW" "$NC" "$*" >&2; }
log_error() { printf '%s✗%s  %s%s%s\n' "$RED" "$NC" "$BOLD" "$*" "$NC" >&2; }
log_verb()  { [[ "${VERBOSE:-false}" == true ]] && printf '%sℹ%s  %s\n' "$CYAN" "$NC" "$*" || true; }

ORIGINAL_ARGS=("$@")
die() {
    log_error "$1"
    [[ -n "${2:-}" ]] && printf '   %s\n' "$2" >&2
    printf '\n   Rerun: sudo %s %s\n' "$0" "${ORIGINAL_ARGS[*]:-}" >&2
    exit 1
}

# ── Defaults ───────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

LOOPBACK=false
NIC_A="${BENCH_NIC_A:-}"
NIC_B="${BENCH_NIC_B:-}"
SERVER_IP="${BENCH_SERVER_IP:-}"
LOCAL_IP="${BENCH_LOCAL_IP:-}"
GATEWAY_IP="${BENCH_GATEWAY_IP:-}"

DURATION="${BENCH_DURATION:-10}"
WARMUP="${BENCH_WARMUP:-2}"
CLIENT_CPU="${BENCH_CLIENT_CPU:-2}"
MOCK_CPU="${BENCH_MOCK_CPU:-4}"
EAL_CORES="${BENCH_EAL_CORES:-0,1}"

SYMBOLS="${BENCH_SYMBOLS:-BTCUSDT,ETHUSDT,SOLUSDT}"
BOOKTICKER_US="${BENCH_BOOKTICKER_US:-100}"
DEPTH_MS="${BENCH_DEPTH_MS:-10}"
TRADE_MEAN_MS="${BENCH_TRADE_MEAN_MS:-5}"
KLINE_S="${BENCH_KLINE_S:-1}"
DEPTH_BYTES="${BENCH_DEPTH_BYTES:-1024}"
SERVER_WORK_NS="${BENCH_SERVER_WORK_NS:-200}"

TCP_PAYLOADS="${BENCH_TCP_PAYLOADS:-64,128,256,512,1024,1460,4096,16384}"
UDP_PAYLOADS="${BENCH_UDP_PAYLOADS:-64,128,256,512,1024,1472}"
WS_PAYLOADS="${BENCH_WS_PAYLOADS:-64,128,256,512,1024,4096}"
MD_UDP_PAYLOADS="${BENCH_MD_UDP_PAYLOADS:-64,256,1024,1400}"
INFLIGHTS="${BENCH_INFLIGHTS:-1,4,16,64}"

ALLOW_NON_ISOLATED=false
DRY_RUN=false
VERBOSE=false
SCENARIOS_RAW="all"
TRANSPORTS_RAW="all"
BUILD_DIR=""

# Ports — kept distinct so a stuck mock from a previous scenario does not
# accidentally serve the next scenario's client.
PORT_TCP=19101
PORT_UDP=19102
PORT_WS=19103
PORT_EX_WS=19104
PORT_EX_MD_UDP=19105

# ── Usage ──────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
${BOLD}bench_latency.sh${NC} — 6-scenario × 2-transport latency runner

${BOLD}Usage${NC}
  Loopback mode (dev, no NIC):
    sudo $0 --loopback [options]

  Real-wire mode (production benchmarks):
    sudo $0 --nic-a IFACE --nic-b IFACE --server-ip IP --gateway-ip IP [options]

${BOLD}Filters${NC}
  --scenarios LIST    csv: tcp,udp,ws,exchange/market,exchange/order,exchange/md_udp,all (default: all)
  --transports LIST   csv: kernel,dpdk,all (default: all)

${BOLD}Tuning${NC}
  --duration SEC      bench measurement window (default: ${DURATION})
  --warmup SEC        bench warmup window     (default: ${WARMUP})
  --client-cpu N      bench client cpu        (default: ${CLIENT_CPU})
  --mock-cpu N        mock server cpu         (default: ${MOCK_CPU})
  --eal-cores LIST    DPDK EAL lcore list     (default: ${EAL_CORES})
  --server-work-ns N  spin N ns inside mock   (default: ${SERVER_WORK_NS})
  --symbols CSV       exchange ws symbols     (default: ${SYMBOLS})
  --bookticker-us N   exchange bookTicker period
  --depth-ms N        exchange depth period
  --trade-mean-ms N   exchange trade Poisson mean
  --kline-s N         exchange kline period
  --depth-bytes N     exchange depth payload bytes
  --tcp-payloads CSV  tcp payload sweep
  --udp-payloads CSV  udp payload sweep
  --ws-payloads CSV   ws payload sweep
  --md-udp-payloads CSV md_udp payload sweep
  --inflights CSV     exchange/order inflight sweep

${BOLD}Behavior${NC}
  --allow-non-isolated  pass through to all binaries (relaxes isolcpus check)
  --build-dir DIR       xmake build dir (auto-detected by default)
  --dry-run             print commands without running them
  -v, --verbose         verbose output
  -h, --help            this message

${BOLD}Examples${NC}
  # Loopback smoke test (3 second windows, just tcp + udp + ws kernel):
  sudo $0 --loopback --duration 3 --scenarios tcp,udp,ws --transports kernel

  # Full real-wire suite, tee output to a log:
  sudo $0 --nic-a ens34 --nic-b ens35 --server-ip 172.31.21.173 \\
          --gateway-ip 172.31.16.1 | tee bench-\$(date +%Y%m%d-%H%M%S).log

  # Just the exchange order scenario, kernel only, with custom inflight sweep:
  sudo $0 --loopback --scenarios exchange/order --transports kernel \\
          --inflights 1,8,32 --duration 5

EOF
    exit 0
}

# ── Argument parsing ───────────────────────────────────────────────────────
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --loopback)            LOOPBACK=true; shift ;;
            --nic-a)               NIC_A="$2"; shift 2 ;;
            --nic-b)               NIC_B="$2"; shift 2 ;;
            --server-ip)           SERVER_IP="$2"; shift 2 ;;
            --local-ip)            LOCAL_IP="$2"; shift 2 ;;
            --gateway-ip)          GATEWAY_IP="$2"; shift 2 ;;
            --duration)            DURATION="$2"; shift 2 ;;
            --warmup)              WARMUP="$2"; shift 2 ;;
            --scenarios)           SCENARIOS_RAW="$2"; shift 2 ;;
            --transports)          TRANSPORTS_RAW="$2"; shift 2 ;;
            --client-cpu)          CLIENT_CPU="$2"; shift 2 ;;
            --mock-cpu)            MOCK_CPU="$2"; shift 2 ;;
            --eal-cores)           EAL_CORES="$2"; shift 2 ;;
            --server-work-ns)      SERVER_WORK_NS="$2"; shift 2 ;;
            --symbols)             SYMBOLS="$2"; shift 2 ;;
            --bookticker-us)       BOOKTICKER_US="$2"; shift 2 ;;
            --depth-ms)            DEPTH_MS="$2"; shift 2 ;;
            --trade-mean-ms)       TRADE_MEAN_MS="$2"; shift 2 ;;
            --kline-s)             KLINE_S="$2"; shift 2 ;;
            --depth-bytes)         DEPTH_BYTES="$2"; shift 2 ;;
            --tcp-payloads)        TCP_PAYLOADS="$2"; shift 2 ;;
            --udp-payloads)        UDP_PAYLOADS="$2"; shift 2 ;;
            --ws-payloads)         WS_PAYLOADS="$2"; shift 2 ;;
            --md-udp-payloads)     MD_UDP_PAYLOADS="$2"; shift 2 ;;
            --inflights)           INFLIGHTS="$2"; shift 2 ;;
            --allow-non-isolated)  ALLOW_NON_ISOLATED=true; shift ;;
            --build-dir)           BUILD_DIR="$2"; shift 2 ;;
            --dry-run)             DRY_RUN=true; shift ;;
            -v|--verbose)          VERBOSE=true; shift ;;
            -h|--help)             usage ;;
            *) die "Unknown option: $1" "Run $0 --help for usage" ;;
        esac
    done

    if [[ "$LOOPBACK" == true ]]; then
        if [[ -n "$NIC_A$NIC_B" ]]; then
            die "--loopback is mutually exclusive with --nic-a / --nic-b" \
                "Pick one mode."
        fi
        SERVER_IP=127.0.0.1
        LOCAL_IP=127.0.0.1
    else
        [[ -z "$NIC_A" ]]      && die "--nic-a is required (or use --loopback)"
        [[ -z "$NIC_B" ]]      && die "--nic-b is required (or use --loopback)"
        [[ -z "$SERVER_IP" ]]  && die "--server-ip is required (or use --loopback)"
        [[ -z "$GATEWAY_IP" ]] && die "--gateway-ip is required (or use --loopback)"
    fi

    # Resolve scenarios.
    if [[ "$SCENARIOS_RAW" == "all" ]]; then
        SCENARIO_LIST=(tcp udp ws exchange/market exchange/order exchange/md_udp)
    else
        IFS=',' read -ra SCENARIO_LIST <<< "$SCENARIOS_RAW"
    fi

    # Resolve transports.
    if [[ "$TRANSPORTS_RAW" == "all" ]]; then
        TRANSPORT_LIST=(kernel dpdk)
    else
        IFS=',' read -ra TRANSPORT_LIST <<< "$TRANSPORTS_RAW"
    fi

    # In loopback mode the dpdk variant cannot work — POSIX-stub today, real
    # DPDK tomorrow still needs vfio-pci. Refuse early so the user is not
    # confused by identical numbers from "kernel" and "dpdk".
    if [[ "$LOOPBACK" == true ]]; then
        local filtered=()
        for t in "${TRANSPORT_LIST[@]}"; do
            if [[ "$t" == "dpdk" ]]; then
                log_warn "loopback mode skips the dpdk transport (no NIC binding possible)"
            else
                filtered+=("$t")
            fi
        done
        TRANSPORT_LIST=("${filtered[@]}")
        if [[ ${#TRANSPORT_LIST[@]} -eq 0 ]]; then
            die "loopback + --transports dpdk has no work to do" \
                "Drop --transports or pick kernel."
        fi
    fi
}

# ── Build dir auto-detect ──────────────────────────────────────────────────
detect_build_dir() {
    if [[ -n "$BUILD_DIR" ]]; then
        [[ -d "$BUILD_DIR" ]] || die "build dir not found: $BUILD_DIR"
        return
    fi
    for d in \
        "$PROJECT_DIR/build/linux/arm64/release" \
        "$PROJECT_DIR/build/linux/aarch64/release" \
        "$PROJECT_DIR/build/linux/x86_64/release"; do
        if [[ -d "$d" ]]; then BUILD_DIR="$d"; return; fi
    done
    die "Cannot auto-detect xmake build dir under $PROJECT_DIR/build/" \
        "Run \`xmake build\` first or pass --build-dir DIR."
}

# ── Preflight ──────────────────────────────────────────────────────────────
preflight() {
    log_phase "Preflight"

    # Root check (loopback also needs root because pin_thread_strict + bind <1024
    # may otherwise fail; we use ports >19000 so technically loopback could run
    # as a normal user, but isolcpus check via /proc reads benefits from root.)
    if [[ "$LOOPBACK" == false && $EUID -ne 0 ]]; then
        die "must run as root for --nic-* mode" "sudo $0 ${ORIGINAL_ARGS[*]:-}"
    fi
    if [[ $EUID -eq 0 ]]; then
        log_info "running as root"
    else
        log_info "running as $(id -un) (loopback mode)"
    fi

    detect_build_dir
    log_info "build dir: $BUILD_DIR"

    # Required binaries vary by selected scenarios.
    local needed=()
    for s in "${SCENARIO_LIST[@]}"; do
        for t in "${TRANSPORT_LIST[@]}"; do
            local suffix=""
            [[ "$t" == "dpdk" ]] && suffix="_dpdk"
            case "$s" in
                tcp) needed+=("mock_lat_tcp${suffix}" "bench_lat_tcp${suffix}") ;;
                udp) needed+=("mock_lat_udp${suffix}" "bench_lat_udp${suffix}") ;;
                ws)  needed+=("mock_lat_ws${suffix}"  "bench_lat_ws${suffix}")  ;;
                exchange/market)
                     needed+=("mock_lat_exchange_ws${suffix}" "bench_lat_exchange_market${suffix}") ;;
                exchange/order)
                     needed+=("mock_lat_exchange_ws${suffix}" "bench_lat_exchange_order${suffix}") ;;
                exchange/md_udp)
                     needed+=("mock_lat_exchange_md_udp${suffix}" "bench_lat_exchange_md_udp${suffix}") ;;
                *) die "unknown scenario: $s"
            esac
        done
    done
    local missing=()
    for b in "${needed[@]}"; do
        if [[ ! -x "$BUILD_DIR/$b" ]]; then
            missing+=("$b")
        fi
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "missing binaries:"
        printf '   %s\n' "${missing[@]}" >&2
        die "binaries missing" \
            "Build with: cd $PROJECT_DIR && xmake build ${missing[*]}"
    fi
    log_info "all required binaries present (${#needed[@]} total)"

    # NIC mode: validate NICs and connectivity.
    if [[ "$LOOPBACK" == false ]]; then
        ip link show "$NIC_A" &>/dev/null || die "NIC-A '$NIC_A' not found"
        if ip link show "$NIC_B" &>/dev/null; then
            log_info "NICs: $NIC_A (server) + $NIC_B (client)"
        else
            log_warn "NIC-B '$NIC_B' not present in kernel — may already be DPDK-bound"
        fi

        if [[ -z "$LOCAL_IP" ]]; then
            LOCAL_IP=$(ip -4 addr show "$NIC_B" 2>/dev/null \
                | grep -oE 'inet [0-9.]+' | awk '{print $2}' || true)
            [[ -z "$LOCAL_IP" ]] && die "cannot auto-detect NIC-B IP" \
                "Pass --local-ip explicitly."
        fi
        local prefix
        prefix=$(ip -4 addr show "$NIC_B" 2>/dev/null \
            | grep -oE 'inet [0-9./]+' | head -1 | awk '{print $2}' \
            | grep -oE '/[0-9]+' || true)
        LOCAL_CIDR="${LOCAL_IP}${prefix:-/20}"
        log_info "client side: $NIC_B = $LOCAL_CIDR  →  $SERVER_IP"
    else
        log_info "loopback: 127.0.0.1, ports $PORT_TCP/$PORT_UDP/$PORT_WS/$PORT_EX_WS/$PORT_EX_MD_UDP"
    fi

    # isolcpus warning.
    local isolated=""
    [[ -r /sys/devices/system/cpu/isolated ]] && isolated=$(< /sys/devices/system/cpu/isolated)
    if [[ -z "$isolated" || "$isolated" == "" ]]; then
        log_warn "no isolcpus configured — pinning will fail unless --allow-non-isolated"
        if [[ "$ALLOW_NON_ISOLATED" == false ]]; then
            log_warn "auto-enabling --allow-non-isolated"
            ALLOW_NON_ISOLATED=true
        fi
    else
        log_info "isolcpus: $isolated"
    fi

    log_info "scenarios:  ${SCENARIO_LIST[*]}"
    log_info "transports: ${TRANSPORT_LIST[*]}"
    log_info "duration=${DURATION}s warmup=${WARMUP}s client_cpu=$CLIENT_CPU mock_cpu=$MOCK_CPU"
}

# ── Network namespace setup (for kernel transport with --nic-* mode) ──────
NETNS_CREATED=false
NIC_B_PCI=""
DPDK_BOUND_BY_US=false

setup_netns() {
    [[ "$LOOPBACK" == true ]] && return
    [[ "$DRY_RUN" == true ]] && { log_verb "[dry-run] would create bench_ns and move $NIC_B"; return; }
    log_phase "Setup bench_ns (kernel transport via NIC-B)"
    ip netns add bench_ns 2>/dev/null || true
    ip link set "$NIC_B" netns bench_ns
    ip netns exec bench_ns ip addr add "$LOCAL_CIDR" dev "$NIC_B"
    ip netns exec bench_ns ip link set "$NIC_B" up
    ip netns exec bench_ns ip link set lo up
    ip netns exec bench_ns ip route add default via "$GATEWAY_IP" dev "$NIC_B"
    NETNS_CREATED=true
    log_info "bench_ns ready: $NIC_B → $GATEWAY_IP"
}

teardown_netns() {
    [[ "$NETNS_CREATED" == false ]] && return
    log_verb "tearing down bench_ns"
    ip netns exec bench_ns ip link set "$NIC_B" netns 1 2>/dev/null || true
    ip netns del bench_ns 2>/dev/null || true
    ip link set "$NIC_B" up 2>/dev/null || true
    ip addr add "$LOCAL_CIDR" dev "$NIC_B" 2>/dev/null || true
    NETNS_CREATED=false
}

setup_dpdk() {
    [[ "$LOOPBACK" == true ]] && return
    [[ "$DRY_RUN" == true ]] && { log_verb "[dry-run] would bind $NIC_B to vfio-pci"; return; }
    log_phase "Bind $NIC_B → vfio-pci for DPDK transport"
    NIC_B_PCI=$(basename "$(readlink -f "/sys/class/net/${NIC_B}/device" 2>/dev/null)" 2>/dev/null || true)
    if [[ -z "$NIC_B_PCI" || "$NIC_B_PCI" == "device" ]]; then
        log_warn "$NIC_B not in kernel — assuming already DPDK-bound"
        if [[ -f "$PROJECT_DIR/.dpdk_state" ]]; then
            # shellcheck disable=SC1091
            source "$PROJECT_DIR/.dpdk_state"
            NIC_B_PCI="$DPDK_PCI"
            log_info "loaded PCI from .dpdk_state: $NIC_B_PCI"
        else
            die "cannot determine PCI address for $NIC_B" \
                "Run scripts/dpdk-setup.sh manually first."
        fi
    elif [[ -x "$SCRIPT_DIR/dpdk-setup.sh" ]]; then
        DPDK_PCI="$NIC_B_PCI" DPDK_IFACE="$NIC_B" "$SCRIPT_DIR/dpdk-setup.sh" -y
        DPDK_BOUND_BY_US=true
        log_info "$NIC_B (PCI $NIC_B_PCI) bound to vfio-pci"
    else
        log_warn "scripts/dpdk-setup.sh not found — assuming preconfigured"
    fi
}

teardown_dpdk() {
    [[ "$DPDK_BOUND_BY_US" == false ]] && return
    [[ "$DRY_RUN" == true ]] && return
    if [[ -x "$SCRIPT_DIR/dpdk-teardown.sh" ]]; then
        log_verb "restoring $NIC_B from vfio-pci"
        DPDK_PCI="$NIC_B_PCI" DPDK_IFACE="$NIC_B" "$SCRIPT_DIR/dpdk-teardown.sh" || true
    fi
    DPDK_BOUND_BY_US=false
}

# ── Mock lifecycle ────────────────────────────────────────────────────────
MOCK_PID=""

start_mock() {
    local cmd=("$@")
    if [[ "$DRY_RUN" == true ]]; then
        log_verb "[dry-run] mock: ${cmd[*]} &"
        MOCK_PID=""
        return
    fi
    log_verb "mock: ${cmd[*]}"
    "${cmd[@]}" &
    MOCK_PID=$!
    # Give the mock 0.4s to bind + listen.
    sleep 0.4
}

stop_mock() {
    if [[ -n "$MOCK_PID" ]]; then
        kill -TERM "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
        MOCK_PID=""
    fi
    sleep 0.2
}

# Build the array of common bench client args.
common_client_args() {
    local out=(
        --server-ip "$SERVER_IP"
        --client-cpu "$CLIENT_CPU"
        --warmup "$WARMUP"
        --duration "$DURATION"
    )
    [[ "$ALLOW_NON_ISOLATED" == true ]] && out+=(--allow-non-isolated)
    printf '%s\n' "${out[@]}"
}

# Build the array of common mock args.
common_mock_args() {
    local out=(
        --server-ip "$SERVER_IP"
        --mock-cpu "$MOCK_CPU"
        --server-work-ns "$SERVER_WORK_NS"
    )
    [[ "$ALLOW_NON_ISOLATED" == true ]] && out+=(--allow-non-isolated)
    printf '%s\n' "${out[@]}"
}

# Run the bench client. In netns mode it goes through bench_ns.
run_client() {
    local cmd=("$@")
    if [[ "$DRY_RUN" == true ]]; then
        if [[ "$LOOPBACK" == true ]]; then
            log_verb "[dry-run] client: ${cmd[*]}"
        else
            log_verb "[dry-run] client: ip netns exec bench_ns ${cmd[*]}"
        fi
        return
    fi
    if [[ "$LOOPBACK" == true ]]; then
        "${cmd[@]}" || log_warn "client returned $?"
    else
        ip netns exec bench_ns "${cmd[@]}" || log_warn "client returned $?"
    fi
}

# ── Per-scenario runners ──────────────────────────────────────────────────
# Each runner: start_mock → run_client (one or many) → stop_mock.
# Distinct because tcp needs per-payload mock restarts (--msg-size baked in)
# and the exchange scenarios share / vary mock binaries.

run_tcp() {
    local transport="$1" suffix=""
    [[ "$transport" == "dpdk" ]] && suffix="_dpdk"
    log_phase "tcp ($transport)"
    local mock_args; mapfile -t mock_args < <(common_mock_args)
    local cli_args;  mapfile -t cli_args  < <(common_client_args)

    IFS=',' read -ra payloads <<< "$TCP_PAYLOADS"
    for p in "${payloads[@]}"; do
        log_info "tcp $transport payload=$p"
        start_mock "$BUILD_DIR/mock_lat_tcp${suffix}" "${mock_args[@]}" \
            --port "$PORT_TCP" --msg-size "$p"
        run_client "$BUILD_DIR/bench_lat_tcp${suffix}" "${cli_args[@]}" \
            --port "$PORT_TCP" --payload-sizes "$p"
        stop_mock
    done
}

run_udp() {
    local transport="$1" suffix=""
    [[ "$transport" == "dpdk" ]] && suffix="_dpdk"
    log_phase "udp ($transport)"
    local mock_args; mapfile -t mock_args < <(common_mock_args)
    local cli_args;  mapfile -t cli_args  < <(common_client_args)

    start_mock "$BUILD_DIR/mock_lat_udp${suffix}" "${mock_args[@]}" --port "$PORT_UDP"
    run_client "$BUILD_DIR/bench_lat_udp${suffix}" "${cli_args[@]}" \
        --port "$PORT_UDP" --payload-sizes "$UDP_PAYLOADS"
    stop_mock
}

run_ws() {
    local transport="$1" suffix=""
    [[ "$transport" == "dpdk" ]] && suffix="_dpdk"
    log_phase "ws ($transport)"
    local mock_args; mapfile -t mock_args < <(common_mock_args)
    local cli_args;  mapfile -t cli_args  < <(common_client_args)

    start_mock "$BUILD_DIR/mock_lat_ws${suffix}" "${mock_args[@]}" --port "$PORT_WS"
    run_client "$BUILD_DIR/bench_lat_ws${suffix}" "${cli_args[@]}" \
        --port "$PORT_WS" --payload-sizes "$WS_PAYLOADS"
    stop_mock
}

# market and order share the same mock — start once, run two clients, stop once.
EX_WS_MOCK_RUNNING=false

ensure_ex_ws_mock() {
    local transport="$1" suffix=""
    [[ "$transport" == "dpdk" ]] && suffix="_dpdk"
    [[ "$EX_WS_MOCK_RUNNING" == true ]] && return
    local mock_args; mapfile -t mock_args < <(common_mock_args)
    start_mock "$BUILD_DIR/mock_lat_exchange_ws${suffix}" "${mock_args[@]}" \
        --port "$PORT_EX_WS" \
        --symbols "$SYMBOLS" \
        --bookticker-us "$BOOKTICKER_US" \
        --depth-ms "$DEPTH_MS" \
        --trade-mean-ms "$TRADE_MEAN_MS" \
        --kline-s "$KLINE_S" \
        --depth-bytes "$DEPTH_BYTES"
    EX_WS_MOCK_RUNNING=true
}

teardown_ex_ws_mock() {
    [[ "$EX_WS_MOCK_RUNNING" == false ]] && return
    stop_mock
    EX_WS_MOCK_RUNNING=false
}

run_exchange_market() {
    local transport="$1" suffix=""
    [[ "$transport" == "dpdk" ]] && suffix="_dpdk"
    log_phase "exchange/market ($transport)"
    ensure_ex_ws_mock "$transport"
    local cli_args;  mapfile -t cli_args  < <(common_client_args)
    run_client "$BUILD_DIR/bench_lat_exchange_market${suffix}" \
        "${cli_args[@]}" --port "$PORT_EX_WS"
}

run_exchange_order() {
    local transport="$1" suffix=""
    [[ "$transport" == "dpdk" ]] && suffix="_dpdk"
    log_phase "exchange/order ($transport)"
    ensure_ex_ws_mock "$transport"
    local cli_args;  mapfile -t cli_args  < <(common_client_args)
    run_client "$BUILD_DIR/bench_lat_exchange_order${suffix}" \
        "${cli_args[@]}" --port "$PORT_EX_WS" --inflights "$INFLIGHTS"
}

run_exchange_md_udp() {
    local transport="$1" suffix=""
    [[ "$transport" == "dpdk" ]] && suffix="_dpdk"
    log_phase "exchange/md_udp ($transport)"
    local mock_args; mapfile -t mock_args < <(common_mock_args)
    local cli_args;  mapfile -t cli_args  < <(common_client_args)
    start_mock "$BUILD_DIR/mock_lat_exchange_md_udp${suffix}" "${mock_args[@]}" \
        --port "$PORT_EX_MD_UDP"
    run_client "$BUILD_DIR/bench_lat_exchange_md_udp${suffix}" \
        "${cli_args[@]}" --port "$PORT_EX_MD_UDP" --payload-sizes "$MD_UDP_PAYLOADS"
    stop_mock
}

# ── Main loop ─────────────────────────────────────────────────────────────
run_one_transport() {
    local transport="$1"
    log_phase "Transport: $transport"
    if [[ "$transport" == "kernel" ]]; then
        setup_netns
    elif [[ "$transport" == "dpdk" ]]; then
        # If we just ran kernel, give NIC-B back from netns first.
        teardown_netns
        setup_dpdk
    fi

    # Local guard so the next transport starts fresh.
    EX_WS_MOCK_RUNNING=false

    for s in "${SCENARIO_LIST[@]}"; do
        case "$s" in
            tcp)              run_tcp              "$transport" ;;
            udp)              run_udp              "$transport" ;;
            ws)               run_ws               "$transport" ;;
            exchange/market)  run_exchange_market  "$transport" ;;
            exchange/order)   run_exchange_order   "$transport" ;;
            exchange/md_udp)  run_exchange_md_udp  "$transport" ;;
            *) log_warn "unknown scenario: $s" ;;
        esac
        # Drop the shared exchange ws mock between scenarios so a stuck mock
        # cannot pollute the next bench.
        teardown_ex_ws_mock
    done

    if [[ "$transport" == "dpdk" ]]; then
        teardown_dpdk
    fi
}

# ── Cleanup trap ──────────────────────────────────────────────────────────
cleanup_on_exit() {
    local rc=$?
    set +e
    stop_mock
    teardown_ex_ws_mock
    teardown_netns
    teardown_dpdk
    if [[ "$LOOPBACK" == false && "$DRY_RUN" == false ]]; then
        # Belt and braces: kill any stray mock binaries this script might have
        # spawned and missed (e.g. SIGKILL during a payload sweep).
        pkill -f mock_lat_ 2>/dev/null || true
    fi
    return $rc
}

main() {
    parse_args "$@"
    trap cleanup_on_exit EXIT INT TERM

    printf '%s%s════════════════════════════════════════════════════════════%s\n' \
        "$BOLD" "$BLUE" "$NC"
    printf '%s%sbench_latency.sh — 6 scenarios × 2 transports%s\n' "$BOLD" "$BLUE" "$NC"
    printf '%s%s════════════════════════════════════════════════════════════%s\n' \
        "$BOLD" "$BLUE" "$NC"
    if [[ "$LOOPBACK" == true ]]; then
        printf '  mode      : %sloopback%s (no NIC, no netns, no DPDK)\n' "$CYAN" "$NC"
    else
        printf '  mode      : real-wire (%s ↔ %s)\n' "$NIC_A" "$NIC_B"
        printf '  server-ip : %s\n  local-ip  : %s\n  gateway   : %s\n' \
            "$SERVER_IP" "$LOCAL_IP" "$GATEWAY_IP"
    fi
    printf '  scenarios : %s\n  transports: %s\n  duration  : %ss (warmup %ss)\n\n' \
        "${SCENARIO_LIST[*]}" "${TRANSPORT_LIST[*]}" "$DURATION" "$WARMUP"

    preflight

    for t in "${TRANSPORT_LIST[@]}"; do
        run_one_transport "$t"
    done

    log_phase "Done"
    log_info "all scenarios complete"
    if [[ "$DRY_RUN" == true ]]; then
        log_info "(this was a dry run — nothing was actually executed)"
    else
        printf '\n%s💡%s save this run with:  sudo %s %s | tee bench-$(date +%%Y%%m%%d-%%H%%M%%S).log\n' \
            "$YELLOW" "$NC" "$0" "${ORIGINAL_ARGS[*]}"
    fi
}

main "$@"
