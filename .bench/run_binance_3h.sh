#!/usr/bin/env bash
# 3-hour benchmark: pingpong + market_multi, kernel vs DPDK, real Binance
set -euo pipefail

BINDIR="/home/ec2-user/ephemeral_dev/build/linux/arm64/release"
OUTDIR="/home/ec2-user/ephemeral_dev/.bench/binance-3h"
mkdir -p "$OUTDIR"

PING_COUNT=5000
PING_INTERVAL=500
MARKET_DURATION=2700

SOCK_ARGS="--tx-cpu 2 --rx-cpu 3 --main-cpu 4"
DPDK_EAL="-a 0000:28:00.0 -l 4-7"
DPDK_NET="--local-ip 172.31.23.112 --gateway-ip 172.31.16.1 --tx-cpu 5 --rx-cpu 6 --main-cpu 7"

START=$(date '+%Y-%m-%d %H:%M:%S')
echo "=== 3-Hour Binance Benchmark Suite ==="
echo "Start: $START"
echo "Ping: ${PING_COUNT} pings × ${PING_INTERVAL}ms ≈ 42min each"
echo "Market: ${MARKET_DURATION}s duration ≈ 45min each"
echo ""

# 1. pingpong kernel (~42 min)
echo "━━━ [1/4] bench_pingpong (kernel) started at $(date '+%H:%M:%S') ━━━"
$BINDIR/bench_pingpong \
    --count $PING_COUNT --ping-interval $PING_INTERVAL $SOCK_ARGS \
    > "$OUTDIR/pingpong_kernel.txt" 2>&1 || true
echo "━━━ [1/4] bench_pingpong (kernel) done at $(date '+%H:%M:%S') ━━━"
echo ""

# 2. pingpong dpdk (~42 min)
echo "━━━ [2/4] bench_pingpong (dpdk) started at $(date '+%H:%M:%S') ━━━"
sudo $BINDIR/bench_pingpong_dpdk \
    $DPDK_EAL -- \
    --count $PING_COUNT --ping-interval $PING_INTERVAL $DPDK_NET \
    > "$OUTDIR/pingpong_dpdk.txt" 2>&1 || true
echo "━━━ [2/4] bench_pingpong (dpdk) done at $(date '+%H:%M:%S') ━━━"
echo ""

# 3. market_multi kernel (~45 min)
echo "━━━ [3/4] bench_market_multi (kernel) started at $(date '+%H:%M:%S') ━━━"
$BINDIR/bench_market_multi \
    --duration $MARKET_DURATION $SOCK_ARGS \
    > "$OUTDIR/market_multi_kernel.txt" 2>&1 || true
echo "━━━ [3/4] bench_market_multi (kernel) done at $(date '+%H:%M:%S') ━━━"
echo ""

# 4. market_multi dpdk (~45 min)
echo "━━━ [4/4] bench_market_multi (dpdk) started at $(date '+%H:%M:%S') ━━━"
sudo $BINDIR/bench_market_multi_dpdk \
    $DPDK_EAL -- \
    --duration $MARKET_DURATION $DPDK_NET \
    > "$OUTDIR/market_multi_dpdk.txt" 2>&1 || true
echo "━━━ [4/4] bench_market_multi (dpdk) done at $(date '+%H:%M:%S') ━━━"
echo ""

END=$(date '+%Y-%m-%d %H:%M:%S')
echo "=== All benchmarks complete ==="
echo "Start: $START"
echo "End:   $END"
echo "Results in: $OUTDIR"
