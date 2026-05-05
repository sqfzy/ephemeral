/// @file bench_helpers.hpp
/// Shared utilities for the eph-net-dpdk microbenchmarks.
///
/// Each bench used to carry its own copy of `fill_random` (byte-identical
/// across `bench_tcp_header.cpp`, `bench_udp.cpp`, `bench_multicast.cpp`).
/// Centralizing here keeps the seed-distribution-loop in one spot, so a
/// future tweak (e.g. switching to a faster PRNG, widening the byte
/// distribution to `uint8_t` directly when GCC stops the deprecation
/// noise) doesn't have to be repeated 3+ times.
///
/// Header-only by design — these helpers exist only inside the bench
/// binaries and never enter eph's public surface.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#if __has_include(<numa.h>)
#include <numa.h>
#define EPH_BENCH_HAVE_LIBNUMA 1
#else
#define EPH_BENCH_HAVE_LIBNUMA 0
#endif

namespace eph::dpdk::bench {

/// Fill `[buf, buf + len)` with bytes derived from a deterministic
/// `mt19937(seed)` stream. Different seeds produce independent
/// pseudo-random streams useful for ensuring inputs across iterations
/// of a benchmark hit different mempool / cacheline patterns.
///
/// `uniform_int_distribution<uint16_t>` is used (not `<uint8_t>`)
/// because `<uint8_t>` is officially "implementation-defined" per the
/// C++ standard and libstdc++ emits a deprecation warning. The
/// `uint16_t` form is portable and we down-cast the result to `uint8_t`.
inline void fill_random(uint8_t* buf, std::size_t len, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (std::size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(dist(rng));
    }
}

// ─────────────────────────────────────────────────────────────────────────
// NUMA helpers (T3.6 from the 2026-05-05 action list)
// ─────────────────────────────────────────────────────────────────────────
//
// Most benchmarks pin a single thread to a CPU but do NOT control which
// NUMA node the bench's hugepages / mempool / mbufs land on. On
// multi-socket hosts that lets the kernel migrate working-set memory
// across the inter-socket interconnect mid-bench, distorting latency
// numbers (cross-socket cache miss ≈ 2-3× longer than local).
//
// `numa_node_count()` returns 1 on single-socket boxes; the helpers
// below then no-op so single-socket runs aren't accidentally penalised
// by the missing-numa-node failure mode.
//
// The standard env hook is `EPH_BENCH_NUMA_NODE`:
//   - unset / empty   → no NUMA pin (legacy behaviour)
//   - "auto"          → pin to node 0 (deterministic single-socket-equivalent)
//   - integer 0..N-1  → pin to that node
//   - anything else   → bench warns and runs unpinned
//
// Library: requires `numactl` (libnuma) at compile time. If
// `<numa.h>` is missing the helpers compile to no-ops with a runtime
// stderr warning so benches still build on minimal containers.

/// Detect online NUMA node count. Returns 1 on hosts without
/// libnuma support OR on single-socket boxes.
[[nodiscard]] inline int numa_node_count() noexcept {
#if EPH_BENCH_HAVE_LIBNUMA
    if (numa_available() < 0) return 1;
    const int n = numa_max_node() + 1;
    return (n < 1) ? 1 : n;
#else
    return 1;
#endif
}

/// Pin this thread's memory allocations + execution to NUMA node `node`
/// using `numa_run_on_node` (sched affinity) plus `numa_set_preferred`
/// (allocator preference). `node < 0` is treated as "no pin" and is a
/// no-op. Out-of-range `node` triggers a stderr warning and no-op.
///
/// Returns true if the pin was applied; false on no-op / failure.
inline bool numa_pin(int node) noexcept {
#if EPH_BENCH_HAVE_LIBNUMA
    if (node < 0) return false;
    if (numa_available() < 0) return false;
    const int max_node = numa_max_node();
    if (node > max_node) {
        std::fprintf(stderr,
            "[eph::dpdk::bench] numa_pin(%d) skipped: only %d NUMA "
            "nodes online (max=%d)\n",
            node, max_node + 1, max_node);
        return false;
    }
    if (numa_run_on_node(node) != 0) {
        std::fprintf(stderr,
            "[eph::dpdk::bench] numa_pin(%d): numa_run_on_node failed\n",
            node);
        return false;
    }
    numa_set_preferred(node);
    return true;
#else
    (void)node;
    return false;
#endif
}

/// Resolve the `EPH_BENCH_NUMA_NODE` env hook. Returns:
///   - std::nullopt if unset / empty / unparsable (caller skips pin)
///   - integer node id otherwise (-1 = "auto" → 0)
[[nodiscard]] inline std::optional<int> env_numa_node() noexcept {
    const char* e = std::getenv("EPH_BENCH_NUMA_NODE");
    if (e == nullptr || *e == '\0') return std::nullopt;
    std::string_view sv{e};
    if (sv == "auto") return 0;
    try {
        return std::stoi(std::string{sv});
    } catch (...) {
        std::fprintf(stderr,
            "[eph::dpdk::bench] EPH_BENCH_NUMA_NODE='%s' unparsable; "
            "ignoring\n", e);
        return std::nullopt;
    }
}

/// One-shot helper for the bench prologue:
///   `if (auto n = env_numa_node()) numa_pin(*n);`
/// reduces to a single call. Returns the node actually pinned to, or
/// std::nullopt if no pin was applied.
inline std::optional<int> apply_env_numa_pin() noexcept {
    const auto n = env_numa_node();
    if (!n) return std::nullopt;
    if (numa_node_count() <= 1) {
        // Single-socket box: pinning is meaningless; surface a note
        // so the operator knows the env var was seen but not applied.
        std::fprintf(stderr,
            "[eph::dpdk::bench] EPH_BENCH_NUMA_NODE=%d on a single-NUMA-"
            "node host; skipping pin (no-op)\n", *n);
        return std::nullopt;
    }
    if (!numa_pin(*n)) return std::nullopt;
    return *n;
}

} // namespace eph::dpdk::bench
