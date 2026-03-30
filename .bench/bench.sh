#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# bench.sh — Unified benchmark runner for ephemeral
#
# Runs kernel and DPDK benchmarks **simultaneously** against the same server
# (real Binance or auto-deployed mock server), ensuring fair comparison.
#
# Usage:
#   .bench/bench.sh                              # mock server, 30s, all benchmarks
#   .bench/bench.sh --duration 300               # mock server, 5 minutes
#   .bench/bench.sh --target binance             # real Binance, 30s
#   .bench/bench.sh --target binance --duration 14400  # real Binance, 4 hours
#   .bench/bench.sh --suite pingpong             # only pingpong benchmarks
#   .bench/bench.sh --suite market               # only market_multi benchmarks
#   .bench/bench.sh --suite market-pingpong      # only market_pingpong benchmarks
#   .bench/bench.sh --suite all                  # all suites (default)
#   .bench/bench.sh --local-only                 # skip network benchmarks
#   .bench/bench.sh --help
#
# Key feature: kernel and DPDK run SIMULTANEOUSLY (not sequentially)
# against the same server endpoint, eliminating time-of-day bias.
# ═══════════════════════════════════════════════════════════════════════════
set -uo pipefail

# ── Color support ──────────────────────────────────────────────────────────
if [ -t 1 ] && [ "${NO_COLOR:-}" = "" ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
    BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BLUE=''; BOLD=''; NC=''
fi

info()  { echo -e "${BLUE}▶${NC}  $*"; }
ok()    { echo -e "${GREEN}✓${NC}  $*"; }
warn()  { echo -e "${YELLOW}⚠${NC}  $*"; }
err()   { echo -e "${RED}✗${NC}  $*" >&2; }
die()   { err "$1"; echo "   重新运行: .bench/bench.sh $ORIG_ARGS" >&2; exit 1; }

ORIG_ARGS="$*"

# ── Defaults (overridable via env or CLI) ──────────────────────────────────
PROJ_DIR="${PROJ_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
BINDIR="${BINDIR:-$PROJ_DIR/build/linux/arm64/release}"

# Mock server config
MOCK_HOST="${MOCK_HOST:-172.31.27.208}"
MOCK_PORT="${MOCK_PORT:-9443}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/aws_key.pem}"
MOCK_SERVER_PATH="${MOCK_SERVER_PATH:-/tmp/mock_server}"

# CPU pinning
SOCK_TX_CPU="${SOCK_TX_CPU:-2}"
SOCK_RX_CPU="${SOCK_RX_CPU:-3}"
SOCK_MAIN_CPU="${SOCK_MAIN_CPU:-4}"
DPDK_EAL="${DPDK_EAL:--a 0000:28:00.0 -l 4-7}"
DPDK_LOCAL_IP="${DPDK_LOCAL_IP:-172.31.23.112}"
DPDK_GATEWAY_IP="${DPDK_GATEWAY_IP:-172.31.16.1}"
DPDK_TX_CPU="${DPDK_TX_CPU:-5}"
DPDK_RX_CPU="${DPDK_RX_CPU:-6}"
DPDK_MAIN_CPU="${DPDK_MAIN_CPU:-7}"

# Benchmark parameters
DURATION=30
PING_COUNT=200
PING_INTERVAL=200
ORDER_INTERVAL=500
TARGET="mock"           # mock | binance
SUITE="all"             # all | pingpong | market | market-pingpong | local
LOCAL_ONLY=false
VERBOSE=false
DRY_RUN=false

# ── Argument parsing ──────────────────────────────────────────────────────
show_help() {
    cat <<'HELP'
bench.sh — Run kernel + DPDK benchmarks simultaneously

USAGE:
    .bench/bench.sh [OPTIONS]

OPTIONS:
    --duration SEC       Network benchmark duration (default: 30)
    --ping-count N       Pings for pingpong benchmark (default: 200)
    --target TARGET      mock (default) or binance
    --suite SUITE        all (default), pingpong, market, market-pingpong, local
    --local-only         Skip network benchmarks, run only parse/container/util
    --verbose            Show detailed progress
    --dry-run            Show what would run without executing
    --help               Show this help

EXAMPLES:
    .bench/bench.sh                                # Quick 30s test, all suites, mock server
    .bench/bench.sh --duration 300 --suite market  # 5-min market data test
    .bench/bench.sh --target binance --duration 3600  # 1-hour real Binance
    .bench/bench.sh --local-only                   # Parse/container benchmarks only

ENVIRONMENT:
    MOCK_HOST       Mock server host (default: 172.31.27.208)
    MOCK_PORT       Mock server port (default: 9443)
    SSH_KEY         SSH key for mock server deployment (default: ~/.ssh/aws_key.pem)
    BINDIR          Binary directory (default: build/linux/arm64/release)

KEY FEATURE:
    Kernel and DPDK benchmarks run SIMULTANEOUSLY against the same server,
    eliminating time-of-day traffic bias in kernel vs DPDK comparisons.
HELP
}

while [ $# -gt 0 ]; do
    case "$1" in
        --duration)     shift; DURATION="$1" ;;
        --duration=*)   DURATION="${1#*=}" ;;
        --ping-count)   shift; PING_COUNT="$1" ;;
        --ping-count=*) PING_COUNT="${1#*=}" ;;
        --target)       shift; TARGET="$1" ;;
        --target=*)     TARGET="${1#*=}" ;;
        --suite)        shift; SUITE="$1" ;;
        --suite=*)      SUITE="${1#*=}" ;;
        --local-only)   LOCAL_ONLY=true; SUITE="local" ;;
        --verbose)      VERBOSE=true ;;
        --dry-run)      DRY_RUN=true ;;
        --help|-h)      show_help; exit 0 ;;
        *) die "Unknown option: $1 (use --help)" ;;
    esac
    shift
