#!/bin/bash
# Migration script: flat include/eph/ → split into eph-base, eph-containers, eph-utils
# Run from project root. Moves headers, preserves #include paths.

set -euo pipefail

# ── eph-base ──────────────────────────────────────────────
mkdir -p eph-base/include/eph
mv include/eph/platform.hpp  eph-base/include/eph/
mv include/eph/types.hpp     eph-base/include/eph/

# ── eph-containers ────────────────────────────────────────
mkdir -p eph-containers/include/eph/core
mv include/eph/core/queue.hpp       eph-containers/include/eph/core/
mv include/eph/core/ring_buffer.hpp eph-containers/include/eph/core/

# ── eph-utils ─────────────────────────────────────────────
mkdir -p eph-utils/include/eph/core
mkdir -p eph-utils/include/eph/benchmark
mv include/eph/core/shared_memory.hpp eph-utils/include/eph/core/
mv include/eph/core/socket.hpp        eph-utils/include/eph/core/
mv include/eph/core/json_buf.hpp      eph-utils/include/eph/core/
mv include/eph/benchmark/*.hpp         eph-utils/include/eph/benchmark/

# ── Cleanup empty dirs ────────────────────────────────────
rmdir include/eph/benchmark 2>/dev/null || true
rmdir include/eph/core      2>/dev/null || true
rmdir include/eph            2>/dev/null || true
rmdir include                2>/dev/null || true

echo "✅ Migration complete. Directory layout:"
find eph-base eph-containers eph-utils -name '*.hpp' | sort
