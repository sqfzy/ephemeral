#!/usr/bin/env bash
# bench_latency.sh — run the latency bench matrix from bench.conf.
#
# Always real-wire back-to-back between two NICs on the same host:
#   mock server binds NIC-A in the host namespace, bench client runs in
#   a `bench_ns` netns that owns NIC-B. For the DPDK transport NIC-B is
#   rebound to vfio-pci via scripts/dpdk-setup.sh between phases.
#
# Configuration lives in benchmarks/latency/bench.conf. Edit it once
# with your NIC / IP / CPU layout and then:
#
#     sudo ./scripts/bench_latency.sh
#     sudo ./scripts/bench_latency.sh --scenarios tcp
#     sudo ./scripts/bench_latency.sh --transports dpdk
#     sudo ./scripts/bench_latency.sh --dry-run
#
# Environment variables (BENCH_<NAME>) override config-file values.
# Bench output goes to stdout; pipe to `tee` if you want a record.

set -euo pipefail

# ── Paths ─────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="${BENCH_CONFIG:-$PROJECT_DIR/benchmarks/latency/bench.conf}"

# ── Color & logging ──────────────────────────────────────────────────
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
die() { log_error "$1"; [[ -n "${2:-}" ]] && printf '   %s\n' "$2" >&2; exit 1; }

# ── Load config ──────────────────────────────────────────────────────
[[ -f "$CONFIG_FILE" ]] || die "config not found: $CONFIG_FILE" \
    "Copy benchmarks/latency/bench.conf and edit it for your host."

# shellcheck disable=SC1090
source "$CONFIG_FILE"

# Let BENCH_<NAME> env vars override config values.
: "${NIC_A:=${BENCH_NIC_A:-}}"
: "${NIC_B:=${BENCH_NIC_B:-}}"
: "${SERVER_IP:=${BENCH_SERVER_IP:-}}"
: "${LOCAL_IP:=${BENCH_LOCAL_IP:-}}"
: "${GATEWAY_IP:=${BENCH_GATEWAY_IP:-}}"
: "${CLIENT_CPU:=${BENCH_CLIENT_CPU:-2}}"
: "${MOCK_CPU:=${BENCH_MOCK_CPU:-4}}"
: "${EAL_CORES:=${BENCH_EAL_CORES:-0,1}}"
: "${ALLOW_NON_ISOLATED:=${BENCH_ALLOW_NON_ISOLATED:-false}}"
: "${DURATION:=${BENCH_DURATION:-10}}"
: "${WARMUP:=${BENCH_WARMUP:-2}}"
: "${SERVER_WORK_NS:=${BENCH_SERVER_WORK_NS:-200}}"
: "${SCENARIOS:=${BENCH_SCENARIOS:-tcp,udp,ws,exchange/market,exchange/order,exchange/md_udp}}"
: "${TRANSPORTS:=${BENCH_TRANSPORTS:-kernel,dpdk}}"

# ── Parse overrides (only 4 flags — everything else lives in config) ─
DRY_RUN=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --scenarios)  SCENARIOS="$2"; shift 2 ;;
        --transports) TRANSPORTS="$2"; shift 2 ;;
        --dry-run)    DRY_RUN=true;   shift ;;
        -h|--help)
            cat <<EOF
${BOLD}bench_latency.sh${NC} — 6-scenario × 2-transport latency runner

Config file : $CONFIG_FILE
Usage       : sudo $0 [--scenarios CSV] [--transports CSV] [--dry-run]

Flags just filter the run; all other settings live in bench.conf.
EOF
            exit 0 ;;
        *) die "unknown option: $1" "Run $0 --help" ;;
    esac
done

# ── Resolve lists ────────────────────────────────────────────────────
IFS=',' read -ra SCENARIO_LIST  <<< "$SCENARIOS"
IFS=',' read -ra TRANSPORT_LIST <<< "$TRANSPORTS"

# Port allocation — distinct per scenario so a stuck mock can't bleed.
PORT_TCP=19101
PORT_UDP=19102
PORT_WS=19103
PORT_EX_WS=19104
PORT_EX_MD_UDP=19105

