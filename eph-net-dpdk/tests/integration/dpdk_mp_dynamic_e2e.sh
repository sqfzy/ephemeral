#!/usr/bin/env bash
# dpdk_mp_dynamic_e2e.sh — autojoin variant of the MP e2e (reshape
# stage 3 of mp-mode2-dynamic). Drives `dpdk_mp_dynamic_primary` /
# `dpdk_mp_dynamic_secondary`, both of which call
# `Platform::join_dynamic` with the SAME PCI BDF and `nb_rx_queues`
# — no shared file_prefix, no manual MpTopology, no manual
# self_index. The first peer to call eal_init becomes primary; the
# second auto-attaches as secondary and CAS-claims slot 1.
#
# The orchestrator just controls launch order so that "first peer"
# is deterministic.
#
# Steps:
#   1. Compute the auto-derived file_prefix (eph_<sanitized BDF>).
#   2. Clean any stale runtime dir from a prior run of this same
#      autojoin test.
#   3. Start primary in background; wait up to 10s for ready-file.
#   4. Start secondary in foreground; assert role + claimed slot.
#   5. Wait for primary to exit (it holds for HOLD_SECONDS).
#   6. Exit 0 iff both binaries exited 0.
#
# Skips cleanly (exit code 77 = gtest SKIP sentinel) if:
#   - either binary is missing
#   - EPH_MP_ALLOWED_DEV is empty and no vfio-pci port is bound
#   - HugePages_Free is below the needed threshold
#   - another DPDK process is using our auto-derived runtime dir
#
# The "skip if DPDK busy" branch sleeps 3 minutes and retries once
# (per the project-wide politeness rule around shared DPDK
# resources).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/linux/arm64/release}"

PRIMARY_BIN="$BUILD_DIR/dpdk_mp_dynamic_primary"
SECONDARY_BIN="$BUILD_DIR/dpdk_mp_dynamic_secondary"

: "${EPH_MP_NB_RX_QUEUES:=4}"
: "${EPH_MP_LCORES:=0,1}"
: "${EPH_MP_LCORES_SEC:=2,3}"
: "${EPH_MP_ALLOWED_DEV:=}"
: "${EPH_MP_HOLD_SECONDS:=20}"
: "${EPH_MP_HUGEPAGES_MIN:=128}"

log() { echo "[dpdk_mp_dynamic_e2e $(date +%H:%M:%S)] $*"; }

skip() {
    log "SKIP: $*"
    exit 77
}

# ── Preflight: binaries built? ──────────────────────────────────────────
[ -x "$PRIMARY_BIN"   ] || skip "primary binary not found at $PRIMARY_BIN (run: xmake build dpdk_mp_dynamic_primary)"
[ -x "$SECONDARY_BIN" ] || skip "secondary binary not found at $SECONDARY_BIN (run: xmake build dpdk_mp_dynamic_secondary)"

# ── Preflight: nb_rx_queues partition is sound? ─────────────────────────
if [ "$EPH_MP_NB_RX_QUEUES" -lt 2 ]; then
    log "FAIL: EPH_MP_NB_RX_QUEUES=$EPH_MP_NB_RX_QUEUES; this test needs >= 2"
    exit 1
fi

# ── Preflight: vfio-pci port available? ─────────────────────────────────
if [ -z "$EPH_MP_ALLOWED_DEV" ]; then
    VFIO_COUNT=$(ls /sys/bus/pci/drivers/vfio-pci/ 2>/dev/null | grep -c '^0000:')
    if [ "$VFIO_COUNT" -eq 0 ]; then
        skip "no vfio-pci-bound NIC and EPH_MP_ALLOWED_DEV unset"
    fi
    EPH_MP_ALLOWED_DEV=$(ls /sys/bus/pci/drivers/vfio-pci/ 2>/dev/null | grep '^0000:' | head -1)
    log "auto-picked EPH_MP_ALLOWED_DEV=$EPH_MP_ALLOWED_DEV"
fi

# ── Compute auto-derived file_prefix to anchor cleanup / busy detection.
# Mirrors detail::sanitize_bdf_for_file_prefix: replace ':' '.' with '_'.
DERIVED_PREFIX="eph_$(echo "$EPH_MP_ALLOWED_DEV" | tr ':.' '_')"
RUNTIME_DIR="/var/run/dpdk/$DERIVED_PREFIX"
EPH_MP_READY_FILE="${EPH_MP_READY_FILE:-/tmp/${DERIVED_PREFIX}.ready}"
PRIMARY_LOG="/tmp/${DERIVED_PREFIX}.primary.log"
SECONDARY_LOG="/tmp/${DERIVED_PREFIX}.secondary.log"

