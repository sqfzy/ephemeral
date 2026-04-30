#!/usr/bin/env bash
# dpdk_mp_icmp_e2e.sh — Cross-process ICMP forwarding e2e (reshape
# mp-icmp-flowdir milestone A). Drives `dpdk_mp_icmp_primary` /
# `dpdk_mp_icmp_secondary`: primary registers a fake ICMP target,
# secondary IPC-forwards a synthesized Frag Needed msg, primary's
# IcmpRegistry::dispatch fires its callback and observes the new MTU.
# Validates the full IPC handler + IcmpDirectory lookup + gen-check
# round-trip without needing raw-socket ICMP injection.
#
# Picks up the two gtest binaries from the xmake build tree and runs
# them in coordinated roles:
#   1. Start primary in background, it creates the mempool + starts the port
#      then touches a ready-file.
#   2. Wait up to 10s for the ready-file.
#   3. Start secondary in foreground; it waits for the same file, then
#      runs `rte_mempool_lookup` + Platform::create_secondary + assertions.
#   4. Wait for primary to exit (it holds for EPH_MP_HOLD_SECONDS).
#   5. Exit 0 iff both binaries exited 0.
#
# Skips cleanly (exit code 77 = gtest SKIP sentinel) if:
#   - either binary is missing (build -g tests didn't run)
#   - EPH_MP_ALLOWED_DEV is empty and no vfio-pci port is bound
#   - HugePages_Free is below the needed threshold
#   - another DPDK process holds the file-prefix runtime dir
#
# The "skip if DPDK busy" branch sleeps 3 minutes and retries once (per the
# project-wide politeness rule around shared DPDK resources).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/linux/arm64/release}"

PRIMARY_BIN="$BUILD_DIR/dpdk_mp_topology_primary"
SECONDARY_BIN="$BUILD_DIR/dpdk_mp_topology_secondary"

: "${EPH_MP_FILE_PREFIX:=eph_mp_topo_$$}"
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

log() { echo "[dpdk_mp_e2e $(date +%H:%M:%S)] $*"; }

skip() {
    log "SKIP: $*"
    exit 77
}

# ── Preflight: binaries built? ──────────────────────────────────────────
[ -x "$PRIMARY_BIN"   ] || skip "primary binary not found at $PRIMARY_BIN (run: xmake build dpdk_mp_primary)"
[ -x "$SECONDARY_BIN" ] || skip "secondary binary not found at $SECONDARY_BIN (run: xmake build dpdk_mp_secondary)"

# ── Preflight: nb_rx_queues partition is sound? ─────────────────────────
# Primary/secondary 50/50 partition needs ≥ 2 queues so the two ranges
# are genuinely disjoint (mirrors the ASSERT_GE in dpdk_mp_primary.cpp).
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
    # Auto-pick the first vfio-pci device.
    EPH_MP_ALLOWED_DEV=$(ls /sys/bus/pci/drivers/vfio-pci/ 2>/dev/null | grep '^0000:' | head -1)
    export EPH_MP_ALLOWED_DEV
    log "auto-picked EPH_MP_ALLOWED_DEV=$EPH_MP_ALLOWED_DEV"
fi

# ── Preflight: hugepages ────────────────────────────────────────────────
HP_FREE=$(grep -E '^HugePages_Free:' /proc/meminfo | awk '{print $2}')
if [ "$HP_FREE" -lt "$EPH_MP_HUGEPAGES_MIN" ]; then
    skip "HugePages_Free=$HP_FREE below required $EPH_MP_HUGEPAGES_MIN"
fi

# ── Preflight: DPDK runtime dir collision? (busy-with-retry) ────────────
RUNTIME_DIR="/var/run/dpdk/$EPH_MP_FILE_PREFIX"
check_dpdk_busy() {
    # Any other DPDK process using our file-prefix would leave the runtime
    # dir present. Also look for mockex / lat_*_dpdk / internal_udp_dpdk_bench
    # in the process list as a broader "DPDK is in use" heuristic.
    if [ -d "$RUNTIME_DIR" ]; then
        return 0
    fi
    # pgrep -af matches against the full command line and excludes itself
    # automatically — robust replacement for the previous broken
    # `ps | grep -q | grep -v grep` chain (where `grep -q` produces no
    # stdout, so the downstream `grep -v` always saw empty input and the
    # whole expression was perpetually false).
    if pgrep -af '(mockex|lat_.*_dpdk|internal_udp_dpdk_bench)' >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

if check_dpdk_busy; then
    log "DPDK appears busy — waiting 3 minutes then retrying once (project politeness rule)"
    sleep 180
    if check_dpdk_busy; then
        skip "DPDK still busy after 3-minute wait"
    fi
fi

# ── Run ────────────────────────────────────────────────────────────────
rm -f "$EPH_MP_READY_FILE"
log "starting primary  (file_prefix=$EPH_MP_FILE_PREFIX dev=$EPH_MP_ALLOWED_DEV)"

"$PRIMARY_BIN" > "/tmp/${EPH_MP_FILE_PREFIX}.primary.log" 2>&1 &
PRIMARY_PID=$!

# Wait for ready marker
for _ in $(seq 1 100); do
    if [ -f "$EPH_MP_READY_FILE" ]; then break; fi
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

log "primary ready — starting secondary"
"$SECONDARY_BIN" > "/tmp/${EPH_MP_FILE_PREFIX}.secondary.log" 2>&1
SEC_RC=$?

log "secondary exited rc=$SEC_RC; waiting for primary to finish hold"
wait "$PRIMARY_PID"
PRI_RC=$?

log "primary exited rc=$PRI_RC"
rm -f "$EPH_MP_READY_FILE"

if [ "$SEC_RC" -ne 0 ] || [ "$PRI_RC" -ne 0 ]; then
    log "FAIL (primary=$PRI_RC secondary=$SEC_RC)"
    echo "--- primary log ---" >&2
    tail -60 "/tmp/${EPH_MP_FILE_PREFIX}.primary.log" >&2 || true
    echo "--- secondary log ---" >&2
    tail -60 "/tmp/${EPH_MP_FILE_PREFIX}.secondary.log" >&2 || true
    exit 1
fi

log "PASS"
exit 0