done

cd "$PROJ_DIR"
OUTDIR=".artifacts/bench-$(date '+%Y%m%d-%H%M%S')"
mkdir -p "$OUTDIR"

SOCK_ARGS="--tx-cpu $SOCK_TX_CPU --rx-cpu $SOCK_RX_CPU --main-cpu $SOCK_MAIN_CPU"
DPDK_NET="--local-ip $DPDK_LOCAL_IP --gateway-ip $DPDK_GATEWAY_IP --tx-cpu $DPDK_TX_CPU --rx-cpu $DPDK_RX_CPU --main-cpu $DPDK_MAIN_CPU"

# ── Pre-flight checks ─────────────────────────────────────────────────────
info "Pre-flight checks"

check_bin() {
    local name=$1
    if [ ! -x "$BINDIR/$name" ]; then
        die "$name not found at $BINDIR/$name\n   Build: xmake build $name"
    fi
}

# Check required binaries based on suite
if [ "$SUITE" != "local" ]; then
    case "$SUITE" in
        pingpong)       check_bin bench_pingpong; check_bin bench_pingpong_dpdk ;;
        market)         check_bin bench_market_multi; check_bin bench_market_multi_dpdk ;;
        market-pingpong) check_bin bench_market_pingpong; check_bin bench_market_pingpong_dpdk ;;
        all)
            for b in bench_pingpong bench_pingpong_dpdk bench_market_multi bench_market_multi_dpdk \
                     bench_market_pingpong bench_market_pingpong_dpdk; do
                check_bin "$b"
            done
            ;;
    esac
fi

# Check local benchmarks
for b in bench_fix_parse bench_itch_parse bench_json_parse bench_array_book \
         bench_bq_pushpop bench_eq_pushpop bench_time bench_ws bench_tls bench_tcp_header; do
    [ -x "$BINDIR/$b" ] || warn "$b not found, skipping"
done

ok "Binaries checked"

