#!/usr/bin/env bash
# Run all benchmarks — local (parse/container/util) + network (kernel/DPDK).
#
# Network benchmarks auto-manage a mock server on the remote host:
#   - Starts a fresh mock server with stress-duration matching the benchmark
#   - Each network benchmark gets its own stress cycle so burst is always hit
#   - Mock server is killed after all network benchmarks complete
#
# Usage:
#   bash .bench/run_all_bench.sh                    # defaults
#   bash .bench/run_all_bench.sh --duration 60      # longer network benchmarks
#   bash .bench/run_all_bench.sh --no-network       # skip network benchmarks
set -uo pipefail
cd /home/ec2-user/ephemeral_dev

BINDIR="build/linux/arm64/release"
OUTDIR=".artifacts/bench-all-$(date '+%Y%m%d-%H%M%S')"
mkdir -p "$OUTDIR"

# ── Configurable parameters ────────────────────────────────────────
MOCK_HOST="172.31.27.208"
MOCK_PORT="9443"
SSH_KEY="$HOME/.ssh/aws_key.pem"
SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=no -o ConnectTimeout=5"
SSH_CMD="ssh $SSH_OPTS ec2-user@$MOCK_HOST"

SOCK_ARGS="--tx-cpu 2 --rx-cpu 3 --main-cpu 4"
DPDK_EAL="-a 0000:28:00.0 -l 4-7"
DPDK_NET="--local-ip 172.31.23.112 --gateway-ip 172.31.16.1 --tx-cpu 5 --rx-cpu 6 --main-cpu 7"

NETWORK_DURATION=30   # seconds for market benchmarks
PING_COUNT=200        # pings for pingpong benchmarks
PING_INTERVAL=200     # ms between pings
SKIP_NETWORK=false

# Parse CLI args
for arg in "$@"; do
    case "$arg" in
        --duration=*) NETWORK_DURATION="${arg#*=}" ;;
        --duration)   shift; NETWORK_DURATION="$1" ;;
        --no-network) SKIP_NETWORK=true ;;
    esac
done

echo "=== All Benchmarks Suite ==="
echo "Start: $(date '+%Y-%m-%d %H:%M:%S')"
echo "Output: $OUTDIR"
echo "Network duration: ${NETWORK_DURATION}s"
echo ""

# ── Helper: run a local benchmark ──────────────────────────────────
run_local() {
    local name=$1; shift
    echo "  [$name] running..."
    $BINDIR/$name "$@" > "$OUTDIR/${name}.txt" 2>&1
    echo "  [$name] done"
}

# ── Helper: ensure mock server is running with correct stress duration ─
# Uses --loop mode so the stress cycle repeats continuously. Only starts
# the server if it's not already running — avoids the kill/restart/TIME_WAIT
# issue that caused intermittent FAIL on rapid restarts.
ensure_mock_server() {
    local stress_dur=$1
    echo "  [mock-server] ensuring running (stress-duration=${stress_dur}s, loop)..."
    $SSH_CMD "
        if ss -tlnp | grep -q $MOCK_PORT; then
            echo 'mock-server: already running'
        else
            nohup python3 /tmp/mock_server/server.py \
                --tls --port $MOCK_PORT \
                --mode stress --stress-duration $stress_dur --loop \
                > /tmp/mock_tls.log 2>&1 &
            for i in 1 2 3 4 5 6 7 8 9 10; do
                ss -tlnp | grep -q $MOCK_PORT && break
                sleep 1
            done
            ss -tlnp | grep -q $MOCK_PORT && echo 'mock-server: OK' || echo 'mock-server: FAIL'
        fi
    " 2>/dev/null
}

stop_mock_server() {
    $SSH_CMD "killall python3 2>/dev/null" 2>/dev/null || true
}

# ══════════════════════════════════════════════════════════════════════
# Phase 1: Local benchmarks (no network dependency)
# ══════════════════════════════════════════════════════════════════════

