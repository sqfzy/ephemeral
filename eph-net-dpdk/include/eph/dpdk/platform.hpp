#pragma once

/// @file platform.hpp
/// Layer 1: DPDK Platform — header-only.
///
/// Assumes EAL is already initialized (see eal.hpp).
///
/// Initialization sequence:
///   port enumerate → mempool create →
///   port configure (NIC capability intersection) →
///   RX/TX queue setup → port start → link poll
///
/// Compile-time philosophy:
///   - PlatformConfig fields that are structurally constrained (pool size
///     must be 2^n-1, queues/descs must be > 0) are validated at compile
///     time when the config is constexpr, and at runtime otherwise.
///   - clamp_desc is constexpr — usable in static assertions if NIC
///     descriptor limits are known ahead of time.

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/error.hpp"                  // core::Error / core::ErrorInfo
#include "eph/dpdk/detail/icmp_registry.hpp"   // detail::IcmpRegistry
#include "eph/dpdk/detail/logger.hpp"
#include "eph/dpdk/detail/mp_registry.hpp"     // detail::MpRegistryHandle
#include "eph/dpdk/mp_topology.hpp"            // MpTopology + ProcSpec
#include "eph/dpdk/packet_parse.hpp"           // ParsedIcmp for dispatch_icmp_
#include "eph/dpdk/proc_type.hpp"              // ProcType enum + to_eal_string
#include "eph/net/dpdk/flow_steering.hpp"

// Forward-declare DpdkPoller so register_poller / poller_for_queue can name
// it without pulling in the entire poller.hpp.  The full template lives at
// eph/net/dpdk/poller.hpp; matches the signature there.
namespace eph::net::dpdk {
template <class P> class DpdkPoller;
}

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

namespace eph::dpdk {

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time utilities
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @brief Check if n is of the form (2^k - 1) for some k >= 1.
///
/// DPDK mempool sizes must satisfy this constraint for optimal bucket hashing.
/// Rejects 0 (which is technically 2^0-1 but not a valid pool size).
///
/// @param n  Value to test
/// @return true if n == 2^k - 1 for some k >= 1
[[nodiscard]] constexpr bool is_power_of_two_minus_one(uint32_t n) noexcept {
    if (n == 0) return false;
    uint32_t m = n + 1;
    return m > 0 && (m & (m - 1)) == 0;
}

/// @brief Compute the next valid DPDK mempool size >= n satisfying the 2^k-1 constraint.
///
/// @param n  Minimum desired pool size
/// @return Smallest value >= n of the form 2^k - 1 (k >= 1, so never 0).
///         `n == 0` promotes to 1 (= 2^1 - 1), matching the "k >= 1"
///         invariant that `is_power_of_two_minus_one` enforces.
[[nodiscard]] constexpr uint32_t next_valid_pool_size(uint32_t n) noexcept {
    if (n == 0) return 1;           // 2^1 - 1 — smallest valid pool size
    uint32_t m = n + 1;
    // Round up to next power of 2. m == 0 happens iff n was UINT32_MAX,
    // in which case 2^32 - 1 is the largest representable 2^k - 1 and
    // is itself >= n.
    if (m == 0) return UINT32_MAX;
    if ((m & (m - 1)) == 0) return m - 1;
    return (1u << (32 - std::countl_zero(m))) - 1u;
}

/// @brief Clamp a descriptor count into [nb_min, nb_max], aligned to nb_align.
///
/// constexpr-evaluable: can be used in static_assert when NIC descriptor
/// limits are known at compile time (e.g., from a constexpr dev_info fixture).
///
/// @param requested  Desired descriptor count
/// @param nb_min     NIC minimum descriptors per queue
/// @param nb_max     NIC maximum descriptors per queue
/// @param nb_align   NIC descriptor alignment requirement
/// @return Clamped and aligned descriptor count
///
/// @note Re-clamps after alignment rounding to prevent exceeding nb_max
///       (e.g., nb_max=255, nb_align=64 would produce 256 without re-clamp).
[[nodiscard]] constexpr uint16_t clamp_desc(uint16_t requested,
                               uint16_t nb_min,
                               uint16_t nb_max,
                               uint16_t nb_align) noexcept {
    uint16_t n = std::max(requested, nb_min);
    n = std::min(n, nb_max);
    if (nb_align > 1)
        n = static_cast<uint16_t>(
            ((n + nb_align - 1) / nb_align) * nb_align);
    // Re-clamp after alignment rounding — alignment can push past nb_max
    // (e.g. nb_max=255, nb_align=64 → 256 without re-clamp).
    n = std::min(n, nb_max);
    return n;
}

/// @brief Overload accepting rte_eth_desc_lim directly from DPDK dev_info.
///
/// Not constexpr because rte_eth_desc_lim is a DPDK C struct populated at runtime.
///
/// @param requested  Desired descriptor count
/// @param lim        NIC descriptor limits from rte_eth_dev_info
/// @return Clamped and aligned descriptor count
[[nodiscard]] inline uint16_t clamp_desc(uint16_t requested,
                            const rte_eth_desc_lim& lim) noexcept {
    return clamp_desc(requested, lim.nb_min, lim.nb_max, lim.nb_align);
}

inline spdlog::logger* platform_logger() { return get_logger<LoggerName{"dpdk.platform"}>(); }

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Configuration for a single DPDK NIC port.
///
/// Controls queue counts, descriptor ring sizes, mempool parameters,
/// promiscuous mode, and link-up timeout. Supports constexpr validation
/// via validate_config() and config_ok() for compile-time checking.
///
/// @brief Maximum number of RX queues the Poller registry can hold.
///
/// Sized for current production HFT NICs:
///   - AWS ENA            ≤ 32 queues per device
///   - Mellanox ConnectX-5 ≤ 63 queues
///   - Intel E810         ≤ 256 queues (would need bump if used)
///
/// Storage cost: `std::array<DpdkPoller<void>*, kMaxRssQueues>` ≈
/// `kMaxRssQueues * sizeof(void*)` = 512 B per Platform; trivial.
/// To raise (e.g. for E810 in a future deployment), bump this constant
/// — no other code change required since `register_poller` /
/// `poller_for_queue` validate against this same value.
///
/// Not bound to `RTE_MAX_QUEUES_PER_PORT` (default 1024 in DPDK), which
/// would burst the per-Platform footprint to 8 KiB for no current benefit.
inline constexpr uint16_t kMaxRssQueues = 64;

// `ProcType` lives in `eph/dpdk/proc_type.hpp` so both this header and
// `eal.hpp` can share a single source of truth without forward declarations.

/// @note Queue counts and descriptor counts are automatically clamped to
///       NIC-reported limits during Platform::create(). Values here are
///       the *requested* values — actual values may be smaller.
struct PlatformConfig {
    uint16_t port_id         = 0;       ///< DPDK port ID to initialize
    uint16_t nb_rx_queues    = 1;       ///< Requested number of RX queues
    uint16_t nb_tx_queues    = 1;       ///< Requested number of TX queues
    uint16_t nb_rx_desc      = 256;     ///< Requested RX descriptors per queue
    uint16_t nb_tx_desc      = 512;     ///< Requested TX descriptors per queue
    /// @brief Mempool size. Must be 2^n - 1 (e.g. 1023, 4095, 8191).
    /// DPDK mempool bucket hashing requires this form for optimal performance.
    uint32_t mbuf_pool_size  = 4095;
    uint16_t mbuf_cache_size = 256;     ///< Per-lcore mempool cache size
    bool     enable_promiscuous = false; ///< Enable promiscuous mode on the port
    /// @brief Opt-in: request NIC RX checksum offload for IPv4 and UDP.
    ///
    /// When true, `configure_port()` sets
    /// `RTE_ETH_RX_OFFLOAD_IPV4_CKSUM | RTE_ETH_RX_OFFLOAD_UDP_CKSUM` in
    /// `rxmode.offloads` (intersected with `dev_info.rx_offload_capa`).
    /// The NIC then populates `mbuf->ol_flags` with
    /// `RTE_MBUF_F_RX_{IP,L4}_CKSUM_{GOOD,BAD,UNKNOWN,NONE}` on every RX,
    /// and `DpdkUdpSocket::process_burst_` drops BAD-flagged packets
    /// before codec dispatch, incrementing `StreamMetric::kRxBadChecksum`.
    ///
    /// Default false — current (pre-fix) behavior is preserved byte-for-byte
    /// when opt-in is off. If the NIC lacks one or both offload capabilities,
    /// `configure_port()` emits a WARN and proceeds without requesting the
    /// unsupported flag (no abort — downstream code still runs, just with
    /// no BAD detection on that layer).
    ///
    /// HFT-design: software fallback is intentionally NOT provided. NIC
    /// capability is an infrastructure-level decision, not a per-packet
    /// cost to absorb in the hot path. If the NIC cannot offload, operators
    /// must either change hardware or accept the default unprotected path.
    ///
    /// Symmetric DpdkTcpStream wiring is a follow-up (see eph-net-dpdk
    /// CHANGELOG TD-3).
    bool     enable_rx_checksum_offload = false;
    /// @brief Opt-in: strict RX checksum semantic (TD-2 of the
    /// lucky-giggling-kahan review). Only takes effect when
    /// `enable_rx_checksum_offload == true`.
    ///
    /// When false (default, "best-effort"): DpdkUdpSocket /
    /// DpdkTcpStream drop only mbufs the NIC flagged as BAD. UNKNOWN
    /// (no info) / GOOD (validated) / NONE (NIC could not verify) all
    /// pass through. This matches the current production behavior and
    /// is appropriate for HFT NICs that legitimately emit UNKNOWN on
    /// tunnel / VLAN paths.
    ///
    /// When true ("strict"): drop any mbuf whose IP or L4 checksum is
    /// NOT explicitly GOOD. The drop condition switches from "BAD bit
    /// is set" to "CKSUM_MASK != CKSUM_GOOD". Safe for environments
    /// where every RX packet should be NIC-verified end-to-end and any
    /// unverifiable packet is a config / security concern. Drop counts
    /// still aggregate into the existing split counters
    /// `kRxIpChecksumBad` / `kRxL4ChecksumBad` — the strict mode just
    /// widens the drop condition, not the counter set.
    ///
    /// If this flag is true but `enable_rx_checksum_offload` is false,
    /// `Platform::create()` emits a WARN and the flag has no effect
    /// (strict mode without NIC offload is meaningless — every packet
    /// would report UNKNOWN and be dropped).
    bool     enable_strict_rx_checksum = false;
    /// @brief Poll timeout for link-up after port start (milliseconds).
    /// 0 = single check, continue regardless of link state.
    int      link_timeout_ms = 2000;

    // ── Multi-process (primary+secondary) configuration ─────────────────
    //
    // All fields default to single-process / primary semantics: existing
    // code compiled against the old PlatformConfig continues to work
    // byte-for-byte. Multi-process callers opt in by setting proc_type /
    // file_prefix and (for secondary) an explicit rx_queue_range. See
    // `docs/dpdk-multiprocess.md` and `ProcType` above.

    /// @brief DPDK process role. Primary owns the port lifecycle
    /// (configure/start/stop/close, mempool creation); Secondary attaches
    /// to an already-running primary via shared hugepage and must avoid
    /// port-state-mutating APIs. Default = Primary preserves existing
    /// single-process behavior.
    ProcType proc_type = ProcType::Primary;

    /// @brief DPDK runtime-dir discriminator, mirrors `--file-prefix` EAL
    /// arg. Empty = no prefix (DPDK uses its default shared runtime dir,
    /// same as passing nothing). Secondary MUST pass the same non-empty
    /// prefix the primary used, otherwise `rte_mempool_lookup` will fail
    /// with "primary not running or file_prefix mismatch".
    ///
    /// Held as `std::string_view` to keep `PlatformConfig` a literal type
    /// (preserves `constexpr PlatformConfig cfg{};` + `validate_config`
    /// static_assert usage). Caller owns the backing buffer — typical
    /// values are string literals or long-lived application-owned
    /// strings; don't point at a temporary.
    std::string_view file_prefix {};

