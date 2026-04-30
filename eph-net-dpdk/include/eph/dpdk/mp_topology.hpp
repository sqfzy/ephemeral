#pragma once

/// @file mp_topology.hpp
/// High-level multi-process resource topology — the user-facing input
/// that lets the library auto-derive `rx_queue_range` and per-process
/// src_port segments instead of asking the caller to hand-partition.
///
/// `eph-net-dpdk` historically left every multi-process resource —
/// `rx_queue_range`, src_port segments, lcore masks — for the user to
/// hand-disjoint across primary+secondary processes (see
/// `docs/dpdk-multiprocess.md` "Source-port partitioning"). That contract
/// is brittle: a typo in one process silently re-uses another's queue or
/// port, which surfaces as either an mbuf race (queue collision) or an
/// exchange-side anti-abuse disconnect (5-tuple collision). Both are
/// painful to reproduce off-prod.
///
/// `MpTopology` collapses that consensus burden to two numbers
/// (`self_index`, `total_procs`) for the typical uniform case, while
/// keeping the door open for power users to declare a non-uniform
/// layout (e.g. one trader process gets 6 queues, the rest 1 each).
/// Whatever the caller declares, the library uses a hugepage-backed
/// shared registry (`detail::MpRegistryHandle`) to cross-validate that
/// the declared topology is consistent across the primary and every
/// secondary that attaches.
///
/// This header is intentionally DPDK-free: it pulls in only `<cstdint>`,
/// `<string>`, `<string_view>`, `<vector>`, `<format>`. That keeps unit
/// tests for the topology-shaping logic (`valid()`, `uniform()`, etc.)
/// runnable in any environment, and lets the registry header be the
/// only place that talks to `<rte_memzone.h>` / `<rte_eal.h>`.
///
/// See `docs/dpdk-multiprocess.md` "Recommended path: MpTopology".

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace eph::dpdk {

/// @brief Per-process resource spec — what one DPDK process owns.
///
/// All ranges are half-open `[lo, hi)`. The `tag` is a free-form
/// human-readable label used in logs / dump output / registry slot
/// metadata — it has no semantic effect. Held as `std::string_view`
/// to keep `ProcSpec` cheap to copy and to mirror `PlatformConfig::
/// file_prefix`'s ownership rule: caller owns the backing buffer
/// (string literals or long-lived application-owned strings; never
/// a temporary).
struct ProcSpec {
    std::string_view tag {};        ///< Optional human-readable label
    uint16_t queue_lo = 0;          ///< RX queue range lo (inclusive)
    uint16_t queue_hi = 0;          ///< RX queue range hi (exclusive)
    /// @brief src_port range lo (inclusive). Held as uint32_t so the
    /// half-open `port_hi` can express the full Linux ephemeral window
    /// up to 65536 without uint16_t wrap. Values are still constrained
    /// to `[0, 65536]` by `valid()` and the `uniform()` factory.
    uint32_t port_lo  = 0;
    uint32_t port_hi  = 0;          ///< src_port range hi (exclusive)

    [[nodiscard]] friend bool operator==(const ProcSpec&,
                                         const ProcSpec&) = default;
};

/// @brief Multi-process resource topology — the full picture of who
///        owns which RX queues and src_port segments on a shared NIC.
///
/// The library uses `procs[self_index]` to derive `rx_queue_range` and
/// to constrain `find_src_port_for_queue`'s search range. Other slots
/// in `procs` are not consumed at runtime by this process — they exist
/// to (a) record the full topology in the cross-process registry so
/// secondaries can cross-validate against the primary, and (b) let
/// `valid()` detect cross-process overlap before any DPDK side-effect.
///
/// Construction patterns:
///   - **Uniform (90% case)**: `MpTopology::uniform(self_index, total_procs,
///     nb_rx_queues)` — equal-share queue split + 32 KiB ephemeral port
///     range divided evenly. Two numbers per process, no other config.
///   - **Custom (power-user case)**: build `procs` directly via
///     initializer list when the layout is intentionally non-uniform
///     (e.g. one trader gets 6 queues, monitors get 1 each).
struct MpTopology {
    /// @brief 0-based index of THIS process within `procs`. Must be
    /// strictly less than `procs.size()`. The primary should typically
    /// use `0` so its slot is the first one written into the shared
    /// registry; secondaries take the indices the operator assigned them.
    uint8_t self_index = 0;

    /// @brief All process slots — the global topology view.
    /// Every process declares the *same* `procs` and reads its own
    /// row via `self_index`. The registry verifies disjointness.
    std::vector<ProcSpec> procs {};

    [[nodiscard]] friend bool operator==(const MpTopology&,
                                         const MpTopology&) = default;

    /// @brief Hard upper bound on `procs.size()`. Aligned with
    /// `kMaxRssQueues = 64` (platform.hpp): no realistic single-NIC HFT
    /// deployment runs more than a handful of processes against one
    /// physical port; 64 is ~8× headroom and matches the registry
    /// header's fixed-size `ProcSlot` array.
    static constexpr uint8_t kMaxProcs = 64;

    /// @brief Returns true iff the topology is internally consistent:
    ///   * `procs` is non-empty and `procs.size() <= kMaxProcs`
    ///   * `self_index < procs.size()`
    ///   * every `ProcSpec` has `queue_lo < queue_hi` and `port_lo < port_hi`
    ///   * RX queue ranges are pairwise disjoint
    ///   * src_port ranges are pairwise disjoint
    ///
    /// O(N²) in `procs.size()`; N ≤ 64 so the worst case is 64*63/2 ≈
    /// 2k comparisons — only ever called once per process, on the cold
    /// `Platform::create_*` path.
    [[nodiscard]] bool valid() const noexcept {
        // Empty / oversized topology — caller almost certainly forgot
        // to populate `procs` or accidentally pushed past kMaxProcs.
        if (procs.empty() || procs.size() > kMaxProcs) return false;
        if (self_index >= procs.size()) return false;

        for (const auto& p : procs) {
            // Empty-or-inverted range is a hard error: it would silently
            // disable any queue / port allocation for this slot, which
            // is far worse than failing fast.
            if (p.queue_lo >= p.queue_hi) return false;
            if (p.port_lo  >= p.port_hi)  return false;
        }

        // Pairwise overlap check. Half-open intervals `[a, b)` and
        // `[c, d)` overlap iff `a < d && c < b`.
        for (size_t i = 0; i < procs.size(); ++i) {
            for (size_t j = i + 1; j < procs.size(); ++j) {
                const auto& a = procs[i];
                const auto& b = procs[j];
                if (a.queue_lo < b.queue_hi && b.queue_lo < a.queue_hi)
                    return false;
                if (a.port_lo < b.port_hi && b.port_lo < a.port_hi)
                    return false;
            }
        }
        return true;
    }

    /// @brief Build an evenly-partitioned topology for `total_procs`
    /// peer processes sharing a NIC with `nb_rx_queues` RX queues and
    /// a contiguous `[port_base, port_base + port_total)` src_port
    /// window.
    ///
    /// Both queues and ports are integer-divided by `total_procs`.
    /// Any remainder is assigned to the **last** proc slot — this keeps
    /// the first N-1 slots at the divisor and concentrates the imbalance
    /// at one end (rather than smearing it via floor / ceil), which is
    /// easier to reason about when reading registry dumps. For example,
    /// `uniform(_, 3, 8)` produces queue ranges `[0,2)`, `[2,4)`,
    /// `[4,8)` — proc 2 gets 4 queues, the others 2 each.
    ///
    /// @param self_index    This process's index in `[0, total_procs)`.
    /// @param total_procs   Number of peers sharing the NIC. Must be
    ///                      ≥ 1 and ≤ `kMaxProcs`. Caller is responsible
    ///                      for using the same value across all peers.
    /// @param nb_rx_queues  Total RX queues on the NIC; matches
    ///                      `PlatformConfig::nb_rx_queues`. Must be ≥
    ///                      `total_procs` so each proc gets at least 1.
    /// @param port_base     Lower bound of the shared ephemeral src_port
    ///                      window (default 32768 — Linux default).
    /// @param port_total    Total port count to split (default 32768
    ///                      = `[32768, 65536)`). Each proc receives
    ///                      `port_total / total_procs` ports (last proc
    ///                      gets the remainder).
    ///
    /// @return A topology whose `valid()` is true on every supported
    ///         input. On invalid input (e.g. `total_procs == 0`) the
    ///         returned topology has empty `procs` and `valid()` will
    ///         report false, which `validate_config` then surfaces as
    ///         a clear error rather than a silent runtime corruption.
    [[nodiscard]] static MpTopology
    uniform(uint8_t self_index, uint8_t total_procs,
            uint16_t nb_rx_queues,
            uint16_t port_base = 32768,
            uint16_t port_total = 32768) noexcept {
        MpTopology t;
        // Defensive guards — return invalid topology rather than UB.
        if (total_procs == 0 || total_procs > kMaxProcs) return t;
        if (nb_rx_queues < total_procs)                  return t;
        if (port_total < total_procs)                    return t;
        if (self_index >= total_procs)                   return t;
        // Overflow guard: port_base + port_total must fit in uint32_t
        // arithmetic (we never let port_hi exceed 65535 here, but the
        // intermediate sum needs to be safe).
        if (static_cast<uint32_t>(port_base) +
            static_cast<uint32_t>(port_total) > 65536u) return t;

        t.self_index = self_index;
        t.procs.reserve(total_procs);

        const uint16_t qlen = static_cast<uint16_t>(nb_rx_queues / total_procs);
        const uint32_t plen = static_cast<uint32_t>(port_total)  / total_procs;
        const uint32_t pbase = static_cast<uint32_t>(port_base);
        const uint32_t pend  = pbase + static_cast<uint32_t>(port_total);

        for (uint8_t i = 0; i < total_procs; ++i) {
            const bool last = (i + 1 == total_procs);
            const uint16_t q_lo = static_cast<uint16_t>(i * qlen);
            const uint16_t q_hi = last
                ? nb_rx_queues
                : static_cast<uint16_t>((i + 1) * qlen);
            const uint32_t p_lo = pbase + i * plen;
            const uint32_t p_hi = last ? pend : (pbase + (i + 1) * plen);
            t.procs.push_back(ProcSpec{
                .tag      = {},
                .queue_lo = q_lo,
                .queue_hi = q_hi,
                .port_lo  = p_lo,
                .port_hi  = p_hi,
            });
        }
        return t;
    }

    /// @brief This process's spec. Precondition: `self_index < procs.size()`
    /// (held by `valid()` — call `valid()` before relying on this).
    /// Returns a reference into `procs`; the returned reference is
    /// invalidated by any subsequent mutation of `procs`.
    [[nodiscard]] const ProcSpec& self() const noexcept {
        return procs[self_index];
    }

    /// @brief Multi-line dump for logging — mirrors
    /// `PlatformConfig::dump()` style.
    [[nodiscard]] std::string dump() const {
        std::string out = std::format(
            "MpTopology: self_index={}, procs={} of max {}\n",
            self_index, procs.size(), kMaxProcs);
        for (size_t i = 0; i < procs.size(); ++i) {
            const auto& p = procs[i];
            out += std::format(
                "  [{}]{} tag='{}' queues=[{},{}) ports=[{},{})\n",
                i, (i == self_index ? "*" : " "),
                p.tag,
                p.queue_lo, p.queue_hi,
                p.port_lo,  p.port_hi);
        }
        return out;
    }
};

} // namespace eph::dpdk
