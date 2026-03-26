#!/bin/bash
# Overnight A/B test: per-symbol vs combined stream arrival time comparison
# Usage: nohup ./scripts/overnight_ab_test.sh &
# Output: .discuss/overnight_ab_results.csv + .discuss/overnight_ab_summary.txt

set -euo pipefail
cd "$(dirname "$0")/.."

BENCH=./build/linux/arm64/release/bench_single_vs_multi
DURATION=30       # seconds per run
INTERVAL=5        # seconds between runs
CSV=.discuss/overnight_ab_results.csv
SUMMARY=.discuss/overnight_ab_summary.txt

mkdir -p .discuss

# CSV header
if [[ ! -f "$CSV" ]]; then
    echo "timestamp,gateway_ip,symbol,paired,p50_ns,persymbol_faster_pct,combined_faster_pct,same_pct" > "$CSV"
fi

echo "=== Overnight A/B test started at $(date -Iseconds) ===" >> "$SUMMARY"
echo "Duration per run: ${DURATION}s | Interval: ${INTERVAL}s" >> "$SUMMARY"
echo "" >> "$SUMMARY"

run=0
while true; do
    run=$((run + 1))
    ts=$(date -Iseconds)
    echo "[$(date)] Run $run starting..."

    output=$("$BENCH" --duration "$DURATION" 2>&1) || true

    # Extract gateway IP
    gw=$(echo "$output" | grep "Resolved" | grep -oP '→ \K[\d.]+' || echo "unknown")

    # Parse per-symbol results
    for sym in btcusdt ethusdt solusdt; do
        # Get the block for this symbol
        block=$(echo "$output" | sed -n "/--- ${sym} ---/,/---/p" | head -20)
        paired=$(echo "$block" | grep "Paired:" | grep -oP 'Paired: \K\d+' || echo "0")
        p50=$(echo "$block" | grep "p50:" | grep -oP 'p50:\s+\K-?\d+' || echo "0")
        ps_pct=$(echo "$block" | grep "Per-symbol faster" | grep -oP '\(\K[\d.]+' || echo "0")
        cb_pct=$(echo "$block" | grep "Combined faster" | grep -oP '\(\K[\d.]+' || echo "0")
        same_pct=$(echo "$block" | grep "Same" | grep -oP '\(\K[\d.]+' || echo "0")

        echo "${ts},${gw},${sym},${paired},${p50},${ps_pct},${cb_pct},${same_pct}" >> "$CSV"
    done

    echo "[$(date)] Run $run done (gateway: $gw)"
    sleep "$INTERVAL"
done
