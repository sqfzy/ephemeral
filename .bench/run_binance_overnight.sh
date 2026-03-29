#!/usr/bin/env bash
# Overnight benchmark: bench_market_multi kernel + DPDK vs real Binance.
# Runs until ~08:00 CST (00:00 UTC), splitting time equally between
# kernel and DPDK. Both connect to the same pinned Binance node
# (via /etc/hosts: 18.182.71.200 fstream.binance.com).
set -euo pipefail

BINDIR="/home/ec2-user/ephemeral_dev/build/linux/arm64/release"
OUTDIR="/home/ec2-user/ephemeral_dev/.bench/binance-overnight-$(date '+%Y%m%d')"
mkdir -p "$OUTDIR"

SOCK_ARGS="--tx-cpu 2 --rx-cpu 3 --main-cpu 4"
DPDK_EAL="-a 0000:28:00.0 -l 4-7"
DPDK_NET="--local-ip 172.31.23.112 --gateway-ip 172.31.16.1 --tx-cpu 5 --rx-cpu 6 --main-cpu 7"

# Calculate duration: split remaining time until 00:00 UTC equally
TARGET_UTC_EPOCH=$(date -u -d "2026-03-30 00:00:00" +%s)
NOW_EPOCH=$(date +%s)
TOTAL_REMAINING=$((TARGET_UTC_EPOCH - NOW_EPOCH))

if [ "$TOTAL_REMAINING" -le 600 ]; then
    echo "ERROR: Less than 10 minutes until target time. Aborting."
    exit 1
fi

# Reserve 60s gap between benchmarks for cleanup
PER_BENCH=$(( (TOTAL_REMAINING - 60) / 2 ))

START=$(date '+%Y-%m-%d %H:%M:%S')
echo "=== Overnight Binance Benchmark ==="
echo "Start: $START"
echo "Target: 2026-03-30 08:00 CST (00:00 UTC)"
echo "Total remaining: ${TOTAL_REMAINING}s (~$((TOTAL_REMAINING/3600))h)"
echo "Per benchmark: ${PER_BENCH}s (~$((PER_BENCH/3600))h)"
echo "Pinned node: $(getent hosts fstream.binance.com | awk '{print $1}')"
echo "Output: $OUTDIR"
echo ""

# 1. bench_market_multi kernel
echo "━━━ [1/2] bench_market_multi (kernel) started at $(date '+%H:%M:%S') ━━━"
echo "    Duration: ${PER_BENCH}s"
$BINDIR/bench_market_multi \
    --duration $PER_BENCH $SOCK_ARGS \
    > "$OUTDIR/market_multi_kernel.txt" 2>&1 || true
echo "━━━ [1/2] bench_market_multi (kernel) done at $(date '+%H:%M:%S') ━━━"
echo ""

# Recalculate remaining time for DPDK to use all of it
NOW_EPOCH2=$(date +%s)
DPDK_DURATION=$((TARGET_UTC_EPOCH - NOW_EPOCH2 - 30))
if [ "$DPDK_DURATION" -le 60 ]; then
    echo "WARNING: Only ${DPDK_DURATION}s left for DPDK, skipping."
    echo "=== Done at $(date '+%Y-%m-%d %H:%M:%S') ==="
    exit 0
fi

# 2. bench_market_multi dpdk
echo "━━━ [2/2] bench_market_multi (dpdk) started at $(date '+%H:%M:%S') ━━━"
echo "    Duration: ${DPDK_DURATION}s"
sudo $BINDIR/bench_market_multi_dpdk \
    $DPDK_EAL -- \
    --duration $DPDK_DURATION $DPDK_NET \
    > "$OUTDIR/market_multi_dpdk.txt" 2>&1 || true
echo "━━━ [2/2] bench_market_multi (dpdk) done at $(date '+%H:%M:%S') ━━━"
echo ""

END=$(date '+%Y-%m-%d %H:%M:%S')
echo "=== All benchmarks complete ==="
echo "Start: $START"
echo "End:   $END"
echo "Node:  $(getent hosts fstream.binance.com | awk '{print $1}')"
echo "Results in: $OUTDIR"