# ── Preflight ────────────────────────────────────────────────────────
detect_build_dir() {
    for d in \
        "$PROJECT_DIR/build/linux/arm64/release" \
        "$PROJECT_DIR/build/linux/aarch64/release" \
        "$PROJECT_DIR/build/linux/x86_64/release"; do
        if [[ -d "$d" ]]; then echo "$d"; return; fi
    done
    die "cannot auto-detect xmake build dir under $PROJECT_DIR/build/"
}
BUILD_DIR=$(detect_build_dir)

preflight() {
    log_phase "Preflight"
    [[ $EUID -eq 0 ]] || die "must run as root" "sudo $0 $*"
    log_info "running as root"
    log_info "config file : $CONFIG_FILE"
    log_info "build dir   : $BUILD_DIR"

    [[ -n "$NIC_A"      ]] || die "NIC_A is not set in $CONFIG_FILE"
    [[ -n "$NIC_B"      ]] || die "NIC_B is not set in $CONFIG_FILE"
    [[ -n "$SERVER_IP"  ]] || die "SERVER_IP is not set in $CONFIG_FILE"
    [[ -n "$LOCAL_IP"   ]] || die "LOCAL_IP is not set in $CONFIG_FILE"
    [[ -n "$GATEWAY_IP" ]] || die "GATEWAY_IP is not set in $CONFIG_FILE"

    ip link show "$NIC_A" &>/dev/null || die "NIC-A '$NIC_A' not found"
    if ip link show "$NIC_B" &>/dev/null; then
        log_info "NICs: $NIC_A (server) + $NIC_B (client)"
    else
        log_warn "NIC-B '$NIC_B' not in kernel — may already be DPDK-bound"
    fi

    local prefix
    prefix=$(ip -4 addr show "$NIC_B" 2>/dev/null \
        | grep -oE 'inet [0-9./]+' | head -1 | awk '{print $2}' \
        | grep -oE '/[0-9]+' || true)
    LOCAL_CIDR="${LOCAL_IP}${prefix:-/20}"
    log_info "client side: $NIC_B = $LOCAL_CIDR  →  $SERVER_IP"

    # isolcpus warning.
    local isolated=""
    [[ -r /sys/devices/system/cpu/isolated ]] && isolated=$(< /sys/devices/system/cpu/isolated)
    if [[ -z "$isolated" ]]; then
        log_warn "no isolcpus configured — pinning will fail without ALLOW_NON_ISOLATED=true"
    else
        log_info "isolcpus: $isolated"
    fi

    # Required binaries.
    local needed=() suffix
    for s in "${SCENARIO_LIST[@]}"; do
        for t in "${TRANSPORT_LIST[@]}"; do
            suffix=""; [[ "$t" == "dpdk" ]] && suffix="_dpdk"
            case "$s" in
                tcp)             needed+=("mock_lat_tcp" "bench_lat_tcp${suffix}") ;;
                udp)             needed+=("mock_lat_udp" "bench_lat_udp${suffix}") ;;
                ws)              needed+=("mock_lat_ws"  "bench_lat_ws${suffix}")  ;;
                exchange/market) needed+=("mock_lat_exchange_ws" "bench_lat_exchange_market${suffix}") ;;
                exchange/order)  needed+=("mock_lat_exchange_ws" "bench_lat_exchange_order${suffix}") ;;
                exchange/md_udp) needed+=("mock_lat_exchange_md_udp" "bench_lat_exchange_md_udp${suffix}") ;;
                *) die "unknown scenario: $s" ;;
            esac
        done
    done
    local missing=()
    for b in "${needed[@]}"; do
        [[ -x "$BUILD_DIR/$b" ]] || missing+=("$b")
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "missing binaries:"
        printf '   %s\n' "${missing[@]}" >&2
        die "binaries missing" "Build with: xmake build ${missing[*]}"
    fi
    log_info "binaries OK (${#needed[@]} referenced)"
}

# ── Netns / DPDK state ───────────────────────────────────────────────
NETNS_CREATED=false
DPDK_BOUND_BY_US=false
NIC_B_PCI=""
MOCK_PID=""
EX_WS_MOCK_RUNNING=false
CURRENT_TRANSPORT=""

