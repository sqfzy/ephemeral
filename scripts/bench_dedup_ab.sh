#!/bin/bash
# Controlled A/B/C/D benchmark for multi-symbol dedup strategies.
# Pre-builds all variants, then runs back-to-back with retry on DPDK failures.
#
# Usage: sudo bash scripts/bench_dedup_ab.sh [rounds] [duration_sec]
set -euo pipefail
cd "$(dirname "$0")/.."

ROUNDS=${1:-3}
DUR=${2:-20}
LOCAL_IP=172.31.23.112
GW_IP=172.31.16.1
EAL_ARGS="-a 0000:28:00.0 -l 0"
APP_ARGS="--local-ip $LOCAL_IP --gateway-ip $GW_IP --rx-cpu 1 --tx-cpu 2 --combined --duration $DUR"
MAX_RETRIES=3
HEADER="eph-net/include/eph/net/transport.hpp"
OUTDIR=.discuss
mkdir -p "$OUTDIR"
CSV="$OUTDIR/bench_dedup_$(date +%Y%m%d_%H%M%S).csv"

echo "strategy,round,samples,msgs,p50,p99,p999,max" > "$CSV"

# ── Phase 1: Pre-build all variants ──
declare -A LABELS=( [0]="baseline" [1]="opt_a_reverse" [2]="opt_b_twophase" )

for s in 0 1 2; do
    echo "[BUILD] Strategy $s (${LABELS[$s]})"
    sed -i "s/#define EPH_WS_DEDUP_STRATEGY.*/#define EPH_WS_DEDUP_STRATEGY $s/" "$HEADER"
    /home/ec2-user/.local/bin/xmake build bench_market_dpdk 2>&1 | tail -1
    cp build/linux/arm64/release/bench_market_dpdk "/tmp/bench_dpdk_s${s}"
done
# Reset header
sed -i "s/#define EPH_WS_DEDUP_STRATEGY.*/#define EPH_WS_DEDUP_STRATEGY 0/" "$HEADER"
echo "[BUILD] All variants ready"
echo ""

# ── Phase 2: Run controlled tests ──
dpdk_cleanup() {
    sudo rm -rf /var/run/dpdk 2>/dev/null || true
    sudo rm -f /dev/hugepages/rtemap_* 2>/dev/null || true
    sudo mkdir -p /var/run/dpdk/rte
    sleep 2
}

run_one() {
    local bin=$1 label=$2 round=$3
    local retry=0
    while [ $retry -lt $MAX_RETRIES ]; do
        dpdk_cleanup
        local output
        output=$(sudo timeout $((DUR + 30)) "$bin" $EAL_ARGS -- $APP_ARGS 2>&1) || true

        # Extract metrics
        local samples=$(echo "$output" | grep -oP 'samples: \K\d+' || echo "0")
        local msgs=$(echo "$output" | grep -oP 'Messages: \K\d+' || echo "0")

        if [ "$samples" = "0" ] || [ "$msgs" = "0" ]; then
            retry=$((retry + 1))
            echo "  [RETRY $retry/$MAX_RETRIES] $label r$round — connection failed"
            sleep 3
            continue
        fi

        local p50=$(echo "$output" | grep "p50:" | grep -oP '\d+(?= ns)' || echo "0")
        local p99=$(echo "$output" | grep "p99:" | grep -v "p99.9" | grep -oP '\d+(?= ns)' || echo "0")
        local p999=$(echo "$output" | grep "p99.9:" | grep -oP '\d+(?= ns)' || echo "0")
        local maxv=$(echo "$output" | grep "max:" | grep -oP '\d+(?= ns)' || echo "0")

        echo "  $label r$round: samples=$samples msgs=$msgs p50=$p50 p99=$p99 p99.9=$p999 max=$maxv"
        echo "$label,$round,$samples,$msgs,$p50,$p99,$p999,$maxv" >> "$CSV"
        return 0
    done
    echo "  [FAILED] $label r$round — all retries exhausted"
    echo "$label,$round,0,0,0,0,0,0" >> "$CSV"
}

echo "=== Starting $ROUNDS rounds, ${DUR}s each ==="
echo "CSV output: $CSV"
echo ""

for round in $(seq 1 $ROUNDS); do
    echo "── Round $round / $ROUNDS ($(date +%H:%M:%S)) ──"
    for s in 0 1 2; do
        run_one "/tmp/bench_dpdk_s${s}" "${LABELS[$s]}" "$round"
    done
    echo ""
done

# ── Phase 3: Summary ──
echo "=== Summary ==="
echo ""
for s in 0 1 2; do
    label="${LABELS[$s]}"
    data=$(grep "^$label," "$CSV" | grep -v ",0,0,0,0,0,0")
    count=$(echo "$data" | wc -l)
    if [ "$count" -gt 0 ]; then
        avg_p99=$(echo "$data" | awk -F',' '{s+=$6; n++} END {printf "%.0f", s/n}')
        avg_p50=$(echo "$data" | awk -F',' '{s+=$5; n++} END {printf "%.0f", s/n}')
        avg_msgs=$(echo "$data" | awk -F',' '{s+=$4; n++} END {printf "%.0f", s/n}')
        echo "  $label ($count runs): p50_avg=${avg_p50}ns p99_avg=${avg_p99}ns msgs_avg=$avg_msgs"
    else
        echo "  $label: NO VALID RUNS"
    fi
done
echo ""
echo "Raw data: $CSV"
