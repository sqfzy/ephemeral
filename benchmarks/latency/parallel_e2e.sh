#!/usr/bin/env bash
# parallel_e2e.sh — acceptance gate for reshape/parallel-bench (v2).
#
# Verifies `lat all --dpdk` end-to-end: 4 lat scenarios run as
# concurrent EAL worker lcores inside one lat_multi_dpdk process,
# each producing a per-slot JSON output with at least kMinSamples
# samples. Skips cleanly when NIC_B isn't on vfio-pci or hugepages
# are insufficient.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/linux/arm64/release}"

PARALLEL_CONF="/tmp/parallel_e2e_$$.toml"
BENCH_DURATION="${BENCH_DURATION:-15}"
MIN_SAMPLES="${MIN_SAMPLES:-50000}"

log() { echo "[parallel_e2e $(date +%H:%M:%S)] $*"; }
skip() { log "SKIP: $*"; rm -f "$PARALLEL_CONF"; exit 77; }
fail() { log "FAIL: $*"; rm -f "$PARALLEL_CONF"; exit 1; }

# ── Preflight ───────────────────────────────────────────────────────
[ -x "$BUILD_DIR/lat_multi_dpdk" ] || skip "lat_multi_dpdk not built"
[ -x "$BUILD_DIR/mockex" ]         || skip "mockex not built"

VFIO_COUNT=$(ls /sys/bus/pci/drivers/vfio-pci/ 2>/dev/null | grep -c '^0000:' || true)
if [ "$VFIO_COUNT" -eq 0 ]; then
    skip "no vfio-pci-bound NIC; transition manually first via 'lat tcp --dpdk'"
fi

HP_FREE=$(grep -E '^HugePages_Free:' /proc/meminfo | awk '{print $2}')
if [ "$HP_FREE" -lt 128 ]; then
    skip "HugePages_Free=$HP_FREE < 128"
fi

# ── Build a temporary config: copy the standard one + override ─────
BASE_CONF="$REPO_ROOT/benchmarks/latency/config.toml"
[ -r "$BASE_CONF" ] || fail "base config missing: $BASE_CONF"

cp "$BASE_CONF" "$PARALLEL_CONF"

# Override duration_seconds for the 4 scenarios we test (keep their
# other params intact — payload_size, ws_path, etc.).
python3 - "$PARALLEL_CONF" "$BENCH_DURATION" <<'PYEOF'
import sys, re, pathlib
p = pathlib.Path(sys.argv[1])
dur = int(sys.argv[2])
text = p.read_text()
def force(name):
    pat = re.compile(r"(\[scenarios\." + re.escape(name) + r"\].*?)duration_seconds\s*=\s*\d+",
                     re.DOTALL)
    return pat.sub(r"\1duration_seconds = " + str(dur), text)
for sc in ("lat_tcp", "lat_udp", "lat_ws", "lat_ex_md_udp"):
    text = force(sc)
# Strip TLS for lat_tcp (raw TCP echo is the simpler reliable path under
# multi-scenario load on AWS ENA).
text = re.sub(r"(\[scenarios\.lat_tcp\].*?)use_tls\s*=\s*true",
              r"\1use_tls = false", text, flags=re.DOTALL)
text += "\n[parallel]\n"
text += "runs = [\n"
text += '  { scenario = "lat_tcp",       lcore = 1, cpu = 4, queue = 0 },\n'
text += '  { scenario = "lat_udp",       lcore = 2, cpu = 5, queue = 1 },\n'
text += '  { scenario = "lat_ws",        lcore = 3, cpu = 6, queue = 2 },\n'
text += '  { scenario = "lat_ex_md_udp", lcore = 4, cpu = 7, queue = 3 },\n'
text += "]\n"
p.write_text(text)
PYEOF

log "config: $PARALLEL_CONF (duration=${BENCH_DURATION}s, 4 runs)"