setup_netns() {
    [[ "$DRY_RUN" == true ]] && return
    log_phase "Setup bench_ns"
    ip netns add bench_ns 2>/dev/null || true
    ip link set "$NIC_B" netns bench_ns
    ip netns exec bench_ns ip addr add "$LOCAL_CIDR" dev "$NIC_B"
    ip netns exec bench_ns ip link set "$NIC_B" up
    ip netns exec bench_ns ip link set lo up
    ip netns exec bench_ns ip route add default via "$GATEWAY_IP" dev "$NIC_B"
    NETNS_CREATED=true
    log_info "bench_ns: $NIC_B → $GATEWAY_IP"
}

teardown_netns() {
    [[ "$NETNS_CREATED" == false ]] && return
    ip netns exec bench_ns ip link set "$NIC_B" netns 1 2>/dev/null || true
    ip netns del bench_ns 2>/dev/null || true
    ip link set "$NIC_B" up 2>/dev/null || true
    ip addr add "$LOCAL_CIDR" dev "$NIC_B" 2>/dev/null || true
    NETNS_CREATED=false
}

setup_dpdk() {
    [[ "$DRY_RUN" == true ]] && return
    log_phase "Bind $NIC_B → vfio-pci"
    NIC_B_PCI=$(basename "$(readlink -f "/sys/class/net/${NIC_B}/device" 2>/dev/null)" 2>/dev/null || true)
    if [[ -z "$NIC_B_PCI" || "$NIC_B_PCI" == "device" ]]; then
        # Maybe already bound.
        if [[ -f "$PROJECT_DIR/.dpdk_state" ]]; then
            # shellcheck source=/dev/null
            source "$PROJECT_DIR/.dpdk_state"
            NIC_B_PCI="$DPDK_PCI"
            log_info "$NIC_B already DPDK-bound (PCI=$NIC_B_PCI)"
            return
        fi
        die "cannot determine PCI for $NIC_B" "Run scripts/dpdk-setup.sh manually first."
    fi
    if [[ -x "$SCRIPT_DIR/dpdk-setup.sh" ]]; then
        DPDK_PCI="$NIC_B_PCI" DPDK_IFACE="$NIC_B" "$SCRIPT_DIR/dpdk-setup.sh" -y
        DPDK_BOUND_BY_US=true
        log_info "$NIC_B (PCI $NIC_B_PCI) bound to vfio-pci"
    fi
}

teardown_dpdk() {
    [[ "$DPDK_BOUND_BY_US" == false ]] && return
    [[ "$DRY_RUN" == true ]] && return
    [[ -x "$SCRIPT_DIR/dpdk-teardown.sh" ]] || return
    DPDK_PCI="$NIC_B_PCI" DPDK_IFACE="$NIC_B" "$SCRIPT_DIR/dpdk-teardown.sh" || true
    DPDK_BOUND_BY_US=false
}

# ── Mock lifecycle ───────────────────────────────────────────────────
start_mock() {
    local cmd=("$@")
    if [[ "$DRY_RUN" == true ]]; then
        printf '  [dry-run] mock: %s\n' "${cmd[*]}"
        MOCK_PID=""
        return
    fi
    "${cmd[@]}" &
    MOCK_PID=$!
    sleep 0.4  # give it time to bind + listen
}

stop_mock() {
    if [[ -n "$MOCK_PID" ]]; then
        kill -TERM "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
        MOCK_PID=""
    fi
    sleep 0.2
}

# ── Client args ──────────────────────────────────────────────────────
common_client_args() {
    local out=(
        --server-ip "$SERVER_IP"
        --client-cpu "$CLIENT_CPU"
        --warmup "$WARMUP"
        --duration "$DURATION"
    )
    [[ "$ALLOW_NON_ISOLATED" == true ]] && out+=(--allow-non-isolated)
    [[ "$CURRENT_TRANSPORT" == "dpdk" ]] && \
        out+=(--local-ip "$LOCAL_IP" --gateway-ip "$GATEWAY_IP")
    printf '%s\n' "${out[@]}"
}

common_mock_args() {
    local out=(
        --server-ip "$SERVER_IP"
        --mock-cpu "$MOCK_CPU"
        --server-work-ns "$SERVER_WORK_NS"
    )
    [[ "$ALLOW_NON_ISOLATED" == true ]] && out+=(--allow-non-isolated)
    printf '%s\n' "${out[@]}"
}

