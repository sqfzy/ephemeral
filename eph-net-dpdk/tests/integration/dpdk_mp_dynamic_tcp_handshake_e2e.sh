#!/usr/bin/env bash
# dpdk_mp_dynamic_tcp_handshake_e2e.sh — acceptance gate for the
# RSS-aware connect fix (reshape/rss-aware-connect).
#
# Drives `dpdk_mp_dynamic_tcp_handshake_primary` and
# `dpdk_mp_dynamic_tcp_handshake_secondary`. Both peers call
# Platform::join_dynamic with the SAME PCI BDF + nb_rx_queues. The
# primary spawns a kernel TCP echo mock on NIC_A, ARP-resolves the
# gateway, publishes the gw_mac to a shared file, then DPDK-connects
# to its own kernel mock from NIC_B vfio-pci. After the primary's
# ready file appears, secondary joins and DPDK-connects from its
# owned queue range. Both must complete handshake + echo round-trip.
#
# Pre-fix (commit c267b9d6 and earlier): secondary's connect hung
# silently because RSS hashed SYN-ACK to queue 0 (primary's), which
# dropped it. This e2e is the one-shot regression gate that catches
# any future regression of the same shape.
#
# Skips cleanly (exit 77) on:
#   - missing binaries / vfio-pci NIC / hugepages
#   - DPDK busy on the auto-derived runtime dir (waits 3 min once)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/linux/arm64/release}"

PRIMARY_BIN="$BUILD_DIR/dpdk_mp_dynamic_tcp_handshake_primary"
SECONDARY_BIN="$BUILD_DIR/dpdk_mp_dynamic_tcp_handshake_secondary"

: "${EPH_MP_NB_RX_QUEUES:=4}"
: "${EPH_MP_LCORES:=0,1}"
: "${EPH_MP_LCORES_SEC:=2,3}"
: "${EPH_MP_ALLOWED_DEV:=}"
: "${EPH_MP_HOLD_SECONDS:=15}"
: "${EPH_MP_HUGEPAGES_MIN:=128}"
: "${BENCH_CONFIG:=$REPO_ROOT/benchmarks/latency/config.toml}"

log() { echo "[dpdk_mp_dynamic_tcp_handshake $(date +%H:%M:%S)] $*"; }

skip() {
    log "SKIP: $*"
    exit 77
}

[ -x "$PRIMARY_BIN"   ] || skip "primary binary not found at $PRIMARY_BIN"
[ -x "$SECONDARY_BIN" ] || skip "secondary binary not found at $SECONDARY_BIN"
[ -r "$BENCH_CONFIG"  ] || skip "bench config not readable: $BENCH_CONFIG"

if [ "$EPH_MP_NB_RX_QUEUES" -lt 2 ]; then
    log "FAIL: EPH_MP_NB_RX_QUEUES=$EPH_MP_NB_RX_QUEUES; need >= 2"
    exit 1
fi

# vfio-pci NIC autopick (matches dpdk_mp_dynamic_e2e.sh)
if [ -z "$EPH_MP_ALLOWED_DEV" ]; then
    VFIO_COUNT=$(ls /sys/bus/pci/drivers/vfio-pci/ 2>/dev/null | grep -c '^0000:')
    if [ "$VFIO_COUNT" -eq 0 ]; then
        skip "no vfio-pci-bound NIC and EPH_MP_ALLOWED_DEV unset"
    fi
    EPH_MP_ALLOWED_DEV=$(ls /sys/bus/pci/drivers/vfio-pci/ 2>/dev/null | grep '^0000:' | head -1)
    log "auto-picked EPH_MP_ALLOWED_DEV=$EPH_MP_ALLOWED_DEV"
fi

DERIVED_PREFIX="eph_$(echo "$EPH_MP_ALLOWED_DEV" | tr ':.' '_')"
RUNTIME_DIR="/var/run/dpdk/$DERIVED_PREFIX"
EPH_MP_READY_FILE="${EPH_MP_READY_FILE:-/tmp/${DERIVED_PREFIX}.tcphs.ready}"
EPH_MP_GW_MAC_FILE="${EPH_MP_GW_MAC_FILE:-/tmp/${DERIVED_PREFIX}.tcphs.gw_mac}"
PRIMARY_LOG="/tmp/${DERIVED_PREFIX}.tcphs.primary.log"
SECONDARY_LOG="/tmp/${DERIVED_PREFIX}.tcphs.secondary.log"

log "BDF=$EPH_MP_ALLOWED_DEV → file_prefix='$DERIVED_PREFIX'"

export EPH_MP_NB_RX_QUEUES EPH_MP_LCORES EPH_MP_LCORES_SEC \
       EPH_MP_ALLOWED_DEV EPH_MP_HOLD_SECONDS \
       EPH_MP_READY_FILE EPH_MP_GW_MAC_FILE BENCH_CONFIG