    /// @brief Half-open RX queue range `[lo, hi)` that THIS process owns.
    /// Secondary processes must pick a range disjoint from the primary's
    /// (and from any other secondary's) to avoid mbuf races on shared
    /// queues. `{0, 0}` (default) is the sentinel meaning "use the full
    /// range `[0, nb_rx_queues)`", which matches existing single-process
    /// behavior — `create_and_attach`'s round-robin target-queue selector
    /// then spreads across every queue, as before.
    std::pair<uint16_t, uint16_t> rx_queue_range {0, 0};

    // ── Per-lcore / NUMA-aware mempools (T2.9) ──────────────────────────
    //
    // Default = 0 (single shared pool, byte-for-byte compat with the
    // pre-T2.9 layout). When > 0, Platform creates N additional pools,
    // one per lcore id `[0, per_lcore_pools)`. Each pool is allocated on
    // the socket local to its lcore (via `rte_lcore_to_socket_id()` and
    // `rte_pktmbuf_pool_create(... socket_id)`), so multi-NUMA hosts
    // avoid the +50–100 ns cross-NUMA mbuf alloc penalty when an lcore
    // draws from its own pool. Per-lcore separation also eliminates the
    // mempool ring contention that shows up under several lcores hammering
    // a single shared pool.
    //
    // Caller picks which pool to use when allocating via
    // `Platform::pool_for_lcore(lcore_id)`. `DpdkTcpStream::create_and_attach`
    // / `DpdkUdpSocket::create_and_attach` honour the optional
    // `StreamConfig::pool_lcore_hint` / `UdpConfig::pool_lcore_hint` —
    // when set, they override `cfg.pool` with `platform.pool_for_lcore(*hint)`
    // so user code only needs to populate the hint instead of looking up
    // the pool by hand.
    //
    // Sized to fit `RTE_MAX_LCORE` (256), which exceeds any realistic HFT
    // box. The first `per_lcore_pools` slots are populated; pool sizing is
    // `mbuf_pool_size` per pool (so memory footprint scales linearly with
    // `per_lcore_pools`). All pools share the existing `mbuf_pool_size /
    // mbuf_cache_size` configuration — make them larger if needed when
    // opting in.
    //
    // Backwards-compat: at `per_lcore_pools == 0`, NO extra pools are
    // created. `pool_for_lcore(any)` returns the single shared pool, and
    // the legacy `mempool()` accessor is unchanged. Setting `> 0` is the
    // opt-in switch — existing call sites that don't touch
    // `pool_lcore_hint` keep using the shared pool.
    uint16_t per_lcore_pools = 0;

    // ── Auto-derived MP layout (recommended path) ───────────────────────
    //
    // When set, the library treats this as the source of truth for
    // multi-process resource allocation: `Platform::create_primary` /
    // `create_secondary` derive the effective `rx_queue_range` from
    // `mp_topology->self()`, register the topology in a shared hugepage
    // memzone (`detail::MpRegistry`) so cross-process disjointness is
    // enforced rather than trusted, and constrain the stream creators'
    // src_port search range to the self spec's `[port_lo, port_hi)`.
    //
    // Mutually exclusive with a hand-set `rx_queue_range != {0,0}`:
    // `validate_config` rejects the combination so the caller can't
    // silently fight the auto-derivation. Setting `mp_topology` plus
    // leaving `rx_queue_range` at its `{0,0}` sentinel is the
    // recommended path; setting `rx_queue_range` and leaving
    // `mp_topology` empty is the legacy escape hatch (manual
    // partitioning, see docs/dpdk-multiprocess.md "Advanced usage").
    //
    // `MpTopology` is a literal type (fixed `std::array<ProcSpec, 64>`
    // backing — see mp_topology.hpp) so wrapping it in `std::optional`
    // keeps `PlatformConfig` constexpr-constructible: existing
    // `static_assert(config_ok(kBaseCfg))` patterns in tests continue
    // to compile against the default-empty optional.
    std::optional<MpTopology> mp_topology {};

    // NOTE on source-port partitioning across MP processes:
    //
    // `eph-net-dpdk` does NOT auto-allocate source ports. The TCP/UDP
    // `create_and_attach` paths take the source port from the caller-
    // supplied `cfg.dpdk.tcp_low_level.tuple.src_port` (TCP) /
    // `cfg.legacy.src_port` (UDP, retained legacy name; see
    // eph::net::dpdk::UdpConfig) in Software / FlowDirector mode, or
    // rebind it to one that hashes to the desired queue (RSS-pinned
    // mode via `find_src_port_for_queue`). In a multi-process setup it is
    // therefore the *caller*'s job to ensure that the primary and each
    // secondary draw their source ports from disjoint sub-ranges — the
    // library has no global view to enforce this. See
    // `docs/dpdk-multiprocess.md` for guidance on partitioning.

    /// Defaulted equality — all fields must match exactly.
    [[nodiscard]] friend bool operator==(const PlatformConfig&,
                                         const PlatformConfig&) = default;

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        std::string base = std::format(
            "PlatformConfig:\n"
            "  port_id: {}, queues: {}rx/{}tx, descriptors: {}rx/{}tx\n"
            "  mbuf pool: {} (cache: {}), promiscuous: {}, link_timeout: {}ms\n"
            "  rx_cksum_offload: {}, strict_rx_cksum: {}\n"
            "  proc_type: {}, file_prefix: '{}', rx_queue_range: [{},{})\n"
            "  per_lcore_pools: {}\n"
            "  mp_topology: {}",
            port_id, nb_rx_queues, nb_tx_queues, nb_rx_desc, nb_tx_desc,
            mbuf_pool_size, mbuf_cache_size,
            enable_promiscuous ? "true" : "false", link_timeout_ms,
            enable_rx_checksum_offload ? "true" : "false",
            enable_strict_rx_checksum  ? "true" : "false",
            proc_type == ProcType::Primary ? "Primary" : "Secondary",
            file_prefix,
            rx_queue_range.first, rx_queue_range.second,
            per_lcore_pools,
            mp_topology.has_value() ? "set" : "unset");
        if (mp_topology.has_value()) {
            base += "\n";
            base += mp_topology->dump();
        }
        return base;
    }

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate_config() which blocks construction, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        // Very small descriptor counts may cause drops under burst
        if (nb_rx_desc < 64)
            w.emplace_back(std::format(
                "nb_rx_desc={} is small -- may cause RX drops under "
                "burst traffic", nb_rx_desc));
        if (nb_tx_desc < 64)
            w.emplace_back(std::format(
                "nb_tx_desc={} is small -- may cause TX backpressure",
                nb_tx_desc));
        // Large cache relative to pool wastes memory per-lcore
        if (mbuf_cache_size > 0 && mbuf_pool_size > 0 &&
            mbuf_cache_size > mbuf_pool_size / 4)
            w.emplace_back(std::format(
                "mbuf_cache_size={} is large relative to pool_size={} -- "
                "each lcore reserves this many mbufs, reducing effective "
                "pool capacity for other lcores",
                mbuf_cache_size, mbuf_pool_size));
        // Promiscuous mode in production is a security concern
        if (enable_promiscuous)
            w.emplace_back("enable_promiscuous=true -- receives all NIC "
                           "traffic, not just this MAC; consider disabling "
                           "in production");
        if (enable_strict_rx_checksum && !enable_rx_checksum_offload)
            w.emplace_back(
                "enable_strict_rx_checksum=true but enable_rx_checksum_offload"
                "=false -- strict mode has no effect without NIC offload; the"
                " flag will be silently ignored (all packets would be UNKNOWN"
                " and strict would drop them all)");
        // Zero link timeout means no wait for link-up
        if (link_timeout_ms == 0)
            w.emplace_back("link_timeout_ms=0 -- will not wait for NIC "
                           "link-up; operations may fail if link is slow "
                           "to establish");
        return w;
    }

    /// JSON-formatted config for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"port_id\":{},\"nb_rx_queues\":{},\"nb_tx_queues\":{},"
            "\"nb_rx_desc\":{},\"nb_tx_desc\":{},"
            "\"mbuf_pool_size\":{},\"mbuf_cache_size\":{},"
            "\"enable_promiscuous\":{},"
            "\"enable_rx_checksum_offload\":{},"
            "\"enable_strict_rx_checksum\":{},"
            "\"link_timeout_ms\":{},"
            "\"proc_type\":\"{}\",\"file_prefix\":\"{}\","
            "\"rx_queue_range\":[{},{}],"
            "\"per_lcore_pools\":{},"
            "\"mp_topology_set\":{},\"mp_topology_self_index\":{},"
            "\"mp_topology_total_procs\":{}}}",
            port_id, nb_rx_queues, nb_tx_queues,
            nb_rx_desc, nb_tx_desc,
            mbuf_pool_size, mbuf_cache_size,
            enable_promiscuous ? "true" : "false",
            enable_rx_checksum_offload ? "true" : "false",
            enable_strict_rx_checksum  ? "true" : "false",
            link_timeout_ms,
            proc_type == ProcType::Primary ? "Primary" : "Secondary",
            file_prefix,
            rx_queue_range.first, rx_queue_range.second,
            per_lcore_pools,
            mp_topology.has_value() ? "true" : "false",
            mp_topology.has_value() ? mp_topology->self_index : uint8_t{0},
            mp_topology.has_value() ? mp_topology->total_procs : uint8_t{0});
    }
};

/// Validation result — empty string_view on success, error description otherwise.
/// constexpr-evaluable: use in static_assert for compile-time configs, or
/// call at runtime for dynamic configs.
///
/// A result containing "2^n" is a performance warning (DPDK rounds up silently),
/// not a hard error.
[[nodiscard]] constexpr std::string_view validate_config(const PlatformConfig& cfg) noexcept {
    if (cfg.nb_rx_queues  == 0) return "nb_rx_queues must be > 0";
    if (cfg.nb_tx_queues  == 0) return "nb_tx_queues must be > 0";
    if (cfg.nb_rx_desc    == 0) return "nb_rx_desc must be > 0";
    if (cfg.nb_tx_desc    == 0) return "nb_tx_desc must be > 0";
    if (cfg.link_timeout_ms < 0) return "link_timeout_ms must be >= 0";
    if (!detail::is_power_of_two_minus_one(cfg.mbuf_pool_size))
        return "mbuf_pool_size must be 2^n - 1 (e.g. 1023, 4095, 8191)";
    // DPDK rte_pktmbuf_pool_create requires cache_size < pool_size.
    if (cfg.mbuf_cache_size >= cfg.mbuf_pool_size)
        return "mbuf_cache_size must be less than mbuf_pool_size";
    // rx_queue_range: either the {0,0} sentinel ("full range") or a non-
    // empty sub-range bounded by nb_rx_queues. Without this check, a
    // half-set range (lo == hi != 0, or lo > hi, or hi > nb_rx_queues)
    // would silently slip through `Platform::create_primary` and corrupt
    // round-robin queue selection at create_and_attach time.
    if (cfg.rx_queue_range.first != 0 || cfg.rx_queue_range.second != 0) {
        if (cfg.rx_queue_range.first >= cfg.rx_queue_range.second)
            return "rx_queue_range: lo must be < hi (or use {0,0} sentinel for full range)";
        if (cfg.rx_queue_range.second > cfg.nb_rx_queues)
            return "rx_queue_range.hi must not exceed nb_rx_queues";
    }
    // Per-lcore pools: 0 = disabled (legacy single-shared-pool layout).
    // Upper bound matches RTE_MAX_LCORE so the on-Platform fixed array
    // can hold every populated slot. We don't enforce
    // `per_lcore_pools <= rte_lcore_count()` here because the active lcore
    // mask is an EAL-runtime value, not a config-time constant.
    if (cfg.per_lcore_pools > 256)
        return "per_lcore_pools must be <= RTE_MAX_LCORE (256)";
    // mp_topology (auto-derived MP layout) is mutually exclusive with a
    // hand-set rx_queue_range. Allowing both would let two source-of-
    // truth values disagree silently — pick one path or the other.
    if (cfg.mp_topology.has_value()) {
        if (cfg.rx_queue_range.first != 0 || cfg.rx_queue_range.second != 0)
            return "mp_topology is set; rx_queue_range must remain {0,0} sentinel";
        if (!cfg.mp_topology->valid())
            return "mp_topology failed valid() (overlap / OOB self_index / "
                   "empty range — see MpTopology::valid())";
        if (cfg.mp_topology->self().queue_hi > cfg.nb_rx_queues)
            return "mp_topology.self().queue_hi exceeds nb_rx_queues";
    }
    return {};
}