# Run a bench client: prepend EAL args for DPDK, wrap in netns for kernel.
run_client() {
    local cmd=("$@")
    if [[ "$CURRENT_TRANSPORT" == "dpdk" ]]; then
        local pci="$NIC_B_PCI"
        if [[ -z "$pci" && -f "$PROJECT_DIR/.dpdk_state" ]]; then
            # shellcheck source=/dev/null
            source "$PROJECT_DIR/.dpdk_state"
            pci="$DPDK_PCI"
        fi
        local binary="${cmd[0]}"
        local app_args=("${cmd[@]:1}")
        cmd=("$binary" -a "$pci" -l "$EAL_CORES" --)
        cmd+=("${app_args[@]}")
    fi

    if [[ "$DRY_RUN" == true ]]; then
        if [[ "$CURRENT_TRANSPORT" == "kernel" ]]; then
            printf '  [dry-run] client: ip netns exec bench_ns %s\n' "${cmd[*]}"
        else
            printf '  [dry-run] client: %s\n' "${cmd[*]}"
        fi
        return
    fi
    if [[ "$CURRENT_TRANSPORT" == "kernel" ]]; then
        ip netns exec bench_ns "${cmd[@]}" || log_warn "client returned $?"
    else
        "${cmd[@]}" || log_warn "client returned $?"
    fi
}

# ── Per-scenario runners ─────────────────────────────────────────────
run_tcp() {
    local suffix=""; [[ "$CURRENT_TRANSPORT" == "dpdk" ]] && suffix="_dpdk"
    log_phase "tcp ($CURRENT_TRANSPORT)"
    local mock_args; mapfile -t mock_args < <(common_mock_args)
    local cli_args;  mapfile -t cli_args  < <(common_client_args)

    # tcp mock bakes msg_size into its accept loop → restart per payload.
    IFS=',' read -ra payloads <<< "$TCP_PAYLOADS"
    for p in "${payloads[@]}"; do
        log_info "tcp payload=$p"
        start_mock "$BUILD_DIR/mock_lat_tcp" "${mock_args[@]}" \
            --port "$PORT_TCP" --msg-size "$p"
        run_client "$BUILD_DIR/bench_lat_tcp${suffix}" "${cli_args[@]}" \
            --port "$PORT_TCP" --payload-sizes "$p"
        stop_mock
    done
}

run_udp() {
    local suffix=""; [[ "$CURRENT_TRANSPORT" == "dpdk" ]] && suffix="_dpdk"
    log_phase "udp ($CURRENT_TRANSPORT)"
    local mock_args; mapfile -t mock_args < <(common_mock_args)
    local cli_args;  mapfile -t cli_args  < <(common_client_args)

    start_mock "$BUILD_DIR/mock_lat_udp" "${mock_args[@]}" --port "$PORT_UDP"
    run_client "$BUILD_DIR/bench_lat_udp${suffix}" "${cli_args[@]}" \
        --port "$PORT_UDP" --payload-sizes "$UDP_PAYLOADS"
    stop_mock
}

run_ws() {
    local suffix=""; [[ "$CURRENT_TRANSPORT" == "dpdk" ]] && suffix="_dpdk"
    log_phase "ws ($CURRENT_TRANSPORT)"
    local mock_args; mapfile -t mock_args < <(common_mock_args)
    local cli_args;  mapfile -t cli_args  < <(common_client_args)

    start_mock "$BUILD_DIR/mock_lat_ws" "${mock_args[@]}" --port "$PORT_WS"
    run_client "$BUILD_DIR/bench_lat_ws${suffix}" "${cli_args[@]}" \
        --port "$PORT_WS" --payload-sizes "$WS_PAYLOADS"
    stop_mock
}

ensure_ex_ws_mock() {
    [[ "$EX_WS_MOCK_RUNNING" == true ]] && return
    local mock_args; mapfile -t mock_args < <(common_mock_args)
    start_mock "$BUILD_DIR/mock_lat_exchange_ws" "${mock_args[@]}" \
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
    local suffix=""; [[ "$CURRENT_TRANSPORT" == "dpdk" ]] && suffix="_dpdk"
    log_phase "exchange/market ($CURRENT_TRANSPORT)"
    ensure_ex_ws_mock
    local cli_args; mapfile -t cli_args < <(common_client_args)
    run_client "$BUILD_DIR/bench_lat_exchange_market${suffix}" \
        "${cli_args[@]}" --port "$PORT_EX_WS"
}

