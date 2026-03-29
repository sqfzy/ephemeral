#!/usr/bin/env bash
# 6-round benchmark: socket vs DPDK, 3 scenarios each
set -euo pipefail

OUTDIR="/home/ec2-user/ephemeral_dev/.bench/rounds"
BINDIR="/home/ec2-user/ephemeral_dev/build/linux/arm64/release"
mkdir -p "$OUTDIR"

# Parameters
PING_COUNT=300
PING_INTERVAL=500
MARKET_DURATION=300
MULTI_DURATION=300
ROUNDS=6

# CPU pinning: socket uses cores 2-4, DPDK uses cores 5-7
SOCK_ARGS="--tx-cpu 2 --rx-cpu 3 --main-cpu 4"
DPDK_EAL="-a 0000:28:00.0 -l 4-7"
DPDK_NET="--local-ip 172.31.23.112 --gateway-ip 172.31.16.1 --tx-cpu 5 --rx-cpu 6 --main-cpu 7"

echo "=== Benchmark Suite: 6 rounds × 6 benchmarks ==="
echo "Start: $(date '+%Y-%m-%d %H:%M:%S')"
echo "Ping: ${PING_COUNT} pings × ${PING_INTERVAL}ms"
echo "Market: ${MARKET_DURATION}s duration"
echo "Multi: ${MULTI_DURATION}s duration"
echo ""

for round in $(seq 1 $ROUNDS); do
    echo "━━━ Round $round/$ROUNDS started at $(date '+%H:%M:%S') ━━━"

    # 1. pingpong socket
    echo "  [R${round}] bench_pingpong (socket)..."
    $BINDIR/bench_pingpong \
        --count $PING_COUNT --ping-interval $PING_INTERVAL $SOCK_ARGS \
        > "$OUTDIR/r${round}_pingpong_socket.txt" 2>&1 || true
    echo "  [R${round}] bench_pingpong (socket) done at $(date '+%H:%M:%S')"

    # 2. pingpong dpdk
    echo "  [R${round}] bench_pingpong (dpdk)..."
    sudo $BINDIR/bench_pingpong_dpdk \
        $DPDK_EAL -- \
        --count $PING_COUNT --ping-interval $PING_INTERVAL $DPDK_NET \
        > "$OUTDIR/r${round}_pingpong_dpdk.txt" 2>&1 || true
    echo "  [R${round}] bench_pingpong (dpdk) done at $(date '+%H:%M:%S')"

    # 3. market socket
    echo "  [R${round}] bench_market (socket)..."
    $BINDIR/bench_market \
        --duration $MARKET_DURATION $SOCK_ARGS \
        > "$OUTDIR/r${round}_market_socket.txt" 2>&1 || true
    echo "  [R${round}] bench_market (socket) done at $(date '+%H:%M:%S')"

    # 4. market dpdk
    echo "  [R${round}] bench_market (dpdk)..."
    sudo $BINDIR/bench_market_dpdk \
        $DPDK_EAL -- \
        --duration $MARKET_DURATION $DPDK_NET \
        > "$OUTDIR/r${round}_market_dpdk.txt" 2>&1 || true
    echo "  [R${round}] bench_market (dpdk) done at $(date '+%H:%M:%S')"

    # 5. market_multi socket
    echo "  [R${round}] bench_market_multi (socket)..."
    $BINDIR/bench_market_multi \
        --duration $MULTI_DURATION $SOCK_ARGS \
        > "$OUTDIR/r${round}_multi_socket.txt" 2>&1 || true
    echo "  [R${round}] bench_market_multi (socket) done at $(date '+%H:%M:%S')"

    # 6. market_multi dpdk
    echo "  [R${round}] bench_market_multi (dpdk)..."
    sudo $BINDIR/bench_market_multi_dpdk \
        $DPDK_EAL -- \
        --duration $MULTI_DURATION $DPDK_NET \
        > "$OUTDIR/r${round}_multi_dpdk.txt" 2>&1 || true
    echo "  [R${round}] bench_market_multi (dpdk) done at $(date '+%H:%M:%S')"

    echo "━━━ Round $round/$ROUNDS finished at $(date '+%H:%M:%S') ━━━"
    echo ""
done

echo "=== All $ROUNDS rounds complete at $(date '+%Y-%m-%d %H:%M:%S') ==="
echo "Results in: $OUTDIR"