# ── SSH helper ─────────────────────────────────────────────────────────────
SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=5"
ssh_cmd() { ssh $SSH_OPTS "ec2-user@$MOCK_HOST" "$@" 2>/dev/null; }

# ── Mock server management ─────────────────────────────────────────────────
deploy_mock_server() {
    info "Deploying mock server to $MOCK_HOST"
    scp $SSH_OPTS "$PROJ_DIR/.bench/mock/server.py" \
        "$PROJ_DIR/.bench/mock/cert.pem" \
        "$PROJ_DIR/.bench/mock/key.pem" \
        "ec2-user@$MOCK_HOST:$MOCK_SERVER_PATH/" 2>/dev/null \
        || die "Failed to deploy mock server via scp"
    ok "Mock server deployed"
}

ensure_mock_server() {
    local stress_dur=$1
    info "Ensuring mock server is running (stress-duration=${stress_dur}s, loop)"

    # Check if already running with correct config
    if ssh_cmd "ss -tlnp | grep -q $MOCK_PORT"; then
        ok "Mock server already running on port $MOCK_PORT"
        return 0
    fi

    # Deploy if needed
    if ! ssh_cmd "test -f $MOCK_SERVER_PATH/server.py"; then
        deploy_mock_server
    fi

    # Ensure websockets is installed
    ssh_cmd "python3 -c 'import websockets' 2>/dev/null" || {
        info "Installing websockets on remote host..."
        # Try local wheel first (remote may lack internet)
        local wheel="/tmp/ws_wheels/websockets-*.whl"
        if ls $wheel 2>/dev/null; then
            scp $SSH_OPTS $wheel "ec2-user@$MOCK_HOST:/tmp/" 2>/dev/null
            ssh_cmd "python3 -m pip install --user /tmp/websockets-*.whl" 2>/dev/null
        else
            ssh_cmd "python3 -m pip install --user websockets" 2>/dev/null
        fi
        ssh_cmd "python3 -c 'import websockets'" 2>/dev/null \
            || die "Failed to install websockets on $MOCK_HOST"
    }

    # Start server
    ssh_cmd "
        nohup python3 $MOCK_SERVER_PATH/server.py \
            --tls --port $MOCK_PORT \
            --mode stress --stress-duration $stress_dur --loop \
            > /tmp/mock_tls.log 2>&1 &
        for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
            ss -tlnp | grep -q $MOCK_PORT && exit 0
            sleep 1
        done
        exit 1
    " || die "Mock server failed to start on $MOCK_HOST:$MOCK_PORT\n   Check: ssh $SSH_OPTS ec2-user@$MOCK_HOST 'cat /tmp/mock_tls.log'"

    ok "Mock server started on $MOCK_HOST:$MOCK_PORT"
}

stop_mock_server() {
    ssh_cmd "killall python3 2>/dev/null" || true
}

# ── Determine server endpoint ──────────────────────────────────────────────
setup_target() {
    case "$TARGET" in
        mock)
            BENCH_HOST="$MOCK_HOST"
            BENCH_PORT="$MOCK_PORT"
            ensure_mock_server "$DURATION"
            ;;
        binance)
            BENCH_HOST="fstream.binance.com"
            BENCH_PORT="443"
            # Verify DNS is pinned (optional)
            local resolved
            resolved=$(getent hosts fstream.binance.com | awk '{print $1}' | head -1)
            if grep -q "fstream.binance.com" /etc/hosts 2>/dev/null; then
                ok "Binance DNS pinned to $resolved"
            else
                warn "Binance DNS not pinned — kernel and DPDK may connect to different nodes"
                warn "Pin with: sudo bash -c 'echo \"$resolved fstream.binance.com\" >> /etc/hosts'"
            fi
            ;;
        *) die "Unknown target: $TARGET (use 'mock' or 'binance')" ;;
    esac
    ok "Target: $TARGET ($BENCH_HOST:$BENCH_PORT)"
}