run_exchange_order() {
    local suffix=""; [[ "$CURRENT_TRANSPORT" == "dpdk" ]] && suffix="_dpdk"
    log_phase "exchange/order ($CURRENT_TRANSPORT)"
    ensure_ex_ws_mock
    local cli_args; mapfile -t cli_args < <(common_client_args)
    run_client "$BUILD_DIR/bench_lat_exchange_order${suffix}" \
        "${cli_args[@]}" --port "$PORT_EX_WS" --inflights "$INFLIGHTS"
}

run_exchange_md_udp() {
    local suffix=""; [[ "$CURRENT_TRANSPORT" == "dpdk" ]] && suffix="_dpdk"
    log_phase "exchange/md_udp ($CURRENT_TRANSPORT)"
    local mock_args; mapfile -t mock_args < <(common_mock_args)
    local cli_args;  mapfile -t cli_args  < <(common_client_args)
    start_mock "$BUILD_DIR/mock_lat_exchange_md_udp" "${mock_args[@]}" \
        --port "$PORT_EX_MD_UDP"
    run_client "$BUILD_DIR/bench_lat_exchange_md_udp${suffix}" \
        "${cli_args[@]}" --port "$PORT_EX_MD_UDP" --payload-sizes "$MD_UDP_PAYLOADS"
    stop_mock
}

# ── Transport driver ─────────────────────────────────────────────────
run_one_transport() {
    CURRENT_TRANSPORT="$1"
    log_phase "Transport: $CURRENT_TRANSPORT"
    if [[ "$CURRENT_TRANSPORT" == "kernel" ]]; then
        setup_netns
    else
        teardown_netns  # give NIC-B back from bench_ns first
        setup_dpdk
    fi

    EX_WS_MOCK_RUNNING=false

    for s in "${SCENARIO_LIST[@]}"; do
        case "$s" in
            tcp)             run_tcp ;;
            udp)             run_udp ;;
            ws)              run_ws ;;
            exchange/market) run_exchange_market ;;
            exchange/order)  run_exchange_order ;;
            exchange/md_udp) run_exchange_md_udp ;;
            *) log_warn "unknown scenario: $s" ;;
        esac
        teardown_ex_ws_mock  # never carry the shared mock across scenarios
    done

    [[ "$CURRENT_TRANSPORT" == "dpdk" ]] && teardown_dpdk
}

# ── Cleanup trap ─────────────────────────────────────────────────────
cleanup_on_exit() {
    local rc=$?
    set +e
    stop_mock
    teardown_ex_ws_mock
    teardown_netns
    teardown_dpdk
    [[ "$DRY_RUN" == false ]] && pkill -f mock_lat_ 2>/dev/null || true
    return $rc
}
trap cleanup_on_exit EXIT INT TERM

# ── Main ─────────────────────────────────────────────────────────────
printf '%s%s════════════════════════════════════════════════════════════%s\n' "$BOLD" "$BLUE" "$NC"
printf '%s%sbench_latency.sh — 6 scenarios × 2 transports%s\n' "$BOLD" "$BLUE" "$NC"
printf '%s%s════════════════════════════════════════════════════════════%s\n\n' "$BOLD" "$BLUE" "$NC"

preflight

printf '\n  NICs        : %s (server) ↔ %s (client)\n' "$NIC_A" "$NIC_B"
printf '  server-ip   : %s\n  local-ip    : %s\n  gateway     : %s\n' \
    "$SERVER_IP" "$LOCAL_IP" "$GATEWAY_IP"
printf '  scenarios   : %s\n  transports  : %s\n  duration    : %ss (warmup %ss)\n\n' \
    "${SCENARIO_LIST[*]}" "${TRANSPORT_LIST[*]}" "$DURATION" "$WARMUP"

for t in "${TRANSPORT_LIST[@]}"; do
    run_one_transport "$t"
done

log_phase "Done"
log_info "all scenarios complete"