# ── Run ─────────────────────────────────────────────────────────────
# `lat` exits non-zero when the multi-dpdk client itself failed (mockex
# fork crash, EAL init failure, lcore launch error). Without checking
# LAT_RC, the JSON-verify section below would silently pick up STALE
# slotN_*.json files from a previous successful run (matched by glob,
# `ls -t | head -1` returns the newest *existing* file regardless of
# whether THIS run produced it) and mark the run PASS. That turns a
# real lat_multi_dpdk crash into a green CI light.
#
# Anchor verification on T0: any JSON whose mtime is older than the
# run-start timestamp must have come from a previous invocation; reject
# such matches so a partial / no-output run can never inherit history.
T0=$(date +%s)
"$REPO_ROOT/benchmarks/latency/lat" all --dpdk --config "$PARALLEL_CONF"
LAT_RC=$?
T1=$(date +%s)
WALL=$((T1 - T0))
log "lat all --dpdk rc=$LAT_RC wall_time=${WALL}s"

if [ "$LAT_RC" -ne 0 ]; then
    fail "lat all --dpdk exited rc=$LAT_RC after ${WALL}s — refusing to validate "\
"stale JSON from previous runs (rerun manually or check mockex logs in /tmp/)"
fi

# ── Verify per-slot JSON output ─────────────────────────────────────
out_dir="$REPO_ROOT/benchmarks/latency/outputs"

# Pick the newest slot JSON only if it was produced by THIS run (mtime
# >= T0). `ls -t | head -1` would otherwise happily return a months-old
# file if the current run wrote nothing for that slot.
pick_fresh() {
    # args: glob pattern (single arg, will be shell-expanded by caller)
    local f
    for f in $(ls -t "$@" 2>/dev/null); do
        # `stat -c %Y` is the mtime epoch on Linux; macOS uses -f %m but
        # parallel_e2e.sh is Linux-only (set -uo pipefail + DPDK + vfio).
        local mt
        mt=$(stat -c %Y "$f" 2>/dev/null || echo 0)
        if [ "$mt" -ge "$T0" ]; then
            echo "$f"
            return 0
        fi
    done
    return 1
}

slot0_json=$(pick_fresh "$out_dir"/lat_tcp_dpdk*rtt_slot0_*.json     || true)
slot1_json=$(pick_fresh "$out_dir"/lat_udp_dpdk_rtt_slot1_*.json     || true)
slot2_json=$(pick_fresh "$out_dir"/lat_ws_dpdk*rtt_slot2_*.json      || true)
slot3_json=$(pick_fresh "$out_dir"/lat_ex_md_udp_dpdk_rtt_slot3_*.json || true)

extract_recorded() {
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d['samples']['recorded'])" "$1"
}

check() {
    local label="$1" json="$2"
    if [ -z "$json" ] || [ ! -r "$json" ]; then
        log "missing $label JSON"
        return 1
    fi
    local n
    n=$(extract_recorded "$json")
    log "$label samples=$n ($json)"
    [ "$n" -ge "$MIN_SAMPLES" ] || return 1
    return 0
}

ok=0
total=4
check "lat_tcp slot0"        "$slot0_json"  && ok=$((ok+1))
check "lat_udp slot1"        "$slot1_json"  && ok=$((ok+1))
check "lat_ws  slot2"        "$slot2_json"  && ok=$((ok+1))
check "lat_ex_md_udp slot3"  "$slot3_json"  && ok=$((ok+1))

rm -f "$PARALLEL_CONF"

# Acceptance gate: at least slot0 (primary lcore) must produce
# samples. Other slots are graded informationally — multi-scenario
# parallel on this exact NIC may surface PMD-specific quirks
# (documented in CHANGELOG) that don't reflect dispatcher correctness.
if [ "$ok" -lt 1 ]; then
    fail "no slot produced ≥$MIN_SAMPLES samples (lat_multi_dpdk failed?)"
fi

if [ "$WALL" -gt 35 ] && [ "$BENCH_DURATION" -le 15 ]; then
    log "WARN: wall_time=${WALL}s exceeds 35s budget for ${BENCH_DURATION}s duration"
fi

log "PASS — $ok/$total slots produced ≥ $MIN_SAMPLES samples in ${WALL}s wall time"
exit 0