# ── Run helpers ────────────────────────────────────────────────────────────
run_local() {
    local name=$1; shift
    [ -x "$BINDIR/$name" ] || { warn "$name not found, skipping"; return; }
    if $DRY_RUN; then
        echo "  [DRY RUN] $BINDIR/$name $*"
        return
    fi
    info "[$name]"
    "$BINDIR/$name" "$@" > "$OUTDIR/${name}.txt" 2>&1
    ok "[$name] done"
}

# Run kernel and DPDK versions of a benchmark SIMULTANEOUSLY.
# Both connect to the same server at the same time — fair comparison.
run_pair() {
    local bench_name=$1; shift
    local kernel_args="$1"; shift
    local dpdk_args="$1"; shift

    local kernel_bin="$BINDIR/${bench_name}"
    local dpdk_bin="$BINDIR/${bench_name}_dpdk"

    if $DRY_RUN; then
        echo "  [DRY RUN] SIMULTANEOUS:"
        echo "    kernel: $kernel_bin $kernel_args"
        echo "    dpdk:   sudo $dpdk_bin $DPDK_EAL -- $dpdk_args"
        return
    fi

    info "[${bench_name}] kernel + DPDK simultaneously"

    # Start both in background
    $kernel_bin $kernel_args \
        > "$OUTDIR/${bench_name}_kernel.txt" 2>&1 &
    local kernel_pid=$!

    sudo $dpdk_bin $DPDK_EAL -- $dpdk_args \
        > "$OUTDIR/${bench_name}_dpdk.txt" 2>&1 &
    local dpdk_pid=$!

    # Wait for both
    local kernel_ok=true dpdk_ok=true
    wait $kernel_pid || kernel_ok=false
    wait $dpdk_pid   || dpdk_ok=false

    if $kernel_ok && $dpdk_ok; then
        ok "[${bench_name}] both done"
    else
        $kernel_ok || warn "[${bench_name}] kernel exited with error"
        $dpdk_ok   || warn "[${bench_name}] dpdk exited with error"
    fi
}

# ── Trap: cleanup on Ctrl+C ───────────────────────────────────────────────
cleanup() {
    echo ""
    warn "Interrupted — cleaning up..."
    # Kill any background benchmark processes
    jobs -p 2>/dev/null | xargs -r kill 2>/dev/null
    wait 2>/dev/null
    if [ "$TARGET" = "mock" ]; then
        stop_mock_server
    fi
    echo "   Partial results in: $OUTDIR"
    exit 130
}
trap cleanup INT TERM

# ══════════════════════════════════════════════════════════════════════════
# Main
# ══════════════════════════════════════════════════════════════════════════

echo ""
echo -e "${BOLD}═══ ephemeral benchmark suite ═══${NC}"
echo "  Time:     $(date '+%Y-%m-%d %H:%M:%S')"
echo "  Target:   $TARGET"
echo "  Suite:    $SUITE"
echo "  Duration: ${DURATION}s"
echo "  Output:   $OUTDIR"
echo ""

# ── Phase 1: Local benchmarks ─────────────────────────────────────────────
if [ "$SUITE" = "all" ] || [ "$SUITE" = "local" ]; then
    info "Phase 1: Local benchmarks (parse / container / util)"

    run_local bench_fix_parse
    run_local bench_itch_parse
    run_local bench_json_parse
    run_local bench_array_book
    run_local bench_bq_pushpop
    run_local bench_eq_pushpop
    run_local bench_time
    run_local bench_ws
    run_local bench_tls
    run_local bench_tcp_header

    echo ""
fi

if $LOCAL_ONLY; then
    echo -e "${BOLD}═══ Done (local only) ═══${NC}"
    echo "  Results: $OUTDIR"
    exit 0
fi

# ── Phase 2: Network benchmarks (kernel + DPDK simultaneous) ──────────────
setup_target

info "Phase 2: Network benchmarks (kernel + DPDK simultaneous)"
echo "  Server: $BENCH_HOST:$BENCH_PORT"
echo ""

