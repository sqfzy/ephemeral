#!/usr/bin/env bash
# dpdk_mp_v3_e2e.sh — Same orchestration as dpdk_mp_e2e.sh but uses the
# v3 secondary binary (Platform::attach_with_eal). Primary is unchanged
# (dpdk_mp_primary still uses v2 PlatformConfig — that migration happens
# in stage 4d).
#
# Validates plan assumption A1: a v3 secondary process can recover the
# primary-configured nb_rx_queues from `rte_eth_dev_info_get` without
# the caller restating it.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/linux/arm64/release}"

PRIMARY_BIN="$BUILD_DIR/dpdk_mp_topology_primary"
SECONDARY_BIN="$BUILD_DIR/dpdk_mp_v3_secondary"

: "${EPH_MP_FILE_PREFIX:=eph_mp_v3_e2e_$$}"
: "${EPH_MP_PORT_ID:=0}"
: "${EPH_MP_NB_RX_QUEUES:=4}"
: "${EPH_MP_LCORES:=0,1}"
: "${EPH_MP_LCORES_SEC:=2,3}"
: "${EPH_MP_ALLOWED_DEV:=}"
: "${EPH_MP_HOLD_SECONDS:=20}"
: "${EPH_MP_HUGEPAGES_MIN:=128}"

EPH_MP_READY_FILE="${EPH_MP_READY_FILE:-/tmp/${EPH_MP_FILE_PREFIX}.ready}"

export EPH_MP_FILE_PREFIX EPH_MP_PORT_ID EPH_MP_NB_RX_QUEUES \
       EPH_MP_LCORES EPH_MP_LCORES_SEC EPH_MP_ALLOWED_DEV \
       EPH_MP_HOLD_SECONDS EPH_MP_READY_FILE

log() { echo "[dpdk_mp_v3_e2e $(date +%H:%M:%S)] $*"; }
skip() { log "SKIP: $*"; exit 77; }

# ── Preflight ──────────────────────────────────────────────────────────
[ -x "$PRIMARY_BIN"   ] || skip "primary binary not found at $PRIMARY_BIN"
[ -x "$SECONDARY_BIN" ] || skip "v3 secondary binary not found at $SECONDARY_BIN"

if [ "$EPH_MP_NB_RX_QUEUES" -lt 2 ]; then
    log "FAIL: EPH_MP_NB_RX_QUEUES=$EPH_MP_NB_RX_QUEUES; need >= 2"
    exit 1
fi

if [ -z "$EPH_MP_ALLOWED_DEV" ]; then
    VFIO_COUNT=$(ls /sys/bus/pci/drivers/vfio-pci/ 2>/dev/null | grep -c '^0000:')
    [ "$VFIO_COUNT" -eq 0 ] && skip "no vfio-pci-bound NIC and EPH_MP_ALLOWED_DEV unset"
    EPH_MP_ALLOWED_DEV=$(ls /sys/bus/pci/drivers/vfio-pci/ 2>/dev/null | grep '^0000:' | head -1)
    export EPH_MP_ALLOWED_DEV
    log "auto-picked EPH_MP_ALLOWED_DEV=$EPH_MP_ALLOWED_DEV"
fi

HP_FREE=$(grep -E '^HugePages_Free:' /proc/meminfo | awk '{print $2}')
[ "$HP_FREE" -lt "$EPH_MP_HUGEPAGES_MIN" ] && skip "HugePages_Free=$HP_FREE below $EPH_MP_HUGEPAGES_MIN"

RUNTIME_DIR="/var/run/dpdk/$EPH_MP_FILE_PREFIX"
check_dpdk_busy() {
    [ -d "$RUNTIME_DIR" ] && return 0
    pgrep -af '(mockex|lat_.*_dpdk|internal_udp_dpdk_bench)' >/dev/null 2>&1 && return 0
    return 1
}
if check_dpdk_busy; then
    log "DPDK appears busy — waiting 3 minutes then retrying once"
    sleep 180
    check_dpdk_busy && skip "DPDK still busy"
fi

# ── Run ────────────────────────────────────────────────────────────────
rm -f "$EPH_MP_READY_FILE"
log "starting primary (v2 caller, file_prefix=$EPH_MP_FILE_PREFIX)"
"$PRIMARY_BIN" > "/tmp/${EPH_MP_FILE_PREFIX}.primary.log" 2>&1 &
PRIMARY_PID=$!

for _ in $(seq 1 100); do
    [ -f "$EPH_MP_READY_FILE" ] && break
    sleep 0.1
    if ! kill -0 "$PRIMARY_PID" 2>/dev/null; then
        log "primary died before ready"
        tail -30 "/tmp/${EPH_MP_FILE_PREFIX}.primary.log" >&2 || true
        wait "$PRIMARY_PID"
        exit $?
    fi
done

if [ ! -f "$EPH_MP_READY_FILE" ]; then
    log "primary did not become ready within 10s"
    kill "$PRIMARY_PID" 2>/dev/null
    wait "$PRIMARY_PID" 2>/dev/null
    tail -30 "/tmp/${EPH_MP_FILE_PREFIX}.primary.log" >&2 || true
    exit 1
fi

log "primary ready — starting v3 secondary (zero-consensus attach)"
"$SECONDARY_BIN" > "/tmp/${EPH_MP_FILE_PREFIX}.secondary.log" 2>&1
SEC_RC=$?

log "secondary exited rc=$SEC_RC; waiting for primary"
wait "$PRIMARY_PID"
PRI_RC=$?
log "primary exited rc=$PRI_RC"
rm -f "$EPH_MP_READY_FILE"

if [ "$SEC_RC" -ne 0 ] || [ "$PRI_RC" -ne 0 ]; then
    log "FAIL (primary=$PRI_RC secondary=$SEC_RC)"
    echo "--- primary log ---" >&2
    tail -60 "/tmp/${EPH_MP_FILE_PREFIX}.primary.log" >&2 || true
    echo "--- v3 secondary log ---" >&2
    tail -60 "/tmp/${EPH_MP_FILE_PREFIX}.secondary.log" >&2 || true
    exit 1
fi

log "PASS — v3 secondary attached with file_prefix only (A1 confirmed)"
exit 0
