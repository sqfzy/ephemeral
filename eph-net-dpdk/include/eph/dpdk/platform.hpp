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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/error.hpp"                  // core::Error / core::ErrorInfo
#include "eph/dpdk/detail/icmp_registry.hpp"   // detail::IcmpRegistry
#include "eph/dpdk/detail/logger.hpp"
#include "eph/dpdk/packet_parse.hpp"           // ParsedIcmp for dispatch_icmp_
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
    /// @brief Enable RSS hashing across `nb_rx_queues` RX queues. When true and
    /// `nb_rx_queues > 1`, Platform::create() turns on `RTE_ETH_MQ_RX_RSS` in
    /// the eth_conf and calls `eph::net::dpdk::configure_rss()` before starting
    /// the port. After the port starts, `detect_rx_dispatch_mode()` is run
    /// once and the result is cached (see `Platform::dispatch_mode()`).
    /// Default false — single-queue Software mode, fully backwards compatible.
    bool     enable_rss      = false;
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

    /// Defaulted equality — all fields must match exactly.
    [[nodiscard]] friend bool operator==(const PlatformConfig&,
                                         const PlatformConfig&) = default;

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "PlatformConfig:\n"
            "  port_id: {}, queues: {}rx/{}tx, descriptors: {}rx/{}tx\n"
            "  mbuf pool: {} (cache: {}), promiscuous: {}, link_timeout: {}ms\n"
            "  rx_cksum_offload: {}, strict_rx_cksum: {}",
            port_id, nb_rx_queues, nb_tx_queues, nb_rx_desc, nb_tx_desc,
            mbuf_pool_size, mbuf_cache_size,
            enable_promiscuous ? "true" : "false", link_timeout_ms,
            enable_rx_checksum_offload ? "true" : "false",
            enable_strict_rx_checksum  ? "true" : "false");
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
        if (enable_rss && nb_rx_queues < 2)
            w.emplace_back(std::format(
                "enable_rss=true but nb_rx_queues={} -- RSS needs >=2 queues, "
                "the flag will be silently ignored (single-queue Software mode)",
                nb_rx_queues));
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
            "\"enable_promiscuous\":{},\"enable_rss\":{},"
            "\"enable_rx_checksum_offload\":{},"
            "\"enable_strict_rx_checksum\":{},"
            "\"link_timeout_ms\":{}}}",
            port_id, nb_rx_queues, nb_tx_queues,
            nb_rx_desc, nb_tx_desc,
            mbuf_pool_size, mbuf_cache_size,
            enable_promiscuous ? "true" : "false",
            enable_rss ? "true" : "false",
            enable_rx_checksum_offload ? "true" : "false",
            enable_strict_rx_checksum  ? "true" : "false",
            link_timeout_ms);
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
    [[nodiscard]] static std::expected<Platform, std::string>
    create(const PlatformConfig& config);

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
    [[nodiscard]] rte_mempool* mempool()          const noexcept;

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
    PlatformConfig config;
    rte_mempool*   mempool{nullptr};
    bool           port_started{false};
    bool           promiscuous_active{false};

    // RSS / multi-queue dispatch state (stage 3).
    bool           rss_active{false};   ///< True if configure_rss() succeeded
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
            "Creating mbuf pool: size={}, cache={}, data_room={}",
            config.mbuf_pool_size, config.mbuf_cache_size,
            RTE_MBUF_DEFAULT_BUF_SIZE);

        // Use a per-port pool name so that multiple Platform instances (one per
        // port) can coexist without EEXIST failure from rte_pktmbuf_pool_create.
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
        if (config.enable_rss && nb_rx > 1) {
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
                    "port={} enable_rss=true but NIC reports no IPv4 TCP/UDP "
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
            [[maybe_unused]] int ret = rte_eth_link_get_nowait(config.port_id, &link);
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
        if (port_started) {
            SPDLOG_LOGGER_DEBUG(log, "Stopping port={}", config.port_id);
            rte_eth_dev_stop(config.port_id);
            rte_eth_dev_close(config.port_id);
            port_started = false;
        }
        if (mempool != nullptr) {
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
    // enable_rss && nb_rx_queues > 1; here we wire up the actual hash params.
    // Failure is non-fatal: we log + degrade to single-queue Software fallback
    // so a NIC that doesn't fully support RSS doesn't bring the whole port down.
    if (config.enable_rss && impl->config.nb_rx_queues > 1) {
        auto rss_r = ::eph::net::dpdk::configure_rss(
            config.port_id, impl->config.nb_rx_queues);
        if (rss_r) {
            impl->rss_active = true;
        } else {
            SPDLOG_LOGGER_WARN(log,
                "configure_rss(port={}, queues={}) failed: {} -- "
                "continuing in single-queue Software fallback",
                config.port_id, impl->config.nb_rx_queues, rss_r.error());
        }
    }

    if (auto r = impl->start_port();             !r) return std::unexpected(r.error());
    impl->wait_link_up();

    // Probe live NIC capability AFTER port start (rte_flow_validate needs the
    // port up). Cache the result for the lifetime of the Platform — Stream
    // attach paths read it but do not re-probe.
    impl->dispatch_mode =
        ::eph::net::dpdk::detect_rx_dispatch_mode(config.port_id);

    // Reflect what THIS Platform is actually doing, not just NIC capability.
    // detect_rx_dispatch_mode reports the NIC's intrinsic capabilities;
    // if we didn't successfully bring up multi-queue RSS (single-queue
    // config OR configure_rss failed on a PMD that rejects hash_update),
    // dispatch_mode is effectively Software for the purposes of stream
    // attach decisions.  Without this pin, Stream::create_and_attach would
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

        // ── Real fix for the "single-Poller misses SYN-ACK in
        // non-zero queue" bug ──────────────────────────────────────────
        //
        // Even when dispatch_mode is pinned to Software, the NIC still
        // physically has nb_rx_queues > 1 queues active and its
        // intrinsic default RSS would scatter incoming packets across
        // them. A single-Poller user (which is the only valid topology
        // in Software mode) would silently miss every packet that
        // hashes to a non-zero queue — most visibly: TCP SYN-ACK lost
        // → connect timeout.
        //
        // Collapse the RETA: write a uniform indirection table where
        // every entry maps to queue 0. After this, the NIC's hash
        // calculation still happens but every result indexes to 0. The
        // other queues stay allocated (descriptor rings, mbuf cache)
        // but receive 0 packets. Trivial extra memory cost; correctness
        // win is total (no more silent drops).
        //
        // RETA update is supported on PMDs that reject hash_update
        // (notably ENA), so this works in the exact case that
        // motivated the fix. If it fails we log WARN and the original
        // bug returns — but at least it's surfaced.
        if (impl->config.nb_rx_queues > 1) {
            rte_eth_dev_info dinfo{};
            if (rte_eth_dev_info_get(config.port_id, &dinfo) == 0) {
                uint16_t reta_size = dinfo.reta_size;
                if (reta_size == 0) reta_size = 128;
                reta_size = std::min(
                    reta_size,
                    static_cast<uint16_t>(RTE_ETH_RSS_RETA_SIZE_512));
                rte_eth_rss_reta_entry64
                    reta[RTE_ETH_RSS_RETA_SIZE_512 /
                         RTE_ETH_RETA_GROUP_SIZE]{};
                const uint16_t groups =
                    reta_size / RTE_ETH_RETA_GROUP_SIZE;
                for (uint16_t i = 0; i < groups; ++i) {
                    reta[i].mask = ~uint64_t(0);
                    // entries[] zero-initialised → all map to queue 0
                }
                int rc = rte_eth_dev_rss_reta_update(
                    config.port_id, reta, reta_size);
                if (rc == 0) {
                    SPDLOG_LOGGER_INFO(log,
                        "Platform: collapsed RETA → queue 0 "
                        "(nb_rx_queues={} active, but Software mode → "
                        "single-Poller; all RX traffic routes to queue 0)",
                        impl->config.nb_rx_queues);
                } else {
                    // RETA update failed too.  Without it, the NIC's
                    // intrinsic default RSS will scatter packets across
                    // all N queues; a single-Poller user would silently
                    // drop everything that doesn't hash to queue 0
                    // (the original SYN-ACK-loss bug). Refuse to bring
                    // up the Platform rather than let the caller hit
                    // that bug undetected. Caller's recovery path: set
                    // nb_rx_queues=1 in PlatformConfig.
                    SPDLOG_LOGGER_ERROR(log,
                        "Platform: RETA collapse failed (ret={}) on PMD "
                        "that doesn't support rss_reta_update; "
                        "single-Poller would drop non-zero-queue packets. "
                        "Refusing to bring up multi-queue port. "
                        "Workaround: set nb_rx_queues=1 in PlatformConfig.",
                        rc);
                    return std::unexpected(std::format(
                        "RETA collapse failed (ret={}); refusing to start "
                        "Platform with nb_rx_queues={} on PMD that supports "
                        "neither rss_hash_update nor rss_reta_update. Set "
                        "PlatformConfig::nb_rx_queues=1 to recover.",
                        rc, impl->config.nb_rx_queues));
                }
            }
        }
    }

    SPDLOG_LOGGER_INFO(log,
        "Platform ready (port={}, nb_rx_queues={}, rss_active={}, "
        "dispatch_mode={})",
        config.port_id, impl->config.nb_rx_queues,
        impl->rss_active ? "true" : "false",
        ::eph::net::dpdk::rx_dispatch_mode_name(impl->dispatch_mode));
    return Platform(std::move(impl));
}

// Null guards on all impl_-accessing methods protect against use on a
// moved-from Platform (move leaves impl_ == nullptr).
inline rte_mempool* Platform::mempool()          const noexcept { return impl_ ? impl_->mempool              : nullptr; }
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