/// For use in static_assert with constexpr configs:
///   constexpr PlatformConfig cfg{...};
///   static_assert(config_ok(cfg), "bad platform config");
[[nodiscard]] constexpr bool config_ok(const PlatformConfig& cfg) noexcept {
    return validate_config(cfg).empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// Platform
// ─────────────────────────────────────────────────────────────────────────────

/// @brief DPDK NIC port manager — owns a configured port and its mempool.
///
/// Encapsulates the full DPDK port lifecycle: port enumeration, mempool
/// creation, port configuration (with NIC capability intersection), queue
/// setup, port start, and link-up polling. Use the static factory
/// Platform::create() for construction.
///
/// Move-only. Moved-from instances return nullptr/0/false from all accessors.
/// Check validity via `explicit operator bool()`.
///
/// @note Assumes EAL is already initialized (see eal.hpp / EalGuard).
class Platform {
public:
    /// @brief NIC-level packet statistics snapshot.
    ///
    /// All counters are cumulative since port start. Use operator- to compute
    /// deltas for interval-based monitoring.
    struct Stats {
        uint64_t rx_packets = 0;
        uint64_t tx_packets = 0;
        uint64_t rx_bytes   = 0;
        uint64_t tx_bytes   = 0;
        uint64_t rx_missed  = 0;
        uint64_t rx_errors  = 0;
        uint64_t tx_errors  = 0;

        /// Multi-line formatted dump for logging/debugging.
        [[nodiscard]] std::string dump() const {
            return std::format(
                "Platform::Stats:\n"
                "  rx_packets: {}\n"
                "  tx_packets: {}\n"
                "  rx_bytes: {}\n"
                "  tx_bytes: {}\n"
                "  rx_missed: {}\n"
                "  rx_errors: {}\n"
                "  tx_errors: {}",
                rx_packets, tx_packets, rx_bytes, tx_bytes,
                rx_missed, rx_errors, tx_errors);
        }

        /// JSON-formatted stats for monitoring system integration.
        [[nodiscard]] std::string to_json() const {
            return std::format(
                "{{\"rx_packets\":{},\"tx_packets\":{},\"rx_bytes\":{},"
                "\"tx_bytes\":{},\"rx_missed\":{},\"rx_errors\":{},"
                "\"tx_errors\":{}}}",
                rx_packets, tx_packets, rx_bytes, tx_bytes,
                rx_missed, rx_errors, tx_errors);
        }

        /// Compute delta between two snapshots for interval-based monitoring.
        [[nodiscard]] friend Stats operator-(const Stats& lhs, const Stats& rhs) noexcept {
            return Stats{
                .rx_packets = lhs.rx_packets - rhs.rx_packets,
                .tx_packets = lhs.tx_packets - rhs.tx_packets,
                .rx_bytes   = lhs.rx_bytes   - rhs.rx_bytes,
                .tx_bytes   = lhs.tx_bytes   - rhs.tx_bytes,
                .rx_missed  = lhs.rx_missed  - rhs.rx_missed,
                .rx_errors  = lhs.rx_errors  - rhs.rx_errors,
                .tx_errors  = lhs.tx_errors  - rhs.tx_errors,
            };
        }

        [[nodiscard]] friend bool operator==(const Stats&, const Stats&) = default;
    };

    /// Create and fully initialize the DPDK platform for one port.
    /// EAL must already be initialized (via eph::dpdk::eal_init).
    ///
    /// This is the original single-process factory. It honours
    /// `config.proc_type` (default Primary → full port bringup), so pre-MP
    /// callers continue to work byte-for-byte. For new multi-process code
    /// prefer the explicit `create_primary` / `create_secondary` entry
    /// points — they make the role visible at the call site and (for
    /// secondary) surface the strict config validation earlier.
    [[nodiscard]] static std::expected<Platform, std::string>
    create(const PlatformConfig& config);

    /// Primary-role factory (single-NIC multi-process aware).
    ///
    /// Equivalent to `create()` with `config.proc_type` forced to
    /// `ProcType::Primary`. Behaviourally identical to the current
    /// single-process setup; the explicit name is for call-site clarity
    /// in code that also uses `create_secondary`.
    [[nodiscard]] static std::expected<Platform, std::string>
    create_primary(PlatformConfig config);

    /// Secondary-role factory (attach to a running primary's port +
    /// mempool via shared hugepage).
    ///
    /// Forces `config.proc_type = Secondary` and enforces the
    /// secondary-mode contract:
    ///   * `validate_config(cfg)` must pass — in particular
    ///     `rx_queue_range` is either the `{0,0}` full-range sentinel or
    ///     a non-empty sub-range bounded by `nb_rx_queues`.
    ///   * `file_prefix` must be non-empty and match the primary's EAL
    ///     `--file-prefix` (else `rte_mempool_lookup` will fail).
    ///   * `rte_eth_dev_is_valid_port(port_id)` must hold — i.e. the
    ///     primary has already started this port under the shared
    ///     hugepage runtime dir.
    ///
    /// Source-port partitioning across MP processes is the **caller's**
    /// responsibility — `eph-net-dpdk` does not auto-allocate src_port
    /// and has no global view across processes to enforce disjointness.
    /// See `docs/dpdk-multiprocess.md` for partitioning guidance.
    ///
    /// EAL must already be initialised with `--proc-type=secondary`
    /// and the same `--file-prefix` the primary used; see
    /// `eph::dpdk::build_eal_argv` / `docs/dpdk-multiprocess.md`.
    [[nodiscard]] static std::expected<Platform, std::string>
    create_secondary(PlatformConfig config);

    ~Platform();

    Platform(const Platform&)            = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) noexcept;
    Platform& operator=(Platform&&) noexcept;

    /// True if this Platform is in a valid (non-moved-from) state.
    /// Use before calling other accessors when the Platform may have been moved.
    [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }

    /// @brief Get the packet mbuf mempool for this port.
    /// @return Mempool pointer, or nullptr if moved-from.
    ///
    /// In single-shared-pool mode (`per_lcore_pools == 0`) this returns
    /// the one and only pool — same as the pre-T2.9 behavior.
    /// In per-lcore-pool mode (`per_lcore_pools > 0`) this returns the
    /// canonical pool (`pool_for_lcore(0)`) — the one used to back the
    /// port's RX descriptor rings. It is a stable, non-null reference
    /// to one of the per-lcore pools, kept here so legacy callers that
    /// don't know about the new accessor continue to work without
    /// changes.
    [[nodiscard]] rte_mempool* mempool()          const noexcept;

    /// @brief Look up the mempool reserved for a given lcore id.
    ///
    /// Hot-path-safe: O(1) array index, no locks, no allocations.
    ///
    /// Behavior:
    ///   * `per_lcore_pools == 0` (default, backwards-compat) — returns
    ///     the single shared pool regardless of `lcore_id`. Equivalent
    ///     to `mempool()`. Existing call sites that pass an arbitrary
    ///     lcore id keep working without code changes.
    ///   * `per_lcore_pools > 0` — returns the pool created for
    ///     `lcore_id` if `lcore_id < per_lcore_pools`, else `nullptr`
    ///     (out-of-range is a programming error and the hot-path caller
    ///     should fall back / abort accordingly).
    ///   * Moved-from Platform — returns `nullptr` for any lcore_id.
    ///
    /// @param lcore_id  DPDK lcore id (typically `rte_lcore_id()`).
    /// @return Per-lcore mempool, or nullptr if out of range / moved-from.
    [[nodiscard]] rte_mempool* pool_for_lcore(uint16_t lcore_id) const noexcept;

    /// @brief Get the DPDK port ID.
    /// @return Port ID, or 0 if moved-from.
    [[nodiscard]] uint16_t     port_id()          const noexcept;

    /// @brief Check if the port has been successfully started.
    [[nodiscard]] bool         is_running()       const noexcept;
    /// Returns true if promiscuous mode was requested AND successfully enabled.
    [[nodiscard]] bool         is_promiscuous()   const noexcept;

    /// @brief Effective strict-RX-checksum mode (TD-2). Returns true
    /// iff BOTH `enable_strict_rx_checksum` AND `enable_rx_checksum_offload`
    /// are configured on this Platform — strict without offload is
    /// meaningless and the getter masks it out so callers (stream /
    /// socket attach) never need to check both flags.
    [[nodiscard]] bool         strict_rx_checksum() const noexcept;

    /// @brief Collect current NIC packet statistics via rte_eth_stats_get.
    /// @return Stats snapshot, or zeroed stats on error or moved-from state.
    [[nodiscard]] Stats collect_stats() const;

    // ── RSS / multi-queue surface (stage 3 of RSS rollout) ────────────────

    /// @brief Cached RX dispatch mode probed once at Platform::create() via
    /// `detect_rx_dispatch_mode()`. Returns Software for moved-from / un-RSS'd
    /// ports.
    [[nodiscard]] ::eph::net::dpdk::RxDispatchMode dispatch_mode() const noexcept;

    /// @brief The actual number of RX queues configured on the port (after
    /// NIC-cap clamping). Returns 0 for moved-from Platforms.
    [[nodiscard]] uint16_t nb_rx_queues() const noexcept;

    /// @brief True iff RSS is active and the prediction key was *probed*
    /// from the NIC (via `rte_eth_dev_rss_hash_conf_get`) rather than
    /// installed by `configure_rss`. Selected automatically when the PMD
    /// rejects `rte_eth_dev_rss_hash_update` but exposes the hash key via
    /// readback (notably newer ENA). Returns false when RSS is inactive
    /// (single-queue Platforms, moved-from), and false on Platforms where
    /// `configure_rss` installed eph's own key in the normal path.
    /// Diagnostic only — does not change hot-path behaviour; predict_rss_queue
    /// / query_rss_state already use whichever key is in effect.
    [[nodiscard]] bool rss_using_probed_key() const noexcept;

    /// @brief Resolved RX-queue range `[lo, hi)` this Platform process owns.
    ///
    /// Cold getter — read once at `create_and_attach` time to drive
    /// round-robin target-queue selection. Two-step resolution:
    ///   * If `config.rx_queue_range == {0, 0}` (sentinel = "full
    ///     range"), returns `{0, nb_rx_queues()}`.
    ///   * Otherwise returns the configured range verbatim (already
    ///     validated by `validate_config`).
    /// Returns `{0, 0}` for moved-from Platforms.
    [[nodiscard]] std::pair<uint16_t, uint16_t>
    effective_rx_queue_range() const noexcept;

    // ── Auto-derived MP layout (mp_topology-driven) ──────────────────────

    /// @brief True iff this Platform was created with a populated
    /// `cfg.mp_topology` and successfully attached to (primary) /
    /// looked up (secondary) the cross-process registry. Cold getter,
    /// safe on moved-from instances (returns false).
    [[nodiscard]] bool has_mp_topology() const noexcept;

    /// @brief This process's `[port_lo, port_hi)` src_port window when
    /// `mp_topology` is in effect; `std::nullopt` otherwise. Stream
    /// `create_and_attach` consults this to constrain
    /// `find_src_port_for_queue`'s search range — letting the library
    /// auto-pick a non-colliding ephemeral src_port instead of asking
    /// the caller to hand-partition src_port ranges across processes.
    /// Cold getter; safe on moved-from instances (returns nullopt).
    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>>
    self_port_range() const noexcept;

    /// @brief Register a per-queue Poller. Intended to be called once per
    /// queue at startup, before the lcore loops begin polling.
    /// Not thread-safe.
    /// @return `InvalidConfig` if `queue_id >= nb_rx_queues()` /
    ///         `>= kMaxRssQueues` / poller is null / Platform is
    ///         moved-from; `InvalidConfig` with "DuplicateQueue" detail
    ///         if the slot is already occupied.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
        register_poller(uint16_t queue_id,
                        ::eph::net::dpdk::DpdkPoller<void>* poller) noexcept;

    /// @brief Look up the Poller registered for a queue. Returns nullptr if
    /// `queue_id` is out of range, the slot is empty, or the Platform is
    /// moved-from.
    [[nodiscard]] ::eph::net::dpdk::DpdkPoller<void>*
        poller_for_queue(uint16_t queue_id) const noexcept;

    /// @brief Unregister the Poller for `queue_id`. Idempotent — out-of-range
    /// `queue_id`, an already-empty slot, and a moved-from Platform are all
    /// silent no-ops. Caller is responsible for ensuring no `process_burst`
    /// dispatch is in flight against this slot when the call returns; in
    /// the typical pattern (Platform / Poller / streams all torn down on
    /// the same lcore) that's automatic.
    ///
    /// Use case: tests that create + destroy Pollers within a longer-lived
    /// Platform's lifetime, and applications that hot-swap a queue's
    /// Poller (e.g. failover). Production code that creates one Poller per
    /// queue at startup and tears the Platform down at shutdown does not
    /// need to call this.
    void unregister_poller(uint16_t queue_id) noexcept;

    // ── ICMP Frag Needed target registry ─────────────────────────────────
    //
    // ICMP messages can arrive on any RX queue (routers pick based on
    // their own hashing, not ours). Platform is the only global view
    // over all streams/queues — so it owns ICMP dispatch.
    //
    // The registry's store + state-machine lives in
    // `detail::IcmpRegistry` (heap-allocated, ref-counted via
    // shared_ptr, internally synchronised with std::mutex). Platform
    // is a thin facade that holds one strong ref via
    // `Impl::icmp_registry_sp` and forwards the public API.
    //
    // `DpdkTcpStream::create_and_attach` installs a closure on each
    // registered Poller's ICMP callback; the closure captures
    // `icmp_registry_shared_()`'s shared_ptr **by value**, giving the
    // Poller a second strong ref. Registry lifetime is therefore the
    // union of "Platform alive" OR "Poller still holds the closure"
    // — so any declaration / destruction order is safe.
    //
    // Streams call `register_icmp_target` once post-attach. The
    // returned RAII Handle holds a `weak_ptr<IcmpRegistry>`: if the
    // registry has already been freed by the time the Handle
    // destructs, the weak_ptr lock returns empty and unregister is
    // a safe no-op.
    //
    // @note LIFETIME is now enforced by shared_ptr/weak_ptr
    //       semantics; no external declaration-order contract is
    //       needed. The recommended ordering (Platform outermost,
    //       Stream innermost) remains a good habit for other
    //       reasons (mempool / port lifetime), but an accidental
    //       reverse order no longer causes UAF in the ICMP path.
    using IcmpMtuCallback  = ::eph::dpdk::detail::IcmpRegistry::MtuCallback;
    using IcmpTargetHandle = ::eph::dpdk::detail::IcmpRegistry::Handle;

    /// @brief Register an ICMP Frag Needed target. Returns an RAII
    ///        handle — destroy it (or overwrite via move-assignment) to
    ///        unregister. Duplicate (tuple, proto) are rejected.
    ///
    /// Not thread-safe: expected to be called from the same
    /// stream-construction thread that registered the Poller.
    [[nodiscard]] std::expected<IcmpTargetHandle, ::eph::core::ErrorInfo>
        register_icmp_target(::eph::dpdk::net::ConnectionTuple tuple,
                             uint8_t  proto,
                             void*    stream,
                             IcmpMtuCallback cb) noexcept;

    /// @brief Running count of ICMP Type 3 Code 4 messages dispatched
    ///        to a registered target since Platform construction.
    [[nodiscard]] uint64_t icmp_frag_needed_dispatched() const noexcept;

    /// @brief Internal: returns the shared_ptr to this Platform's ICMP
    ///        registry. Used by `DpdkTcpStream::create_and_attach` to
    ///        capture a strong ref into the Poller's ICMP callback
    ///        closure so the registry outlives Platform if needed.
    ///        Returns empty shared_ptr on moved-from Platform.
    [[nodiscard]] std::shared_ptr<::eph::dpdk::detail::IcmpRegistry>
    icmp_registry_shared_() const noexcept;

private:
    struct Impl;
    explicit Platform(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Platform::Impl
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Internal implementation of Platform (pimpl pattern).
///
/// Owns the mempool and port lifecycle. Destructor calls cleanup() which
/// stops the port and frees the mempool.
struct Platform::Impl {
    /// Hard upper bound on per-lcore pools — matches DPDK's
    /// `RTE_MAX_LCORE` (256). Storing the array inline keeps lookup
    /// branch-free at the hot path's expense of ~2 KiB per Platform,
    /// which is negligible compared to the mempools themselves.
    static constexpr std::size_t kMaxPools = 256;

    /// @brief Cross-process MP registry attachment, populated by
    /// `Platform::create_primary` / `create_secondary` when the caller
    /// supplied `cfg.mp_topology`. Held in `std::optional` so the
    /// non-MP path leaves it empty without paying the (still small)
    /// cost of a default-constructed handle. **Declared before every
    /// other DPDK-resource-owning field** so it outlives them on
    /// destruction (reverse construction order): mempool, port state
    /// and pollers tear down first; the registry's hugepage memzone
    /// release happens last, when no other process state could still
    /// be reading it.
    std::optional<::eph::dpdk::detail::MpRegistryHandle> mp_registry;

    PlatformConfig config;
    /// Canonical pool — points at the single shared pool when
    /// `per_lcore_pools == 0`, or at `pools_[0]` when
    /// `per_lcore_pools > 0`. Used unchanged by all the existing
    /// `mempool()`-driven call sites (RX queue setup, secondary
    /// attach mirror, etc.).
    rte_mempool*   mempool{nullptr};
    /// Per-lcore mempool slots. Slots `[0, per_lcore_pools)` are
    /// populated when the feature is opt-in; all other slots stay
    /// `nullptr`. In default mode (`per_lcore_pools == 0`) the array
    /// is left empty and lookup falls back to `mempool` so the legacy
    /// "any lcore → shared pool" semantics holds.
    std::array<rte_mempool*, kMaxPools> per_lcore_pool{};
    bool           port_started{false};
    bool           promiscuous_active{false};

    // RSS / multi-queue dispatch state (stage 3).
    bool           rss_active{false};   ///< True if configure_rss() succeeded
                                        ///< OR probe-based bring-up resolved.
    /// True iff `rss_active` was set via the probe path
    /// (rte_eth_dev_rss_hash_conf_get) rather than via configure_rss
    /// installing eph's own key. Selected automatically on PMDs that
    /// reject rss_hash_update but expose hash_conf_get (notably newer
    /// ENA). predict_rss_queue / query_rss_state pick up the actual
    /// NIC key transparently in this mode.
    bool           rss_using_probed_key{false};
    ::eph::net::dpdk::RxDispatchMode dispatch_mode{
        ::eph::net::dpdk::RxDispatchMode::Software};
    /// Per-queue Poller registry. Populated by Stream::create_and_attach
    /// (stage 4) at startup, read on the hot path via poller_for_queue.
    /// nullptr slot = unregistered queue.
    std::array<::eph::net::dpdk::DpdkPoller<void>*, kMaxRssQueues> pollers{};

    // ── ICMP Frag Needed target registry ──
    // State + state-machine live in `detail::IcmpRegistry`; this
    // `shared_ptr` is the strong owner. The same registry may also
    // be held strongly by a `DpdkPoller`'s ICMP callback closure
    // (for lifetime-safe dispatch if this Platform is destroyed
    // first) and weakly by every stream's `IcmpTargetHandle` (for
    // safe deregistration if the registry outlives the handle).
    // See `eph/dpdk/detail/icmp_registry.hpp`.
    std::shared_ptr<::eph::dpdk::detail::IcmpRegistry> icmp_registry_sp{
        std::make_shared<::eph::dpdk::detail::IcmpRegistry>()};

    ~Impl() { cleanup(); }

    [[nodiscard]] std::expected<void, std::string> enumerate_ports() {
        [[maybe_unused]] auto log = detail::platform_logger();
        uint16_t count = rte_eth_dev_count_avail();

        if (count == 0) {
            SPDLOG_LOGGER_ERROR(log,
                "No DPDK ports available (count=0); "
                "check VFIO binding and hugepage configuration");
            return std::unexpected("No DPDK ports available; check VFIO binding");
        }
        SPDLOG_LOGGER_INFO(log, "Available DPDK ports: {}", count);

        if (config.port_id >= count) {
            SPDLOG_LOGGER_ERROR(log,
                "Requested port_id={} but only {} port(s) available",
                config.port_id, count);
            return std::unexpected(std::format(
                "port_id {} out of range (available ports: {})",
                config.port_id, count));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> create_mempool() {
        [[maybe_unused]] auto log = detail::platform_logger();
        SPDLOG_LOGGER_DEBUG(log,
            "Creating mbuf pool: size={}, cache={}, data_room={}, per_lcore_pools={}",
            config.mbuf_pool_size, config.mbuf_cache_size,
            RTE_MBUF_DEFAULT_BUF_SIZE, config.per_lcore_pools);

        // ── Default path: single shared pool ─────────────────────────────
        // Byte-for-byte identical to the pre-T2.9 layout. `per_lcore_pool`
        // stays empty; `pool_for_lcore()` falls back to `mempool` for any
        // lcore id.
        if (config.per_lcore_pools == 0) {
            // Use a per-port pool name so that multiple Platform instances
            // (one per port) can coexist without EEXIST failure from
            // rte_pktmbuf_pool_create.
            auto pool_name = std::format("eph_mbuf_p{}", config.port_id);
            mempool = rte_pktmbuf_pool_create(
                pool_name.c_str(), config.mbuf_pool_size, config.mbuf_cache_size,
                0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);

            if (mempool == nullptr) {
                SPDLOG_LOGGER_ERROR(log,
                    "rte_pktmbuf_pool_create failed: pool_size={}, rte_errno={}: {}",
                    config.mbuf_pool_size, rte_errno, rte_strerror(rte_errno));
                return std::unexpected(std::format(
                    "Failed to create mbuf pool (rte_errno={}): {}",
                    rte_errno, rte_strerror(rte_errno)));
            }
            SPDLOG_LOGGER_DEBUG(log, "mbuf pool created at {:p}",
                                static_cast<void*>(mempool));
            return {};
        }

        // ── Per-lcore / NUMA-aware path (T2.9) ──────────────────────────
        //
        // For each lcore id `[0, per_lcore_pools)`, create a dedicated
        // pool on the lcore's local NUMA socket. Pool naming includes the
        // lcore id so multiple per-lcore pools across one or more
        // Platforms coexist without name clashes.
        //
        // Failure mode: if any pool fails to create, we free the ones we
        // already built and surface a structured error. Partial state is
        // never visible to the public accessors.
        const std::size_t n_pools =
            std::min<std::size_t>(config.per_lcore_pools, Impl::kMaxPools);
        for (std::size_t i = 0; i < n_pools; ++i) {
            // Resolve the lcore's local NUMA socket. `rte_lcore_to_socket_id`
            // returns 0 on hosts without NUMA awareness or for inactive
            // lcore slots — that's fine: all pools land on socket 0
            // and the layout is functionally a regular shared-pool array.
            // If the EAL didn't enable this lcore, fall back to
            // `SOCKET_ID_ANY` so DPDK picks any socket rather than fail.
            int socket_id = SOCKET_ID_ANY;
            if (rte_lcore_is_enabled(static_cast<unsigned>(i))) {
                socket_id = static_cast<int>(
                    rte_lcore_to_socket_id(static_cast<unsigned>(i)));
            }

            auto pool_name = std::format("eph_mbuf_p{}_l{}",
                                          config.port_id, i);
            rte_mempool* p = rte_pktmbuf_pool_create(
                pool_name.c_str(),
                config.mbuf_pool_size, config.mbuf_cache_size,
                /*priv_size=*/0,
                RTE_MBUF_DEFAULT_BUF_SIZE,
                socket_id);
            if (p == nullptr) {
                const int err = rte_errno;
                SPDLOG_LOGGER_ERROR(log,
                    "rte_pktmbuf_pool_create('{}', socket={}) failed: "
                    "pool_size={}, rte_errno={}: {} — rolling back partial "
                    "per-lcore pool init",
                    pool_name, socket_id, config.mbuf_pool_size,
                    err, rte_strerror(err));
                // Roll back: free any pools we already created so the
                // Platform never holds half-populated state.
                for (std::size_t j = 0; j < i; ++j) {
                    if (per_lcore_pool[j] != nullptr) {
                        rte_mempool_free(per_lcore_pool[j]);
                        per_lcore_pool[j] = nullptr;
                    }
                }
                return std::unexpected(std::format(
                    "Failed to create per-lcore mbuf pool '{}' "
                    "(socket={}, rte_errno={}): {}",
                    pool_name, socket_id, err, rte_strerror(err)));
            }
            per_lcore_pool[i] = p;
            SPDLOG_LOGGER_DEBUG(log,
                "per-lcore pool created: lcore={}, socket={}, pool='{}', ptr={:p}",
                i, socket_id, pool_name, static_cast<void*>(p));
        }

        // Pin `mempool` (the legacy accessor) at slot 0 so RX queue setup
        // and any non-lcore-aware caller (e.g. existing stream creation
        // call sites that haven't migrated to `pool_lcore_hint`) draw from
        // a real, live pool. Slot 0 is always populated when n_pools > 0.
        mempool = per_lcore_pool[0];
        SPDLOG_LOGGER_INFO(log,
            "per-lcore mempool layout active: pools={}, pool_size={}, "
            "cache={}, canonical (lcore=0) at {:p}",
            n_pools, config.mbuf_pool_size, config.mbuf_cache_size,
            static_cast<void*>(mempool));
        return {};
    }

    /// Secondary-mode mempool attach. Looks up the mempool the primary
    /// already created under the well-known name `eph_mbuf_p<port>`. A
    /// miss means the primary is not running *or* the EAL file-prefix
    /// doesn't match — both surface as the same `ENOENT`-shaped error.
    ///
    /// When `per_lcore_pools > 0`, also attaches each per-lcore pool the
    /// primary created (`eph_mbuf_p<port>_l<lcore>`). Callers in the
    /// secondary process must pass the same `per_lcore_pools` value the
    /// primary used; mismatched configs surface as ENOENT on the first
    /// missing slot.
    [[nodiscard]] std::expected<void, std::string> lookup_mempool_secondary() {
        [[maybe_unused]] auto log = detail::platform_logger();

        // ── Default path: single shared pool (legacy layout) ───────────
        if (config.per_lcore_pools == 0) {
            auto pool_name = std::format("eph_mbuf_p{}", config.port_id);
            mempool = rte_mempool_lookup(pool_name.c_str());
            if (mempool == nullptr) {
                const int err = rte_errno;
                SPDLOG_LOGGER_ERROR(log,
                    "rte_mempool_lookup('{}') failed — primary not running or "
                    "file_prefix mismatch (expected runtime dir "
                    "/var/run/dpdk/{}/, rte_errno={}): {}",
                    pool_name,
                    config.file_prefix.empty() ? std::string{"<default>"}
                                               : std::string{config.file_prefix},
                    err, rte_strerror(err));
                return std::unexpected(std::format(
                    "rte_mempool_lookup('{}') failed — primary not running or "
                    "file_prefix mismatch (looked for runtime dir /var/run/dpdk/{}/, "
                    "rte_errno={} ({}))",
                    pool_name,
                    config.file_prefix.empty() ? std::string{"<default>"}
                                               : std::string{config.file_prefix},
                    err, rte_strerror(err)));
            }
            SPDLOG_LOGGER_INFO(log,
                "Secondary attached to mempool '{}' at {:p} (shared from primary)",
                pool_name, static_cast<void*>(mempool));
            return {};
        }

        // ── Per-lcore path: attach every primary-created per-lcore pool ─
        const std::size_t n_pools =
            std::min<std::size_t>(config.per_lcore_pools, Impl::kMaxPools);
        for (std::size_t i = 0; i < n_pools; ++i) {
            auto pool_name = std::format("eph_mbuf_p{}_l{}",
                                          config.port_id, i);
            rte_mempool* p = rte_mempool_lookup(pool_name.c_str());
            if (p == nullptr) {
                const int err = rte_errno;
                SPDLOG_LOGGER_ERROR(log,
                    "rte_mempool_lookup('{}') failed in secondary "
                    "(per_lcore_pools={}; expected primary to have created "
                    "all {} pools, rte_errno={}): {}",
                    pool_name, config.per_lcore_pools, n_pools,
                    err, rte_strerror(err));
                // Don't free per_lcore_pool entries — secondary never owns
                // the underlying pool memory; we just zero our view so the
                // Platform isn't half-attached.
                for (std::size_t j = 0; j < i; ++j) per_lcore_pool[j] = nullptr;
                return std::unexpected(std::format(
                    "rte_mempool_lookup('{}') failed in secondary "
                    "(per_lcore_pools={}, rte_errno={}: {})",
                    pool_name, config.per_lcore_pools, err, rte_strerror(err)));
            }
            per_lcore_pool[i] = p;
        }
        mempool = per_lcore_pool[0];
        SPDLOG_LOGGER_INFO(log,
            "Secondary attached to per-lcore mempool layout: pools={}, "
            "canonical (lcore=0) at {:p}",
            n_pools, static_cast<void*>(mempool));
        return {};
    }

    [[nodiscard]] std::expected<void, std::string>
    configure_port(const rte_eth_dev_info& dev_info) {
        [[maybe_unused]] auto log = detail::platform_logger();

        SPDLOG_LOGGER_DEBUG(log,
            "port={} driver={} max_rx_q={} max_tx_q={} "
            "rx_offload_capa={:#x} tx_offload_capa={:#x}",
            config.port_id,
            dev_info.driver_name ? dev_info.driver_name : "unknown",
            dev_info.max_rx_queues, dev_info.max_tx_queues,
            dev_info.rx_offload_capa, dev_info.tx_offload_capa);

        // Clamp queue counts to NIC capabilities — passing a value that exceeds
        // max_rx/tx_queues causes rte_eth_dev_configure to fail with EINVAL.
        uint16_t nb_rx = std::min(config.nb_rx_queues, dev_info.max_rx_queues);
        uint16_t nb_tx = std::min(config.nb_tx_queues, dev_info.max_tx_queues);
        if (nb_rx != config.nb_rx_queues) {
            SPDLOG_LOGGER_WARN(log,
                "nb_rx_queues={} exceeds NIC max={}; clamped to {}",
                config.nb_rx_queues, dev_info.max_rx_queues, nb_rx);
            config.nb_rx_queues = nb_rx;
        }
        if (nb_tx != config.nb_tx_queues) {
            SPDLOG_LOGGER_WARN(log,
                "nb_tx_queues={} exceeds NIC max={}; clamped to {}",
                config.nb_tx_queues, dev_info.max_tx_queues, nb_tx);
            config.nb_tx_queues = nb_tx;
        }

        rte_eth_conf eth_conf{};
        // No offloads requested — conservative default for minimal setup.
        // Value-initialization above already zero-initializes all fields.
        // TX checksum offload is handled per-packet via PacketTemplate::hw_cksum.
        eth_conf.rxmode.offloads = 0;
        eth_conf.txmode.offloads = 0;

        // Opt-in: RX checksum offload. When enabled, the NIC computes IPv4
        // header + L4 (UDP + TCP) checksums and stamps the result into
        // `mbuf->ol_flags` as RTE_MBUF_F_RX_{IP,L4}_CKSUM_{GOOD,BAD,UNKNOWN}.
        // Both DpdkUdpSocket and DpdkTcpStream's RX hot paths consume those
        // flags to drop BAD packets before codec dispatch (see
        // StreamMetric::kRxBadChecksum). If the NIC lacks any of the three
        // capabilities, we WARN once and request only the subset the NIC
        // supports. We never abort: the worst-case outcome is "same as
        // opt-in off".
        if (config.enable_rx_checksum_offload) {
            constexpr uint64_t kWantIp =
                static_cast<uint64_t>(RTE_ETH_RX_OFFLOAD_IPV4_CKSUM);
            constexpr uint64_t kWantUdp =
                static_cast<uint64_t>(RTE_ETH_RX_OFFLOAD_UDP_CKSUM);
            constexpr uint64_t kWantTcp =
                static_cast<uint64_t>(RTE_ETH_RX_OFFLOAD_TCP_CKSUM);
            const uint64_t have_ip  = dev_info.rx_offload_capa & kWantIp;
            const uint64_t have_udp = dev_info.rx_offload_capa & kWantUdp;
            const uint64_t have_tcp = dev_info.rx_offload_capa & kWantTcp;
            eth_conf.rxmode.offloads |= (have_ip | have_udp | have_tcp);

            if (!have_ip || !have_udp || !have_tcp) {
                SPDLOG_LOGGER_WARN(log,
                    "port={} enable_rx_checksum_offload=true but NIC lacks"
                    " capability: ipv4={} udp={} tcp={} (rx_offload_capa={:#x})"
                    " - proceeding without the missing flag(s); bad-cksum"
                    " packets on unsupported layers will not be detected",
                    config.port_id,
                    have_ip  ? "ok" : "MISSING",
                    have_udp ? "ok" : "MISSING",
                    have_tcp ? "ok" : "MISSING",
                    dev_info.rx_offload_capa);
            } else {
                SPDLOG_LOGGER_DEBUG(log,
                    "port={} RX checksum offload enabled (IPv4 + UDP + TCP)",
                    config.port_id);
            }
        }

        // RSS multi-queue mode. Must be set BEFORE rte_eth_dev_configure;
        // rss_hash_update later cannot upgrade single-queue → multi-queue.
        // Hash flags are intersected with NIC capability — passing flags the
        // PMD does not advertise causes EINVAL.
        if (nb_rx > 1) {
            eth_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
            eth_conf.rx_adv_conf.rss_conf.rss_key = nullptr;
            eth_conf.rx_adv_conf.rss_conf.rss_key_len = 0;
            eth_conf.rx_adv_conf.rss_conf.rss_hf =
                dev_info.flow_type_rss_offloads &
                (RTE_ETH_RSS_NONFRAG_IPV4_TCP |
                 RTE_ETH_RSS_NONFRAG_IPV4_UDP |
                 RTE_ETH_RSS_IPV4);
            if (eth_conf.rx_adv_conf.rss_conf.rss_hf == 0) {
                SPDLOG_LOGGER_WARN(log,
                    "port={} nb_rx_queues > 1 but NIC reports no IPv4 TCP/UDP "
                    "RSS hash offloads (flow_type_rss_offloads=0x{:016x}); "
                    "RSS will be inactive — falling back to single-queue dispatch",
                    config.port_id, dev_info.flow_type_rss_offloads);
                eth_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
            }
        }

        int ret = rte_eth_dev_configure(config.port_id, nb_rx, nb_tx, &eth_conf);
        if (ret != 0) {
            SPDLOG_LOGGER_ERROR(log,
                "rte_eth_dev_configure(port={}) failed: ret={}: {}",
                config.port_id, ret, rte_strerror(-ret));
            return std::unexpected(std::format(
                "eth_dev_configure failed for port {} (ret={}): {}",
                config.port_id, ret, rte_strerror(-ret)));
        }
        SPDLOG_LOGGER_DEBUG(log, "port={} configured", config.port_id);
        return {};
    }

    [[nodiscard]] std::expected<void, std::string>
    setup_queues(const rte_eth_dev_info& dev_info) {
        [[maybe_unused]] auto log = detail::platform_logger();

        uint16_t rx_desc = detail::clamp_desc(config.nb_rx_desc,
                                               dev_info.rx_desc_lim);
        uint16_t tx_desc = detail::clamp_desc(config.nb_tx_desc,
                                               dev_info.tx_desc_lim);

        if (rx_desc != config.nb_rx_desc)
            SPDLOG_LOGGER_WARN(log,
                "nb_rx_desc adjusted {} -> {} [min={}, max={}, align={}]",
                config.nb_rx_desc, rx_desc,
                dev_info.rx_desc_lim.nb_min, dev_info.rx_desc_lim.nb_max,
                dev_info.rx_desc_lim.nb_align);
        if (tx_desc != config.nb_tx_desc)
            SPDLOG_LOGGER_WARN(log,
                "nb_tx_desc adjusted {} -> {} [min={}, max={}, align={}]",
                config.nb_tx_desc, tx_desc,
                dev_info.tx_desc_lim.nb_min, dev_info.tx_desc_lim.nb_max,
                dev_info.tx_desc_lim.nb_align);

        SPDLOG_LOGGER_DEBUG(log,
            "Setting up {} RX queue(s) x {} descs, {} TX queue(s) x {} descs",
            config.nb_rx_queues, rx_desc, config.nb_tx_queues, tx_desc);

        for (uint16_t q = 0; q < config.nb_rx_queues; ++q) {
            int ret = rte_eth_rx_queue_setup(
                config.port_id, q, rx_desc, SOCKET_ID_ANY, nullptr, mempool);
            if (ret != 0) {
                SPDLOG_LOGGER_ERROR(log,
                    "eth_rx_queue_setup(port={}, queue={}) failed: ret={}: {}",
                    config.port_id, q, ret, rte_strerror(-ret));
                return std::unexpected(std::format(
                    "eth_rx_queue_setup failed (port={}, queue={}, ret={}): {}",
                    config.port_id, q, ret, rte_strerror(-ret)));
            }
        }
        for (uint16_t q = 0; q < config.nb_tx_queues; ++q) {
            int ret = rte_eth_tx_queue_setup(
                config.port_id, q, tx_desc, SOCKET_ID_ANY, nullptr);
            if (ret != 0) {
                SPDLOG_LOGGER_ERROR(log,
                    "eth_tx_queue_setup(port={}, queue={}) failed: ret={}: {}",
                    config.port_id, q, ret, rte_strerror(-ret));
                return std::unexpected(std::format(
                    "eth_tx_queue_setup failed (port={}, queue={}, ret={}): {}",
                    config.port_id, q, ret, rte_strerror(-ret)));
            }
        }
        SPDLOG_LOGGER_DEBUG(log, "All queues configured for port={}",
                            config.port_id);
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> start_port() {
        [[maybe_unused]] auto log = detail::platform_logger();

        if (config.enable_promiscuous) {
            int ret = rte_eth_promiscuous_enable(config.port_id);
            if (ret != 0) {
                SPDLOG_LOGGER_WARN(log,
                    "eth_promiscuous_enable(port={}) failed: ret={} "
                    "(promiscuous mode will be inactive)",
                    config.port_id, ret);
            } else {
                promiscuous_active = true;
                SPDLOG_LOGGER_DEBUG(log,
                    "port={} promiscuous mode enabled", config.port_id);
            }
        }

        int ret = rte_eth_dev_start(config.port_id);
        if (ret != 0) {
            SPDLOG_LOGGER_ERROR(log,
                "rte_eth_dev_start(port={}) failed: ret={}: {}",
                config.port_id, ret, rte_strerror(-ret));
            return std::unexpected(std::format(
                "eth_dev_start failed for port {} (ret={}): {}",
                config.port_id, ret, rte_strerror(-ret)));
        }
        port_started = true;
        SPDLOG_LOGGER_DEBUG(log, "port={} started", config.port_id);
        return {};
    }

    void wait_link_up() {
        [[maybe_unused]] auto log = detail::platform_logger();
        using namespace std::chrono;

        auto check_once = [&]() -> bool {
            rte_eth_link link{};
            // Return value only indicates query failure, not link state.
            // On failure, link struct is zeroed → link_status == DOWN.
            // Surface query failures at TRACE so a persistent
            // rte_eth_link_get_nowait error path (PMD bug, port closed
            // out from under us) is visible without spamming logs in
            // the link-down-but-NIC-ok common case.
            int ret = rte_eth_link_get_nowait(config.port_id, &link);
            if (ret != 0) {
                SPDLOG_LOGGER_TRACE(log,
                    "port={} rte_eth_link_get_nowait ret={} ({}) "
                    "— treating as link DOWN",
                    config.port_id, ret, rte_strerror(-ret));
            }
            if (link.link_status == RTE_ETH_LINK_UP) {
                SPDLOG_LOGGER_INFO(log, "port={} link up: {} Mbps {}",
                    config.port_id, link.link_speed,
                    link.link_duplex ? "full-duplex" : "half-duplex");
                return true;
            }
            return false;
        };

        if (config.link_timeout_ms == 0) {
            if (!check_once())
                SPDLOG_LOGGER_WARN(log,
                    "port={} link not yet up (timeout=0); "
                    "link may negotiate asynchronously", config.port_id);
            return;
        }

        auto deadline = steady_clock::now()
                      + milliseconds(config.link_timeout_ms);
        while (steady_clock::now() < deadline) {
            if (check_once()) return;
            std::this_thread::sleep_for(milliseconds(10));
        }
        SPDLOG_LOGGER_WARN(log,
            "port={} link not yet up after {}ms; "
            "continuing — link may negotiate asynchronously",
            config.port_id, config.link_timeout_ms);
    }

    void cleanup() noexcept {
        [[maybe_unused]] auto log = detail::platform_logger();

        // Secondary-mode cleanup is intentionally narrow: we must NOT call
        // rte_eth_dev_stop/close or rte_mempool_free — those would corrupt
        // the primary's port state or free memory the primary still owns.
        // Stream / Poller / FlowRule teardown happens through their own
        // RAII chains in the owning objects; Platform::Impl only zeroes
        // the shared-view pointers here so any lingering accessor returns
        // nullptr instead of a dangling primary-owned handle.
        if (config.proc_type == ProcType::Secondary) {
            SPDLOG_LOGGER_DEBUG(log,
                "secondary cleanup (port={}, file_prefix='{}'): "
                "stream/poller teardown only, port + mempool untouched "
                "(owned by primary)",
                config.port_id, config.file_prefix);
            port_started = false;
            // Zero the pointer view — we never owned the underlying pool.
            mempool = nullptr;
            // Per-lcore views are also primary-owned; just zero the slots.
            for (auto& slot : per_lcore_pool) slot = nullptr;
            // Clear the per-queue Poller registry so any late lookup
            // misses cleanly rather than returning a stale Poller*.
            for (auto& slot : pollers) slot = nullptr;
            return;
        }

        if (port_started) {
            SPDLOG_LOGGER_DEBUG(log, "Stopping port={}", config.port_id);
            rte_eth_dev_stop(config.port_id);
            rte_eth_dev_close(config.port_id);
            port_started = false;
        }
        // Per-lcore pools (if any) own the mbuf memory in primary mode.
        // Free each populated slot. When `per_lcore_pools == 0`, the
        // canonical `mempool` is the single shared pool and gets freed
        // by the fallback branch below; in per-lcore mode `mempool`
        // aliases `per_lcore_pool[0]`, so we must NOT double-free it.
        bool freed_via_per_lcore = false;
        for (auto& slot : per_lcore_pool) {
            if (slot != nullptr) {
                SPDLOG_LOGGER_DEBUG(log, "Freeing per-lcore mbuf pool {:p}",
                                    static_cast<void*>(slot));
                rte_mempool_free(slot);
                slot = nullptr;
                freed_via_per_lcore = true;
            }
        }
        if (freed_via_per_lcore) {
            // `mempool` was an alias to per_lcore_pool[0] — it's already
            // freed. Zero the alias so accessors return nullptr.
            mempool = nullptr;
        } else if (mempool != nullptr) {
            SPDLOG_LOGGER_DEBUG(log, "Freeing mbuf pool {:p}",
                                static_cast<void*>(mempool));
            rte_mempool_free(mempool);
            mempool = nullptr;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Platform method definitions
// ─────────────────────────────────────────────────────────────────────────────

inline Platform::Platform(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline Platform::~Platform() = default;

inline Platform::Platform(Platform&&) noexcept            = default;
inline Platform& Platform::operator=(Platform&&) noexcept = default;

[[nodiscard]] inline std::expected<Platform, std::string>
Platform::create(const PlatformConfig& config) {
    [[maybe_unused]] auto log = detail::platform_logger();

    if (auto err = validate_config(config); !err.empty()) {
        SPDLOG_LOGGER_ERROR(log, "Invalid PlatformConfig: {}", err);
        return std::unexpected(std::string{err});
    }

    // Surface non-fatal misconfigurations (undersized rings, promiscuous mode,
    // zero link-timeout, etc.) at WARN so operators see them in production
    // logs. Advisory only — does not block construction.
    for (const auto& w : config.warnings()) {
        SPDLOG_LOGGER_WARN(log, "PlatformConfig advisory: {}", w);
    }

    auto impl    = std::make_unique<Impl>();
    impl->config = config;

    if (auto r = impl->enumerate_ports(); !r) return std::unexpected(r.error());
    if (auto r = impl->create_mempool();  !r) return std::unexpected(r.error());

    // Query NIC capabilities once — offload flags MUST be intersected with
    // device caps; hard-coding flags is the most common portability bug.
    // Shared between configure_port() and setup_queues() to avoid redundant
    // DPDK syscalls.
    rte_eth_dev_info dev_info{};
    if (int ret = rte_eth_dev_info_get(config.port_id, &dev_info); ret != 0) {
        SPDLOG_LOGGER_ERROR(log,
            "rte_eth_dev_info_get(port={}) failed: ret={}",
            config.port_id, ret);
        return std::unexpected(std::format(
            "eth_dev_info_get failed for port {} (ret={})",
            config.port_id, ret));
    }

    if (auto r = impl->configure_port(dev_info); !r) return std::unexpected(r.error());
    if (auto r = impl->setup_queues(dev_info);   !r) return std::unexpected(r.error());

    // RSS hash key + RETA must be installed BEFORE rte_eth_dev_start.
    // configure_port already set mq_mode=RTE_ETH_MQ_RX_RSS in eth_conf when
    // nb_rx_queues > 1; here we wire up the actual hash params.
    //
    // Failure is NOT silently absorbed any more (commit BREAKING CHANGE):
    //   * If configure_rss succeeds: rss_active=true (eph's own key
    //     installed). Common path for non-ENA PMDs.
    //   * If configure_rss fails: we record the error and try a
    //     probe-based bring-up after `start_port` (some PMDs — notably
    //     ENA — reject `rte_eth_dev_rss_hash_update` but expose the
    //     in-use hash key via `rte_eth_dev_rss_hash_conf_get`, which we
    //     can use for `predict_rss_queue` predictions).
    //   * If both fail and the user asked for `nb_rx_queues > 1`,
    //     `Platform::create` returns an error rather than silently
    //     collapsing to single-queue (the previous behaviour, which hid
    //     a real configuration mismatch behind an INFO log).
    std::string configure_rss_error;
    if (impl->config.nb_rx_queues > 1) {
        auto rss_r = ::eph::net::dpdk::configure_rss(
            config.port_id, impl->config.nb_rx_queues);
        if (rss_r) {
            impl->rss_active = true;
        } else {
            configure_rss_error = rss_r.error();
            SPDLOG_LOGGER_WARN(log,
                "configure_rss(port={}, queues={}) failed: {} -- "
                "will attempt probe-based bring-up after port start",
                config.port_id, impl->config.nb_rx_queues, configure_rss_error);
        }
    }

    if (auto r = impl->start_port();             !r) return std::unexpected(r.error());
    impl->wait_link_up();

    // Probe live NIC capability AFTER port start (rte_flow_validate needs the
    // port up). Cache the result for the lifetime of the Platform — Stream
    // attach paths read it but do not re-probe.
    impl->dispatch_mode =
        ::eph::net::dpdk::detect_rx_dispatch_mode(config.port_id);

    // ── Probe-based RSS bring-up (post-start) ────────────────────────────
    //
    // configure_rss earlier may have failed because the PMD rejects
    // rte_eth_dev_rss_hash_update (notably ENA, regardless of the
    // rss_key argument). Many such PMDs still expose their in-use hash
    // key via rte_eth_dev_rss_hash_conf_get; if so, predict_rss_queue
    // can use the probed key transparently and multi-queue RSS is
    // genuinely usable. We probe AFTER port start because some PMDs
    // only return meaningful RSS state once the device is running.
    if (impl->config.nb_rx_queues > 1 && !impl->rss_active) {
        SPDLOG_LOGGER_WARN(log,
            "Platform: configure_rss failed earlier on port={} ('{}'); "
            "attempting probe-based bring-up via "
            "rte_eth_dev_rss_hash_conf_get",
            config.port_id, configure_rss_error);
        auto probed = ::eph::net::dpdk::query_rss_state(config.port_id);
        if (probed && probed->key_len > 0) {
            SPDLOG_LOGGER_INFO(log,
                "Platform: probe succeeded on port={} (key_len={}, "
                "reta_size={}); RSS active via probed key — multi-queue "
                "RssPartitioned usable",
                config.port_id, probed->key_len, probed->reta_size);
            impl->rss_active           = true;
            impl->rss_using_probed_key = true;
        } else {
            const std::string probe_err = probed
                ? std::string{"rss_hash_conf_get returned key_len=0 (PMD "
                              "won't expose its hash key)"}
                : probed.error();
            SPDLOG_LOGGER_ERROR(log,
                "Platform: RSS bring-up failed on port={}: configure_rss "
                "rejected ('{}') AND probe also failed ('{}'); cannot "
                "safely route packets to nb_rx_queues={}",
                config.port_id, configure_rss_error, probe_err,
                impl->config.nb_rx_queues);
            return std::unexpected(std::format(
                "Multi-queue RSS bring-up failed on port={}: "
                "configure_rss rejected ('{}') AND rss_hash_conf_get "
                "probe also failed ('{}'). Cannot safely route packets "
                "to nb_rx_queues={}. Recovery: set "
                "PlatformConfig::nb_rx_queues=1.",
                config.port_id, configure_rss_error, probe_err,
                impl->config.nb_rx_queues));
        }
    }

    // Reflect what THIS Platform is actually doing, not just NIC capability.
    // detect_rx_dispatch_mode reports the NIC's intrinsic capabilities;
    // if we didn't bring up RSS (single-queue config: nb_rx_queues == 1),
    // dispatch_mode is effectively Software for the purposes of stream
    // attach decisions. Without this pin, Stream::create_and_attach would
    // walk the RssPartitioned branch and call predict_rss_queue + attach
    // to a non-existent target Poller.
    if (impl->config.nb_rx_queues <= 1 || !impl->rss_active) {
        if (impl->dispatch_mode !=
                ::eph::net::dpdk::RxDispatchMode::Software) {
            SPDLOG_LOGGER_INFO(log,
                "Platform: NIC supports {} but RSS not active "
                "(nb_rx_queues={}, rss_active={}); pinning dispatch_mode "
                "to Software for attach decisions",
                ::eph::net::dpdk::rx_dispatch_mode_name(impl->dispatch_mode),
                impl->config.nb_rx_queues,
                impl->rss_active ? "true" : "false");
            impl->dispatch_mode = ::eph::net::dpdk::RxDispatchMode::Software;
        }
    }

    // ── Hard-fail the legitimate-but-unsafe combination "nb_rx_queues>1
    //    with no functional RSS path" — happens when the PMD rejected both
    //    `configure_rss` (rss_hash_update) AND the probe-based fallback
    //    (rss_hash_conf_get returned key_len=0). The previous behaviour
    //    silently collapsed the RETA to queue 0, which masked the
    //    misconfiguration; we now refuse so the caller makes an explicit
    //    decision. (The configure_rss-failed-but-probe-succeeded path
    //    has set rss_active=true above and is exempt; the both-failed
    //    path returned earlier and never reaches here.)
    if (impl->config.nb_rx_queues > 1 && !impl->rss_active) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform: nb_rx_queues={} but rss_active=false on port={}; "
            "cannot route packets to multiple queues without functional RSS",
            impl->config.nb_rx_queues, config.port_id);
        return std::unexpected(std::format(
            "PlatformConfig has nb_rx_queues={} but RSS bring-up failed "
            "(both rss_hash_update and rss_hash_conf_get were rejected by "
            "the PMD); eph cannot route packets to multiple queues without "
            "a functional RSS path. Recovery: set nb_rx_queues=1 to use "
            "single-queue Software dispatch, or use a NIC whose PMD "
            "supports rss_hash_update or rss_hash_conf_get.",
            impl->config.nb_rx_queues));
    }

    SPDLOG_LOGGER_INFO(log,
        "Platform ready (port={}, nb_rx_queues={}, rss_active={}, "
        "using_probed_key={}, dispatch_mode={})",
        config.port_id, impl->config.nb_rx_queues,
        impl->rss_active ? "true" : "false",
        impl->rss_using_probed_key ? "true" : "false",
        ::eph::net::dpdk::rx_dispatch_mode_name(impl->dispatch_mode));
    return Platform(std::move(impl));
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-process factories (Primary + Secondary attach)
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline std::expected<Platform, std::string>
Platform::create_primary(PlatformConfig config) {
    [[maybe_unused]] auto log = detail::platform_logger();
    // Force-set the role so mis-assembled configs can't accidentally
    // travel into create() with Secondary marked but primary semantics
    // intended.
    config.proc_type = ProcType::Primary;

    // ── mp_topology auto-derivation (cold path) ─────────────────────────
    //
    // When the caller opts into MpTopology, we:
    //   1. validate the config (catches an invalid topology before any
    //      DPDK side-effect),
    //   2. reserve the cross-process registry memzone — the primary's
    //      reset-on-create contract (see detail/mp_registry.hpp),
    //   3. derive cfg.rx_queue_range from `self()`,
    //   4. clear `cfg.mp_topology` so the downstream `create()` sees a
    //      "manual partition" config and its mp_topology⇄rx_queue_range
    //      mutual-exclusion check still passes,
    //   5. attach the registry handle to the resulting Platform's Impl
    //      after `create()` succeeds (the handle's RAII destructor frees
    //      the memzone if `create()` fails).
    //
    // The original topology survives via `Impl::mp_registry` —
    // `has_mp_topology()` and `self_port_range()` consult it for the
    // stream-attach src_port narrowing path.
    std::optional<::eph::dpdk::detail::MpRegistryHandle> reg;
    if (config.mp_topology.has_value()) {
        if (auto err = validate_config(config); !err.empty()) {
            SPDLOG_LOGGER_ERROR(log,
                "Platform::create_primary: invalid PlatformConfig: {}", err);
            return std::unexpected(std::string{err});
        }
        if (config.file_prefix.empty()) {
            SPDLOG_LOGGER_ERROR(log,
                "Platform::create_primary: mp_topology requires a "
                "non-empty file_prefix (used as the registry memzone "
                "name eph_mp/<file_prefix>)");
            return std::unexpected(std::string{
                "create_primary: mp_topology set but file_prefix is empty"});
        }
        auto r = ::eph::dpdk::detail::MpRegistryHandle::create_primary(
            config.file_prefix, *config.mp_topology);
        if (!r) {
            SPDLOG_LOGGER_ERROR(log,
                "Platform::create_primary: registry reserve failed: {}",
                r.error().detail);
            return std::unexpected(std::string{r.error().detail});
        }
        reg = std::move(*r);
        const auto& self = config.mp_topology->self();
        config.rx_queue_range = {self.queue_lo, self.queue_hi};
        // Consumed: clear so create()'s validate_config doesn't trip
        // the mp_topology⇄rx_queue_range mutual-exclusion check.
        config.mp_topology.reset();

        SPDLOG_LOGGER_INFO(log,
            "Platform::create_primary: mp_topology derived "
            "rx_queue_range=[{},{}) for self_index (registry attached)",
            config.rx_queue_range.first, config.rx_queue_range.second);
    }

    auto p = create(config);
    if (!p) return p;  // reg's RAII frees the memzone on early-out

    if (reg.has_value() && p->impl_) {
        p->impl_->mp_registry = std::move(reg);
    }
    return p;
}

[[nodiscard]] inline std::expected<Platform, std::string>
Platform::create_secondary(PlatformConfig config) {
    [[maybe_unused]] auto log = detail::platform_logger();
    config.proc_type = ProcType::Secondary;

    // --- Secondary-mode contract validation ---
    //
    // The shared validator polices structural invariants (queue counts,
    // descriptors, mbuf pool, rx_queue_range vs nb_rx_queues). Anything
    // *secondary-specific* (file_prefix non-empty, port visibility under
    // shared hugepage) is checked here on top.
    //
    // Source-port partitioning across MP processes is the *caller's*
    // responsibility — `eph-net-dpdk` does not auto-allocate src_port,
    // and has no global view to enforce disjoint ranges. See
    // `PlatformConfig` comment + `docs/dpdk-multiprocess.md`.

    if (auto err = validate_config(config); !err.empty()) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::create_secondary: invalid PlatformConfig: {}", err);
        return std::unexpected(std::string{err});
    }

    if (config.file_prefix.empty()) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::create_secondary: file_prefix is empty "
            "(must match the primary's EAL --file-prefix)");
        return std::unexpected(std::string{
            "create_secondary: file_prefix must be non-empty and match "
            "the primary's EAL --file-prefix (otherwise rte_mempool_lookup "
            "cannot find the primary's shared mempool)"});
    }

    // ── mp_topology attach (cold path) ──────────────────────────────────
    //
    // Mirror of create_primary's auto-derivation, but in lookup mode:
    // the primary already wrote the registry header; we attach, cross-
    // validate that our declared spec matches the primary's view of
    // this self_index, derive cfg.rx_queue_range from `self()`, and
    // clear `cfg.mp_topology` so the rest of secondary bring-up sees a
    // "manual partition" config. The registry handle is moved into the
    // Impl after the rest of secondary attach succeeds; the handle's
    // RAII destructor releases the slot on early-out.
    std::optional<::eph::dpdk::detail::MpRegistryHandle> reg;
    if (config.mp_topology.has_value()) {
        auto r = ::eph::dpdk::detail::MpRegistryHandle::attach_secondary(
            config.file_prefix, *config.mp_topology);
        if (!r) {
            SPDLOG_LOGGER_ERROR(log,
                "Platform::create_secondary: registry attach failed: {}",
                r.error().detail);
            return std::unexpected(std::string{r.error().detail});
        }
        reg = std::move(*r);
        const auto& self = config.mp_topology->self();
        config.rx_queue_range = {self.queue_lo, self.queue_hi};
        config.mp_topology.reset();
        SPDLOG_LOGGER_INFO(log,
            "Platform::create_secondary: mp_topology derived "
            "rx_queue_range=[{},{}) (registry attached)",
            config.rx_queue_range.first, config.rx_queue_range.second);
    }

    // --- Phase-3 real secondary attach ---
    //
    // EAL has already joined the primary's runtime dir (via
    // `--proc-type=secondary --file-prefix=<same>`). We:
    //   1. validate the target port is visible from this process;
    //   2. look up the shared mempool by the primary's well-known name;
    //   3. skip configure/setup/start/rss — those are primary-only;
    //   4. mark port_started=true so accessors behave as "live" (we are
    //      sharing the primary's live port);
    //   5. probe dispatch_mode post-attach for `create_and_attach` to read.
    //
    // We deliberately do NOT call `rte_eth_dev_info_get` as a hard
    // prerequisite: some PMDs accept it in secondary, others reject it.
    // Validating `rte_eth_dev_is_valid_port` is sufficient — anything
    // deeper belongs in the first `rte_eth_rx_burst` which is on the
    // caller's hot path anyway.

    // Surface non-fatal misconfigurations (undersized rings, etc.) to
    // keep parity with create()'s advisory output.
    for (const auto& w : config.warnings()) {
        SPDLOG_LOGGER_WARN(log, "PlatformConfig advisory: {}", w);
    }

    if (!rte_eth_dev_is_valid_port(config.port_id)) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::create_secondary: port_id={} not valid from "
            "secondary — primary may not have started this port yet, "
            "or file_prefix may not match",
            config.port_id);
        return std::unexpected(std::format(
            "create_secondary: port_id {} not visible from secondary "
            "(is the primary running with the same --file-prefix, "
            "and did it start this port?)",
            config.port_id));
    }

    auto impl       = std::make_unique<Impl>();
    impl->config    = config;

    if (auto r = impl->lookup_mempool_secondary(); !r)
        return std::unexpected(r.error());

    // Best-effort sanity check: cross-check the caller's `nb_rx_queues`
    // against the live NIC. The secondary contract previously trusted
    // the caller blindly, so a mismatched config (e.g. caller said 4
    // but the primary actually configured 2) silently let `rr_counter`
    // hand out queue ids that the NIC has no rings for. We can't always
    // call `rte_eth_dev_info_get` from a secondary — some PMDs reject
    // it — so this is non-fatal: we WARN on mismatch / probe failure
    // and continue. ENA (the project's reference PMD) does respond.
    {
        rte_eth_dev_info dev_info{};
        int probe_ret = rte_eth_dev_info_get(config.port_id, &dev_info);
        if (probe_ret == 0) {
            if (dev_info.max_rx_queues > 0 &&
                config.nb_rx_queues > dev_info.max_rx_queues) {
                SPDLOG_LOGGER_WARN(log,
                    "create_secondary: caller nb_rx_queues={} exceeds live "
                    "max_rx_queues={} on port {} — rr_counter may hand out "
                    "queue ids the NIC has no rings for; check that the "
                    "secondary's PlatformConfig matches the primary's",
                    config.nb_rx_queues, dev_info.max_rx_queues,
                    config.port_id);
            }
        } else {
            SPDLOG_LOGGER_DEBUG(log,
                "create_secondary: rte_eth_dev_info_get probe rejected "
                "(ret={}, rte_errno={}: {}) — PMD does not support it from "
                "secondary; skipping nb_rx_queues cross-check (advisory)",
                probe_ret, rte_errno, rte_strerror(rte_errno));
        }
    }

    // Skip: enumerate_ports (primary did it) / create_mempool (lookup
    // above) / configure_port / setup_queues / configure_rss /
    // start_port / wait_link_up — all primary-only.
    impl->port_started = true;

    // Probe the live NIC dispatch capability just like create() does.
    // `detect_rx_dispatch_mode` is read-only so it's safe in secondary.
    impl->dispatch_mode =
        ::eph::net::dpdk::detect_rx_dispatch_mode(config.port_id);
    // Honor the same "effective Software when single-queue / RSS not
    // active" pin as create().
    if (impl->config.nb_rx_queues <= 1 &&
        impl->dispatch_mode != ::eph::net::dpdk::RxDispatchMode::Software) {
        impl->dispatch_mode = ::eph::net::dpdk::RxDispatchMode::Software;
    }

    if (reg.has_value()) {
        impl->mp_registry = std::move(reg);
    }

    SPDLOG_LOGGER_INFO(log,
        "Platform::create_secondary ready (port={}, file_prefix='{}', "
        "rx_queue_range=[{},{}), dispatch_mode={})",
        config.port_id, config.file_prefix,
        config.rx_queue_range.first, config.rx_queue_range.second,
        ::eph::net::dpdk::rx_dispatch_mode_name(impl->dispatch_mode));

    return Platform(std::move(impl));
}

// Null guards on all impl_-accessing methods protect against use on a
// moved-from Platform (move leaves impl_ == nullptr).
inline rte_mempool* Platform::mempool()          const noexcept { return impl_ ? impl_->mempool              : nullptr; }

inline rte_mempool* Platform::pool_for_lcore(uint16_t lcore_id) const noexcept {
    // Hot path: O(1) array index, no locks. The branching here is on a
    // cold field (`per_lcore_pools`) and trivially predictable per
    // Platform — modern branch predictors lock onto it after the first
    // call.
    if (impl_ == nullptr) return nullptr;
    // Per-lcore feature off → backwards-compat: every lcore sees the
    // same single shared pool. This keeps existing call sites
    // (which never knew about lcores) working unchanged when they use
    // `pool_for_lcore(rte_lcore_id())` instead of `mempool()`.
    if (impl_->config.per_lcore_pools == 0) {
        return impl_->mempool;
    }
    // Out-of-range lcore id → nullptr. Caller policy: log + fall back to
    // `mempool()` if a soft failure is preferable to abort.
    if (lcore_id >= impl_->config.per_lcore_pools ||
        lcore_id >= Impl::kMaxPools) {
        return nullptr;
    }
    return impl_->per_lcore_pool[lcore_id];
}

inline uint16_t     Platform::port_id()          const noexcept { return impl_ ? impl_->config.port_id       : 0; }
inline bool         Platform::is_running()       const noexcept { return impl_ && impl_->port_started; }
inline bool         Platform::is_promiscuous()   const noexcept { return impl_ && impl_->promiscuous_active; }
inline bool         Platform::strict_rx_checksum() const noexcept {
    // Gated by offload: strict is only meaningful when the NIC is actually
    // producing per-mbuf cksum flags (see PlatformConfig doc).
    return impl_ && impl_->config.enable_strict_rx_checksum
                 && impl_->config.enable_rx_checksum_offload;
}

inline ::eph::net::dpdk::RxDispatchMode
Platform::dispatch_mode() const noexcept {
    return impl_ ? impl_->dispatch_mode
                 : ::eph::net::dpdk::RxDispatchMode::Software;
}

inline uint16_t Platform::nb_rx_queues() const noexcept {
    return impl_ ? impl_->config.nb_rx_queues : 0;
}

inline bool Platform::rss_using_probed_key() const noexcept {
    return impl_ && impl_->rss_using_probed_key;
}

inline std::pair<uint16_t, uint16_t>
Platform::effective_rx_queue_range() const noexcept {
    if (!impl_) return {0, 0};
    const auto& r = impl_->config.rx_queue_range;
    // Sentinel {0, 0} means "use full range [0, nb_rx_queues)".
    // Any other value has been validated by validate_config (lo < hi <=
    // nb_rx_queues).
    if (r.first == 0 && r.second == 0)
        return {uint16_t{0}, impl_->config.nb_rx_queues};
    return r;
}

inline bool Platform::has_mp_topology() const noexcept {
    return impl_ && impl_->mp_registry.has_value();
}

inline std::optional<std::pair<uint32_t, uint32_t>>
Platform::self_port_range() const noexcept {
    if (!impl_ || !impl_->mp_registry.has_value()) return std::nullopt;
    const auto& slot = impl_->mp_registry->self();
    return std::pair{slot.port_lo, slot.port_hi};
}

inline std::expected<void, ::eph::core::ErrorInfo>
Platform::register_poller(uint16_t queue_id,
                          ::eph::net::dpdk::DpdkPoller<void>* poller) noexcept {
    if (!impl_)
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "Platform::register_poller: Platform is moved-from"});
    if (poller == nullptr)
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "Platform::register_poller: poller pointer is null"});
    if (queue_id >= impl_->config.nb_rx_queues || queue_id >= kMaxRssQueues) {
        SPDLOG_LOGGER_WARN(detail::platform_logger(),
            "Platform::register_poller: queue_id={} not in [0, {})",
            queue_id,
            std::min(impl_->config.nb_rx_queues, kMaxRssQueues));
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "Platform::register_poller: queue_id out of range"});
    }
    if (impl_->pollers[queue_id] != nullptr) {
        SPDLOG_LOGGER_WARN(detail::platform_logger(),
            "Platform::register_poller: queue_id={} already has a registered Poller",
            queue_id);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "Platform::register_poller: queue already has a registered Poller"});
    }
    // Reject the same Poller landing on two different queues. A Poller owns
    // one lcore and polls one RX queue; pointing two slots at it would mean
    // Stream::create_and_attach routes RX-burst dispatches from two queues
    // to a Poller that only calls rte_eth_rx_burst on one, so packets on
    // the unpolled queue are never consumed (silent tail drop). Linear
    // scan over kMaxRssQueues = 64 is trivial at attach time.
    for (uint16_t q = 0; q < impl_->config.nb_rx_queues && q < kMaxRssQueues; ++q) {
        if (impl_->pollers[q] == poller) {
            SPDLOG_LOGGER_WARN(detail::platform_logger(),
                "Platform::register_poller: poller={:p} already registered on "
                "queue={} — a Poller owns one RX queue exclusively",
                static_cast<void*>(poller), q);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "Platform::register_poller: poller already registered on another queue"});
        }
    }
    impl_->pollers[queue_id] = poller;
    // NB: the ICMP callback (`poller->set_icmp_callback(...)`) is NOT
    // wired here because DpdkPoller is only forward-declared at this
    // point (platform.hpp sits below poller.hpp in the dep graph).
    // `DpdkTcpStream::create_and_attach` installs the callback as part
    // of its attach sequence — by the time any stream attaches, both
    // headers are fully included.
    SPDLOG_LOGGER_DEBUG(detail::platform_logger(),
        "Poller registered: port={}, queue={}, ptr={:p}",
        impl_->config.port_id, queue_id, static_cast<void*>(poller));
    return {};
}