log "BDF=$EPH_MP_ALLOWED_DEV → derived file_prefix='$DERIVED_PREFIX'"

export EPH_MP_NB_RX_QUEUES EPH_MP_LCORES EPH_MP_LCORES_SEC \
       EPH_MP_ALLOWED_DEV EPH_MP_HOLD_SECONDS EPH_MP_READY_FILE

# ── Preflight: hugepages ────────────────────────────────────────────────
HP_FREE=$(grep -E '^HugePages_Free:' /proc/meminfo | awk '{print $2}')
if [ "$HP_FREE" -lt "$EPH_MP_HUGEPAGES_MIN" ]; then
    skip "HugePages_Free=$HP_FREE below required $EPH_MP_HUGEPAGES_MIN"
fi

# ── Preflight: DPDK runtime dir collision? ──────────────────────────────
check_dpdk_proc_running() {
    # `-x` matches only against the executable name (not the full
    # cmdline) so this script's own argv (which contains the string
    # "dpdk_mp_dynamic") doesn't false-positive on itself.
    if pgrep -af '(mockex|lat_.*_dpdk|internal_udp_dpdk_bench)' >/dev/null 2>&1; then
        return 0
    fi
    if pgrep -x dpdk_mp_dynamic_primary   >/dev/null 2>&1 ||
       pgrep -x dpdk_mp_dynamic_secondary >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

# Stale runtime dir from a previous successful or crashed run is fine
# to remove **iff** no live DPDK process is using it. The auto-derived
# prefix is shared across every autojoin run on the same NIC so we
# cannot rely on a PID suffix to keep dirs disjoint the way the
# declarative-path shells do.
if [ -d "$RUNTIME_DIR" ] && ! check_dpdk_proc_running; then
    log "removing stale runtime dir '$RUNTIME_DIR' (no live DPDK proc)"
    rm -rf "$RUNTIME_DIR"
fi

if [ -d "$RUNTIME_DIR" ] || check_dpdk_proc_running; then
    log "DPDK appears busy — waiting 3 minutes then retrying once"
    sleep 180
    if [ -d "$RUNTIME_DIR" ] && ! check_dpdk_proc_running; then
        log "removing stale runtime dir '$RUNTIME_DIR' (no live DPDK proc) after retry"
        rm -rf "$RUNTIME_DIR"
    fi
    if [ -d "$RUNTIME_DIR" ] || check_dpdk_proc_running; then
        skip "DPDK still busy after 3-minute wait"
    fi
fi

# ── Run ────────────────────────────────────────────────────────────────
rm -f "$EPH_MP_READY_FILE"
log "starting primary  (lcores=$EPH_MP_LCORES nb_rx_queues=$EPH_MP_NB_RX_QUEUES)"

"$PRIMARY_BIN" > "$PRIMARY_LOG" 2>&1 &
PRIMARY_PID=$!

# Wait for ready marker
for _ in $(seq 1 100); do
    if [ -f "$EPH_MP_READY_FILE" ]; then break; fi
    sleep 0.1
    if ! kill -0 "$PRIMARY_PID" 2>/dev/null; then
        log "primary died before ready"
        tail -30 "$PRIMARY_LOG" >&2 || true
        wait "$PRIMARY_PID"
        exit $?
    fi
done

if [ ! -f "$EPH_MP_READY_FILE" ]; then
    log "primary did not become ready within 10s"
    kill "$PRIMARY_PID" 2>/dev/null
    wait "$PRIMARY_PID" 2>/dev/null
    tail -30 "$PRIMARY_LOG" >&2 || true
    exit 1
fi

log "primary ready — starting secondary (lcores=$EPH_MP_LCORES_SEC)"
"$SECONDARY_BIN" > "$SECONDARY_LOG" 2>&1
SEC_RC=$?

log "secondary exited rc=$SEC_RC; waiting for primary to finish hold"
wait "$PRIMARY_PID"
PRI_RC=$?

log "primary exited rc=$PRI_RC"
rm -f "$EPH_MP_READY_FILE"

if [ "$SEC_RC" -ne 0 ] || [ "$PRI_RC" -ne 0 ]; then
    log "FAIL (primary=$PRI_RC secondary=$SEC_RC)"
    echo "--- primary log ---" >&2
    tail -60 "$PRIMARY_LOG" >&2 || true
    echo "--- secondary log ---" >&2
    tail -60 "$SECONDARY_LOG" >&2 || true
    exit 1
fi

# Clean up the runtime dir on success so back-to-back runs of this
# same e2e do not trip the "stale dir present" guard.
if [ -d "$RUNTIME_DIR" ] && ! check_dpdk_proc_running; then
    rm -rf "$RUNTIME_DIR"
fi

log "PASS"
exit 0
