#!/usr/bin/env bash
# Sweep `lat_rss_scaling` across conn_count ∈ {5, 20, 100} on both
# backends and tabulate the per-connection P50/P99/P99.9 distributions
# alongside the aggregate slope.
#
#     sudo ./benchmarks/latency/scripts/run_rss_scaling.sh
#
# Each (backend × conn_count) cell drives one fresh lat invocation:
# mockex re-spawns, EAL re-inits on the DPDK side, NIC state machine
# transitions are idempotent so reruns take the fast path. The script
# captures stdout from each cell, greps the report blocks out, and
# emits a single comparison table at the end.
#
# Override the conn_count list with $CONN_LIST (space-separated).
# Override duration with $DURATION (seconds, applies to all cells —
# rewrites scenarios.lat_rss_scaling.duration_seconds in a tmp config).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAT="$SCRIPT_DIR/../lat"
CONFIG="$SCRIPT_DIR/../config.toml"

CONN_LIST="${CONN_LIST:-5 20 100}"
DURATION="${DURATION:-}"
BACKENDS=("kernel" "dpdk")

if [[ $EUID -ne 0 ]]; then
    echo "must run as root (lat needs to flip NIC state)" >&2
    exit 1
fi

OUT_DIR="$(mktemp -d -t rss_scaling.XXXXXX)"
trap 'rm -rf "$OUT_DIR"' EXIT

# If DURATION is set, rewrite duration_seconds in a working copy of the
# config and pass that via --config to lat.
WORK_CONFIG="$CONFIG"
if [[ -n "$DURATION" ]]; then
    WORK_CONFIG="$OUT_DIR/config.toml"
    cp "$CONFIG" "$WORK_CONFIG"
    awk -v d="$DURATION" '
        /^\[scenarios\.lat_rss_scaling\]/ { in_sec = 1 }
        /^\[/ && !/^\[scenarios\.lat_rss_scaling\]/ { in_sec = 0 }
        in_sec && /^duration_seconds/ {
            sub(/= *[0-9]+/, "= " d)
        }
        { print }
    ' "$CONFIG" > "$WORK_CONFIG"
    echo "[..] working config: $WORK_CONFIG  (duration_seconds=$DURATION)"
fi

run_cell() {
    local backend="$1" conn="$2" out_file="$3"
    local dpdk_flag=()
    [[ "$backend" == "dpdk" ]] && dpdk_flag=(--dpdk)

    echo
    echo "──────────────────────────────────────────────────────────"
    echo "  lat rss_scaling ${dpdk_flag[*]} — conn=$conn"
    echo "──────────────────────────────────────────────────────────"
    BENCH_CONN_COUNT="$conn" "$LAT" rss_scaling \
        "${dpdk_flag[@]}" --config "$WORK_CONFIG" 2>&1 | tee "$out_file"
}

# Capture: per-conn-P50 distribution row "p50 across conns" + aggregate
# RX_one_way_ns p50 line.
extract_metric() {
    local out_file="$1" metric="$2"
    case "$metric" in
        per_conn_p50_p50)
            grep -E "^\s+P50 across conns" "$out_file" | head -1 \
                | awk '{ for(i=1;i<=NF;i++) if($i ~ /^p50=/) { sub("p50=","",$i); print $i; exit } }'
            ;;
        per_conn_p99_p50)
            grep -E "^\s+P99 across conns" "$out_file" | head -1 \
                | awk '{ for(i=1;i<=NF;i++) if($i ~ /^p50=/) { sub("p50=","",$i); print $i; exit } }'
            ;;
        agg_p50)
            awk '/RX_one_way_ns:/ { f=1; next } f && /p50/ { print $3; exit }' "$out_file"
            ;;
        agg_p999)
            awk '/RX_one_way_ns:/ { f=1; next } f && /p99\.9/ { print $3; exit }' "$out_file"
            ;;
    esac
}

declare -A R_p50_p50 R_p99_p50 R_agg_p50 R_agg_p999
for backend in "${BACKENDS[@]}"; do
    for conn in $CONN_LIST; do
        out_file="$OUT_DIR/${backend}_${conn}.log"
        run_cell "$backend" "$conn" "$out_file"
        R_p50_p50["$backend,$conn"]="$(extract_metric "$out_file" per_conn_p50_p50)"
        R_p99_p50["$backend,$conn"]="$(extract_metric "$out_file" per_conn_p99_p50)"
        R_agg_p50["$backend,$conn"]="$(extract_metric "$out_file" agg_p50)"
        R_agg_p999["$backend,$conn"]="$(extract_metric "$out_file" agg_p999)"
    done
done

echo
echo "═══════════════════════════════════════════════════════════════════"
echo "  rss_scaling sweep — one-way RX latency vs connection count"
echo "═══════════════════════════════════════════════════════════════════"
printf "\n%-10s | %-6s | %-14s | %-14s | %-12s | %-12s\n" \
       "backend" "conns" "p50-of-P50/conn" "p50-of-P99/conn" "agg p50" "agg p99.9"
printf "%-10s-+-%-6s-+-%-14s-+-%-14s-+-%-12s-+-%-12s\n" \
       "----------" "------" "--------------" "--------------" "------------" "------------"
for backend in "${BACKENDS[@]}"; do
    for conn in $CONN_LIST; do
        printf "%-10s | %-6s | %-14s | %-14s | %-12s | %-12s\n" \
            "$backend" "$conn" \
            "${R_p50_p50[$backend,$conn]:--}" \
            "${R_p99_p50[$backend,$conn]:--}" \
            "${R_agg_p50[$backend,$conn]:--}" \
            "${R_agg_p999[$backend,$conn]:--}"
    done
done
echo
echo "(units: nanoseconds. Slope = (agg-p50 at max conn − agg-p50 at min conn)/Δconn)"
echo "raw logs preserved in $OUT_DIR (until script exits) — set OUT_KEEP=1 to retain"
[[ "${OUT_KEEP:-}" == "1" ]] && trap - EXIT && echo "kept: $OUT_DIR"