inline ::eph::net::dpdk::DpdkPoller<void>*
Platform::poller_for_queue(uint16_t queue_id) const noexcept {
    if (!impl_) return nullptr;
    if (queue_id >= impl_->config.nb_rx_queues || queue_id >= kMaxRssQueues)
        return nullptr;
    return impl_->pollers[queue_id];
}

inline void Platform::unregister_poller(uint16_t queue_id) noexcept {
    if (!impl_) return;
    if (queue_id >= impl_->config.nb_rx_queues || queue_id >= kMaxRssQueues)
        return;
    impl_->pollers[queue_id] = nullptr;
}

inline std::expected<Platform::IcmpTargetHandle, ::eph::core::ErrorInfo>
Platform::register_icmp_target(::eph::dpdk::net::ConnectionTuple tuple,
                                uint8_t  proto,
                                void*    stream,
                                IcmpMtuCallback cb) noexcept {
    if (!impl_ || !impl_->icmp_registry_sp) {
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "Platform::register_icmp_target: Platform is moved-from"});
    }
    return impl_->icmp_registry_sp->register_target(tuple, proto, stream, cb);
}

inline uint64_t Platform::icmp_frag_needed_dispatched() const noexcept {
    return (impl_ && impl_->icmp_registry_sp)
               ? impl_->icmp_registry_sp->dispatched()
               : 0;
}

