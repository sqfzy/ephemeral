#!/usr/bin/env bash
# benchmarks/latency/scripts/dpdk-setup.sh — DEPRECATED LOCATION.
#
# The canonical script lives at:
#
#     eph-net-dpdk/scripts/dpdk-setup.sh
#
# This shim only forwards arguments so anyone who memorised the old
# path still gets the current behaviour. The stale copy that used to
# live here drifted out of sync with the canonical version (missed
# noiommu fallback, fuser-based PID detection, netns-recovery hint,
# and other fixes since 2026-04-22).
#
# Migration: invoke `eph-net-dpdk/scripts/dpdk-setup.sh` directly. The
# `lat` dispatcher already does so; CLAUDE.md / production-config.md /
# ONBOARDING.md all point there. This shim will be removed in a future
# cleanup pass.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if PROJECT_DIR=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null); then
    :
else
    PROJECT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
fi
CANONICAL="$PROJECT_DIR/eph-net-dpdk/scripts/dpdk-setup.sh"

if [[ ! -x "$CANONICAL" ]]; then
    echo "ERROR: canonical dpdk-setup.sh not found at $CANONICAL" >&2
    echo "       did the eph-net-dpdk module move? check git log -- eph-net-dpdk/scripts/" >&2
    exit 1
fi

echo "[deprecated path] forwarding to $CANONICAL" >&2
exec "$CANONICAL" "$@"