echo "── Parse benchmarks ──"
run_local bench_fix_parse
run_local bench_itch_parse
run_local bench_json_parse
run_local bench_array_book

echo "── Container benchmarks ──"
run_local bench_bq_pushpop
run_local bench_eq_pushpop
run_local bench_bq_throughput
run_local bench_eq_throughput

echo "── Utility benchmarks ──"
run_local bench_time
run_local bench_ws
run_local bench_tls
run_local bench_tcp_header

if $SKIP_NETWORK; then
    echo ""
    echo "── Network benchmarks SKIPPED (--no-network) ──"
    echo ""
    echo "=== All done at $(date '+%Y-%m-%d %H:%M:%S') ==="
    echo "Results in: $OUTDIR"
    exit 0
fi

# ══════════════════════════════════════════════════════════════════════
# Phase 2: Network benchmarks (mock server)
# ══════════════════════════════════════════════════════════════════════

# Start mock server once with stress cycle = benchmark duration.
# --loop makes the cycle repeat, so every 30s benchmark that runs
# during the cycle will experience all 5 phases including burst.
# No per-benchmark restart needed — avoids TIME_WAIT port conflicts.
ensure_mock_server "$NETWORK_DURATION"

echo "── Network benchmarks (kernel, mock server) ──"

echo "  [bench_pingpong kernel] running..."
$BINDIR/bench_pingpong \
    --host $MOCK_HOST --port $MOCK_PORT \
    --count $PING_COUNT --ping-interval $PING_INTERVAL $SOCK_ARGS \
    > "$OUTDIR/bench_pingpong_kernel.txt" 2>&1
echo "  [bench_pingpong kernel] done"

echo "  [bench_market_multi kernel] running..."
$BINDIR/bench_market_multi \
    --host $MOCK_HOST --port $MOCK_PORT \
    --duration $NETWORK_DURATION $SOCK_ARGS \
    > "$OUTDIR/bench_market_multi_kernel.txt" 2>&1
echo "  [bench_market_multi kernel] done"

echo "  [bench_market_pingpong kernel] running..."
$BINDIR/bench_market_pingpong \
    --host $MOCK_HOST --port $MOCK_PORT \
    --duration $NETWORK_DURATION --ping-interval 500 $SOCK_ARGS \
    > "$OUTDIR/bench_market_pingpong_kernel.txt" 2>&1
echo "  [bench_market_pingpong kernel] done"

echo "── Network benchmarks (DPDK, mock server) ──"

echo "  [bench_pingpong dpdk] running..."
sudo $BINDIR/bench_pingpong_dpdk $DPDK_EAL -- \
    --host $MOCK_HOST --port $MOCK_PORT \
    --count $PING_COUNT --ping-interval $PING_INTERVAL $DPDK_NET \
    > "$OUTDIR/bench_pingpong_dpdk.txt" 2>&1
echo "  [bench_pingpong dpdk] done"

echo "  [bench_market_multi dpdk] running..."
sudo $BINDIR/bench_market_multi_dpdk $DPDK_EAL -- \
    --host $MOCK_HOST --port $MOCK_PORT \
    --duration $NETWORK_DURATION $DPDK_NET \
    > "$OUTDIR/bench_market_multi_dpdk.txt" 2>&1
echo "  [bench_market_multi dpdk] done"

echo "  [bench_market_pingpong dpdk] running..."
sudo $BINDIR/bench_market_pingpong_dpdk $DPDK_EAL -- \
    --host $MOCK_HOST --port $MOCK_PORT \
    --duration $NETWORK_DURATION --ping-interval 500 $DPDK_NET \
    > "$OUTDIR/bench_market_pingpong_dpdk.txt" 2>&1
echo "  [bench_market_pingpong dpdk] done"

stop_mock_server

echo ""
echo "=== All done at $(date '+%Y-%m-%d %H:%M:%S') ==="
echo "Results in: $OUTDIR"
