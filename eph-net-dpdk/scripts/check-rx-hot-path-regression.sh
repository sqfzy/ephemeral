#!/usr/bin/env bash
# check-rx-hot-path-regression.sh — compare RX parser microbench against baseline
#
# Purpose: catch parse-path per-packet regressions before they leak into
# lat_* e2e benchmarks (where a 5-ns/packet creep is invisible at RTT
# scale but compounds in tail latency). Follow-up guard for the
# Tier 2 #7 baseline captured at 2026-04-23 by the lucky-giggling-kahan
# review (.artifacts/bench-rx-hot-path-20260423.txt).
#
# Usage:
#   eph-net-dpdk/scripts/check-rx-hot-path-regression.sh [--baseline PATH]
#                                                        [--threshold PCT]
#
# Defaults: baseline = .artifacts/bench-rx-hot-path-20260423.txt
#           threshold = 5 (percent)
#
# Exit codes:
#   0  all benches within threshold
#   1  at least one bench regressed >= threshold
#   2  baseline file missing / unparseable
#   3  benchmark build/run failure
#
# Production hygiene:
#   - idempotent (re-runnable; does not mutate state beyond xmake build dir)
#   - dry-run-safe (no --auto-tag / --push / --destroy flags)
#   - no host kernel mutation (unlike dpdk-setup.sh)

set -euo pipefail

# Resolve repo root from this script's location so the default
# BASELINE path resolves regardless of caller's cwd. The script
# lives at <repo>/eph-net-dpdk/scripts/<this>; go up two levels.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

# ── Config ─────────────────────────────────────────────────────────
BASELINE="${BASELINE:-.artifacts/bench-rx-hot-path-20260423.txt}"
THRESHOLD="${THRESHOLD:-5}"
BENCH_TARGET="bench_rx_hot_path"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --baseline) BASELINE="$2"; shift 2 ;;
        --threshold) THRESHOLD="$2"; shift 2 ;;
        -h|--help)
            sed -n '3,25p' "$0"
            exit 0 ;;
        *)  echo "ERR: unknown arg: $1" >&2; exit 2 ;;
    esac
done

log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" >&2; }

# ── Preflight ──────────────────────────────────────────────────────
if [[ ! -f "$BASELINE" ]]; then
    log "ERROR: baseline file not found: $BASELINE"
    log "   fix: run \`xmake run $BENCH_TARGET --benchmark_format=console\`"
    log "        once on a known-good commit and commit the output here."
    exit 2
fi

if ! command -v xmake >/dev/null; then
    log "ERROR: xmake not in PATH"
    exit 3
fi

# ── Build + run ────────────────────────────────────────────────────
log "building $BENCH_TARGET ..."
if ! xmake build "$BENCH_TARGET" >/dev/null 2>&1; then
    log "ERROR: xmake build $BENCH_TARGET failed"
    xmake build "$BENCH_TARGET" 2>&1 | tail -20 >&2
    exit 3
fi

log "running $BENCH_TARGET ..."
# Allocate every tempfile up front and register a single EXIT trap
# covering all of them. Allocating-then-trap-then-allocate-then-retrap
# briefly leaves the second batch unprotected: any unexpected exit
# between the second mktemp and the second trap (e.g. SIGTERM, ENOMEM
# from awk) leaks them. Doing it in one shot is both simpler and
# leak-proof.
CURRENT=$(mktemp)
BASELINE_TABLE=$(mktemp)
CURRENT_TABLE=$(mktemp)
trap 'rm -f "$CURRENT" "$BASELINE_TABLE" "$CURRENT_TABLE"' EXIT

if ! xmake run "$BENCH_TARGET" --benchmark_format=console > "$CURRENT" 2>&1; then
    log "ERROR: $BENCH_TARGET run failed"
    tail -20 "$CURRENT" >&2
    exit 3
fi

# ── Parse: extract "<name> <time>" from Google Benchmark output ────
# Google Benchmark line layout (console):
#   BM_ParseIpHeader/64   1.25 ns   1.25 ns   559313101 bytes_per_second=...
# We take the FIRST time column (wall time) as the canonical measurement.
extract_benches() {
    # args: $1 = file; echoes "<name> <ns>" lines
    awk '
        /^BM_[A-Za-z]/ {
            name=$1
            time=$2
            unit=$3
            # Normalize to ns if needed
            if (unit == "us") time = time * 1000
            else if (unit == "ms") time = time * 1000000
            else if (unit != "ns") next
            printf "%s %.4f\n", name, time
        }
    ' "$1"
}

extract_benches "$BASELINE"   > "$BASELINE_TABLE"
extract_benches "$CURRENT"    > "$CURRENT_TABLE"

if [[ ! -s "$BASELINE_TABLE" ]]; then
    log "ERROR: could not parse any bench lines from baseline: $BASELINE"
    exit 2
fi
if [[ ! -s "$CURRENT_TABLE" ]]; then
    log "ERROR: could not parse any bench lines from current run"
    exit 3
fi

# ── Compare ────────────────────────────────────────────────────────
printf '\n%-40s %10s %10s %8s %s\n' \
    "Benchmark" "Baseline" "Current" "Delta" "Status"
printf '%s\n' "$(printf -- '-%.0s' {1..90})"

regression_count=0
total=0
while read -r name base_ns; do
    current_ns=$(awk -v n="$name" '$1 == n { print $2; exit }' "$CURRENT_TABLE")
    if [[ -z "$current_ns" ]]; then
        printf '%-40s %10s %10s %8s %s\n' \
            "$name" "$base_ns" "MISSING" "-" "⚠️ gone"
        continue
    fi
    total=$((total + 1))
    # delta in percent = (current - baseline) / baseline * 100
    delta=$(awk -v b="$base_ns" -v c="$current_ns" \
        'BEGIN { if (b == 0) print 0; else printf "%.2f", (c - b) / b * 100 }')
    abs_delta=$(awk -v d="$delta" 'BEGIN { print (d < 0 ? -d : d) }')
    status="✅ OK"
    if awk -v d="$abs_delta" -v t="$THRESHOLD" \
        'BEGIN { exit !(d >= t) }'; then
        if awk -v d="$delta" 'BEGIN { exit !(d > 0) }'; then
            status="❌ REGRESSED"
            regression_count=$((regression_count + 1))
        else
            status="🎉 IMPROVED"
        fi
    fi
    printf '%-40s %10.4f %10.4f %+7.2f%% %s\n' \
        "$name" "$base_ns" "$current_ns" "$delta" "$status"
done < "$BASELINE_TABLE"

printf '%s\n' "$(printf -- '-%.0s' {1..90})"
log "baseline: $BASELINE"
log "threshold: ±${THRESHOLD}%"
log "compared $total benches; $regression_count regressed beyond threshold"

if [[ "$regression_count" -gt 0 ]]; then
    log "RESULT: FAIL ($regression_count regression(s))"
    exit 1
fi
log "RESULT: PASS"
exit 0