inline std::shared_ptr<::eph::dpdk::detail::IcmpRegistry>
Platform::icmp_registry_shared_() const noexcept {
    return impl_ ? impl_->icmp_registry_sp : nullptr;
}

inline Platform::Stats Platform::collect_stats() const {
    [[maybe_unused]] auto log = detail::platform_logger();
    if (!impl_) {
        SPDLOG_LOGGER_WARN(log,
            "collect_stats() called on moved-from Platform; returning empty stats");
        return {};
    }
    rte_eth_stats raw{};
    int ret = rte_eth_stats_get(impl_->config.port_id, &raw);
    if (ret != 0) {
        SPDLOG_LOGGER_ERROR(log,
            "eth_stats_get(port={}) failed: ret={}; returning zeroed stats",
            impl_->config.port_id, ret);
        return {};
    }
    return Stats{
        .rx_packets = raw.ipackets,
        .tx_packets = raw.opackets,
        .rx_bytes   = raw.ibytes,
        .tx_bytes   = raw.obytes,
        .rx_missed  = raw.imissed,
        .rx_errors  = raw.ierrors,
        .tx_errors  = raw.oerrors,
    };
}

} // namespace eph::dpdk

// std::formatter specialization for Platform::Stats
template <>
struct std::formatter<eph::dpdk::Platform::Stats> : std::formatter<std::string> {
    auto format(const eph::dpdk::Platform::Stats& s, auto& ctx) const {
        return std::formatter<std::string>::format(s.dump(), ctx);
    }
};

/// @brief std::formatter for PlatformConfig — compact one-line summary.
template <>
struct std::formatter<eph::dpdk::PlatformConfig> : std::formatter<std::string> {
    auto format(const eph::dpdk::PlatformConfig& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("PlatformConfig(port={}, q={}rx/{}tx, desc={}rx/{}tx, "
                        "pool={}, promisc={})",
                c.port_id, c.nb_rx_queues, c.nb_tx_queues,
                c.nb_rx_desc, c.nb_tx_desc, c.mbuf_pool_size,
                c.enable_promiscuous ? "true" : "false"),
            ctx);
    }
};