# Hugepages
HP_FREE=$(grep -E '^HugePages_Free:' /proc/meminfo | awk '{print $2}')
if [ "$HP_FREE" -lt "$EPH_MP_HUGEPAGES_MIN" ]; then
    skip "HugePages_Free=$HP_FREE below $EPH_MP_HUGEPAGES_MIN"
fi

check_dpdk_proc_running() {
    if pgrep -af '(mockex|lat_.*_dpdk|internal_udp_dpdk_bench)' >/dev/null 2>&1; then
        return 0
    fi
    if pgrep -x dpdk_mp_dynamic_tcp_handshake_primary   >/dev/null 2>&1 ||
       pgrep -x dpdk_mp_dynamic_tcp_handshake_secondary >/dev/null 2>&1 ||
       pgrep -x dpdk_mp_dynamic_primary                 >/dev/null 2>&1 ||
       pgrep -x dpdk_mp_dynamic_secondary               >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

# Stale dirs from prior crashed/successful runs of any autojoin test
# share the same auto-derived prefix; safe to remove if no live proc.
if [ -d "$RUNTIME_DIR" ] && ! check_dpdk_proc_running; then
    log "removing stale runtime dir '$RUNTIME_DIR'"
    rm -rf "$RUNTIME_DIR"
fi

if [ -d "$RUNTIME_DIR" ] || check_dpdk_proc_running; then
    log "DPDK appears busy — waiting 3 minutes then retrying once"
    sleep 180
    if [ -d "$RUNTIME_DIR" ] && ! check_dpdk_proc_running; then
        rm -rf "$RUNTIME_DIR"
    fi
    if [ -d "$RUNTIME_DIR" ] || check_dpdk_proc_running; then
        skip "DPDK still busy after 3-minute wait"
    fi
fi

# ── Run ────────────────────────────────────────────────────────────────
rm -f "$EPH_MP_READY_FILE" "$EPH_MP_GW_MAC_FILE"
log "starting primary  (lcores=$EPH_MP_LCORES nb_rx_queues=$EPH_MP_NB_RX_QUEUES)"

"$PRIMARY_BIN" > "$PRIMARY_LOG" 2>&1 &
PRIMARY_PID=$!

# Wait for ready marker. Bumped from 10s to 15s vs dpdk_mp_dynamic_e2e
# because primary additionally bootstraps a kernel TCP echo mock + ARP
# resolves the gateway before signalling ready.
for _ in $(seq 1 150); do
    if [ -f "$EPH_MP_READY_FILE" ]; then break; fi
    sleep 0.1
    if ! kill -0 "$PRIMARY_PID" 2>/dev/null; then
        log "primary died before ready"
        tail -40 "$PRIMARY_LOG" >&2 || true
        wait "$PRIMARY_PID"
        exit $?
    fi
done

if [ ! -f "$EPH_MP_READY_FILE" ]; then
    log "primary did not become ready within 15s"
    kill "$PRIMARY_PID" 2>/dev/null
    wait "$PRIMARY_PID" 2>/dev/null
    tail -40 "$PRIMARY_LOG" >&2 || true
    exit 1
fi

[ -r "$EPH_MP_GW_MAC_FILE" ] || {
    log "FAIL: ready set but gw_mac file '$EPH_MP_GW_MAC_FILE' missing"
    kill "$PRIMARY_PID" 2>/dev/null
    wait "$PRIMARY_PID" 2>/dev/null
    tail -40 "$PRIMARY_LOG" >&2 || true
    exit 1
}

log "primary ready (gw_mac=$(cat "$EPH_MP_GW_MAC_FILE")) — starting secondary"
"$SECONDARY_BIN" > "$SECONDARY_LOG" 2>&1
SEC_RC=$?

log "secondary exited rc=$SEC_RC; waiting for primary"
wait "$PRIMARY_PID"
PRI_RC=$?
log "primary exited rc=$PRI_RC"

rm -f "$EPH_MP_READY_FILE" "$EPH_MP_GW_MAC_FILE"

if [ "$SEC_RC" -ne 0 ] || [ "$PRI_RC" -ne 0 ]; then
    log "FAIL (primary=$PRI_RC secondary=$SEC_RC)"
    echo "--- primary log ---" >&2
    tail -80 "$PRIMARY_LOG" >&2 || true
    echo "--- secondary log ---" >&2
    tail -80 "$SECONDARY_LOG" >&2 || true
    exit 1
fi

if [ -d "$RUNTIME_DIR" ] && ! check_dpdk_proc_running; then
    rm -rf "$RUNTIME_DIR"
fi

log "PASS"
exit 0