COMMON_SOCK="--host $BENCH_HOST --port $BENCH_PORT $SOCK_ARGS"
COMMON_DPDK="--host $BENCH_HOST --port $BENCH_PORT $DPDK_NET"

if [ "$SUITE" = "all" ] || [ "$SUITE" = "pingpong" ]; then
    run_pair bench_pingpong \
        "$COMMON_SOCK --count $PING_COUNT --ping-interval $PING_INTERVAL" \
        "$COMMON_DPDK --count $PING_COUNT --ping-interval $PING_INTERVAL"
fi

if [ "$SUITE" = "all" ] || [ "$SUITE" = "market" ]; then
    # Brief pause between DPDK benchmarks for ENA driver cleanup
    [ "$SUITE" = "all" ] && sleep 3
    run_pair bench_market_multi \
        "$COMMON_SOCK --duration $DURATION" \
        "$COMMON_DPDK --duration $DURATION"
fi

if [ "$SUITE" = "all" ] || [ "$SUITE" = "market-pingpong" ]; then
    [ "$SUITE" = "all" ] && sleep 3
    # market-pingpong uses order echo — only works with mock server
    if [ "$TARGET" = "binance" ]; then
        warn "bench_market_pingpong requires mock server for order echo — skipping"
    else
        run_pair bench_market_pingpong \
            "$COMMON_SOCK --duration $DURATION --ping-interval $ORDER_INTERVAL" \
            "$COMMON_DPDK --duration $DURATION --ping-interval $ORDER_INTERVAL"
    fi
fi

# ── Phase 3: Summary ──────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}═══ Summary ═══${NC}"
echo "  Time:    $(date '+%Y-%m-%d %H:%M:%S')"
echo "  Target:  $TARGET ($BENCH_HOST:$BENCH_PORT)"
echo "  Results: $OUTDIR"
echo ""

# Extract key p50/p99 for network benchmarks
for f in "$OUTDIR"/*_kernel.txt; do
    [ -f "$f" ] || continue
    base=$(basename "$f" _kernel.txt)
    dpdk_f="$OUTDIR/${base}_dpdk.txt"
    [ -f "$dpdk_f" ] || continue

    echo "  --- $base ---"
    kp50=$(grep "RX Pipeline" -A3 "$f" 2>/dev/null | grep "p50:" | head -1 | grep -oE '[0-9]+' | tail -1)
    dp50=$(grep "RX Pipeline" -A3 "$dpdk_f" 2>/dev/null | grep "p50:" | head -1 | grep -oE '[0-9]+' | tail -1)
    kp99=$(grep "RX Pipeline" -A4 "$f" 2>/dev/null | grep "p99:" | grep -v "p99\.9" | head -1 | grep -oE '[0-9]+' | tail -1)
    dp99=$(grep "RX Pipeline" -A4 "$dpdk_f" 2>/dev/null | grep "p99:" | grep -v "p99\.9" | head -1 | grep -oE '[0-9]+' | tail -1)

    if [ -n "$kp50" ] && [ -n "$dp50" ] && [ "$dp50" -gt 0 ]; then
        ratio=$(python3 -c "print(f'{$kp50/$dp50:.1f}x')" 2>/dev/null || echo "?x")
        echo "    RX Pipeline p50: kernel=${kp50}ns  dpdk=${dp50}ns  (DPDK ${ratio})"
    fi
    if [ -n "$kp99" ] && [ -n "$dp99" ] && [ "$dp99" -gt 0 ]; then
        ratio=$(python3 -c "print(f'{$kp99/$dp99:.1f}x')" 2>/dev/null || echo "?x")
        echo "    RX Pipeline p99: kernel=${kp99}ns  dpdk=${dp99}ns  (DPDK ${ratio})"
    fi
done

echo ""
echo -e "💡 ${BOLD}Detailed results:${NC} ls $OUTDIR/"
