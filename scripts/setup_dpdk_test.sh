#!/usr/bin/env bash
set -euo pipefail

# ─────────────────────────────────────────────────────────────────────────────
# setup_dpdk_test.sh — prepare the host for running DPDK integration tests.
#
# Two modes:
#   1. With hugepages (better fidelity, needs root):
#        sudo ./scripts/setup_dpdk_test.sh --hugepages
#
#   2. Without hugepages (CI-friendly, no root needed):
#        Tests use --no-huge automatically.  Just run them directly.
#        This script is only needed if you want hugepages.
#
# The integration tests in tests/dpdk_test_env.hpp launch EAL with:
#   --no-huge --no-pci --vdev=net_null0
# so they work out of the box on any Linux machine, no setup required.
# ─────────────────────────────────────────────────────────────────────────────

HUGEPAGES=${1:-}

log() { printf "\033[1;34m[dpdk-setup]\033[0m %s\n" "$*"; }
warn() { printf "\033[1;33m[dpdk-setup]\033[0m %s\n" "$*"; }
err() { printf "\033[1;31m[dpdk-setup]\033[0m %s\n" "$*" >&2; }

# ── Check prerequisites ──────────────────────────────────────────────────

if [[ "$(uname)" != "Linux" ]]; then
    err "DPDK requires Linux.  On macOS/Windows, use a Linux VM or container."
    exit 1
fi

# ── Hugepage setup (optional) ────────────────────────────────────────────

if [[ "$HUGEPAGES" == "--hugepages" ]]; then
    if [[ $EUID -ne 0 ]]; then
        err "Hugepage setup requires root.  Run: sudo $0 --hugepages"
        exit 1
    fi

    NR_HUGEPAGES=256  # 256 x 2MB = 512MB, enough for tests.

    log "Configuring $NR_HUGEPAGES hugepages (2MB each)..."

    # Ensure hugetlbfs is mounted
    if ! mount | grep -q hugetlbfs; then
        log "Mounting hugetlbfs at /dev/hugepages..."
        mkdir -p /dev/hugepages
        mount -t hugetlbfs nodev /dev/hugepages
    fi

    echo "$NR_HUGEPAGES" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

    # Verify allocation
    ACTUAL=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    FREE=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)

    if [[ "$ACTUAL" -lt "$NR_HUGEPAGES" ]]; then
        warn "Requested $NR_HUGEPAGES hugepages but only $ACTUAL allocated."
        warn "System may have insufficient contiguous memory."
        warn "Try after a fresh reboot, or reduce NR_HUGEPAGES."
    fi

    log "Hugepages: allocated=$ACTUAL free=$FREE"
    log "Done.  Tests can now run with or without --no-huge."

else
    log "No --hugepages flag.  Tests will run in --no-huge mode (default)."
    log ""
    log "To run integration tests:"
    log "  xmake build test_platform test_tx_engine"
    log "  xmake run test_platform"
    log "  xmake run test_tx_engine"
    log ""
    log "Pure-logic tests (no DPDK runtime needed):"
    log "  xmake build test_dpdk_logic"
    log "  xmake run test_dpdk_logic"
    log ""
    log "For hugepage mode (better fidelity):"
    log "  sudo $0 --hugepages"
    log "  # Then modify dpdk_test_env.hpp to remove --no-huge"
fi
