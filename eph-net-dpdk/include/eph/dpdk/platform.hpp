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
#include <csignal>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <pthread.h>

#include <spdlog/spdlog.h>

#include "eph/core/error.hpp"                  // core::Error / core::ErrorInfo
#include "eph/dpdk/detail/bdf_sanitize.hpp"     // detail::sanitize_bdf_for_file_prefix
#include "eph/dpdk/detail/icmp_directory.hpp"  // detail::IcmpDirectoryHandle, IPC thunk
#include "eph/dpdk/detail/icmp_registry.hpp"   // detail::IcmpRegistry
#include "eph/dpdk/detail/logger.hpp"
#include "eph/dpdk/detail/mp_ipc.hpp"          // detail::MpIpcAction, mp_ipc_send_oneway
#include "eph/dpdk/detail/mp_registry.hpp"     // detail::MpRegistryHandle
#include "eph/dpdk/detail/queue_allocator.hpp" // detail::QueueAllocator + IPC payloads (S5)
#include "eph/dpdk/eal.hpp"                    // EalConfig / build_eal_argv / eal_init (internal helper)
#include "eph/dpdk/lcore_pin.hpp"              // LcorePin (typed pin spec for PlatformConfig)
#include "eph/dpdk/mp_topology.hpp"            // MpTopology + ProcSpec
#include "eph/dpdk/packet_parse.hpp"           // ParsedIcmp for dispatch_icmp_
#include "eph/dpdk/proc_type.hpp"              // ProcType enum + to_eal_string
#include "eph/net/dpdk/flow_steering.hpp"
#include "eph/utils/cpu.hpp"                   // CpuPinPolicy

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

namespace detail {

/// @brief Internal-only Platform bring-up config.
///
/// Mirror of the v3 `PlatformConfig` user-facing shape, plus the
/// role-and-topology fields the bring-up body needs at runtime
/// (`proc_type`, `mp_topology`, `rx_queue_range`). Public callers see
/// only `PlatformConfig` (single-process) and `CreateOrJoinConfig`
/// (autojoin); `BringupConfig` is synthesized internally by the v3
/// entry points and the `primary_bringup_` / `secondary_bringup_`
/// helpers.
///
/// @note Queue counts and descriptor counts are automatically clamped
///       to NIC-reported limits during the bring-up body. Values here
///       are the *requested* values — actual values may be smaller.
struct BringupConfig {
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
    // All fields default to single-process / primary semantics. Multi-
    // process callers opt in by setting proc_type / file_prefix and (for
    // secondary) an explicit rx_queue_range. See
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
    /// Held as `std::string_view` to keep `BringupConfig` a literal type
    /// (mirrors the public `PlatformConfig`'s constexpr-friendliness;
    /// useful when test fixtures want `static_assert(...)` on a synthesized
    /// bring-up config). Caller owns the backing buffer — typical values
    /// are string literals or long-lived application-owned strings;
    /// don't point at a temporary.
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

    // ── Auto-derived MP layout (internal autojoin path) ─────────────────
    //
    // When set, the library treats this as the source of truth for
    // multi-process resource allocation: the internal bring-up helpers
    // `Platform::primary_bringup_` / `secondary_bringup_` (invoked by
    // `Platform::create_or_join` once the EAL race resolves the role)
    // derive the effective `rx_queue_range` from `mp_topology->self()`,
    // register the topology in a shared hugepage memzone
    // (`detail::MpRegistry`) so cross-process disjointness is enforced
    // rather than trusted, and constrain the stream creators' src_port
    // search range to the self spec's `[port_lo, port_hi)`.
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
    // keeps `BringupConfig` constexpr-constructible.
    std::optional<MpTopology> mp_topology {};

    // NOTE on source-port partitioning across MP processes:
    //
    // `eph-net-dpdk` does NOT auto-allocate source ports. The TCP/UDP
    // `create_and_attach` paths take the source port from the caller-
    // supplied `cfg.dpdk.wire.tuple.src_port` (TCP) /
    // `cfg.dpdk.wire.src_port` (UDP) in Software / FlowDirector
    // mode, or rebind it to one that hashes to the desired queue
    // (RSS-pinned mode via `find_src_port_for_queue`). In a multi-process
    // setup it is
    // therefore the *caller*'s job to ensure that the primary and each
    // secondary draw their source ports from disjoint sub-ranges — the
    // library has no global view to enforce this. See
    // `docs/dpdk-multiprocess.md` for guidance on partitioning.

    /// Defaulted equality — all fields must match exactly.
    [[nodiscard]] friend bool operator==(const BringupConfig&,
                                         const BringupConfig&) = default;

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        std::string base = std::format(
            "BringupConfig:\n"
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
    /// Advisory only — does not block construction.
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
};

} // namespace detail

// ─────────────────────────────────────────────────────────────────────
// Public Config types — daemon-led model (post-reshape)
// ─────────────────────────────────────────────────────────────────────
//
// Two configs replace the old kitchen-sink PlatformConfig +
// CreateOrJoinConfig + EalConfig public surface:
//
//   1. PlatformConfig — application-side, consumed by Platform::create
//      to attach as a DPDK secondary. Just NIC selection (`pci`),
//      resource ask (`queues`), and per-process EAL knobs.
//
//   2. NicServiceConfig — daemon-side, consumed by Platform::serve_nic.
//      All NIC physical state (descriptors, RSS key, mempool, …) lives
//      here. Apps never see it.
//
// EalConfig (eal.hpp) stays as an internal helper type; it is no longer
// passed by callers.

/// @brief Default 40-byte symmetric Toeplitz RSS key.
///
/// `0x6d, 0x5a` repeated — matches the well-known "Toeplitz default"
/// used by most PMDs and recommended by Microsoft for symmetric RX/TX
/// hashing. Ops can override per-NIC via `NicServiceConfig::rss_key`.
inline constexpr std::array<std::uint8_t, 40> kDefaultRssKey{
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
};

/// @brief Application-facing Platform config — what an app passes to
/// `Platform::create` to attach to an already-running NIC daemon.
///
/// Lean by design: the only NIC-related field is the PCI BDF (which
/// also derives the EAL `--file-prefix` deterministically) and the
/// per-app queue ask. Every other physical NIC knob lives on
/// `NicServiceConfig` and is owned by the daemon.
///
/// Internally derived (caller never sets these):
///   * `proc_type = Secondary` — apps are always DPDK secondaries
///   * `file_prefix = "eph_" + sanitize_bdf(pci)`
///   * `allowed_devs = {pci}`
struct PlatformConfig {
    // ── NIC selection ────────────────────────────────────────────────
    /// PCI BDF of the NIC to attach (e.g. `"0000:01:00.1"`). Drives
    /// both the EAL `-a` allowlist AND the auto-derived `file_prefix`.
    /// Empty = resolved at create time from the daemon's
    /// `default = true` toml (S6 wires this in; today the empty path
    /// still requires daemon registry to exist for the derived prefix).
    std::string_view pci{};

    // ── Resource ask ────────────────────────────────────────────────
    /// @brief Bidirectional queue pairs requested. Each queue pair
    /// is one RX + one TX queue, bound 1:1 (the assumption table in
    /// the design plan calls this out — the library does not support
    /// asymmetric RX-heavy / TX-light layouts).
    ///
    /// Pool exhausted ⇒ `Platform::create` returns
    /// `ErrorInfo{QueuePoolExhausted}`. Today (foundation commit) the
    /// queue allocation is a static placeholder: the secondary just
    /// claims queues `0..(queues-1)`. The proper QueueAllocator + RETA
    /// dynamics arrive in S5.
    std::uint16_t queues = 1;

    // ── Per-process EAL knobs (the old `EalConfig` minus the bits
    //    the library auto-derives) ────────────────────────────────────
    /// Typed lcore→cpu pin spec. When non-empty, the library validates
    /// each pin against the process-wide pin registry BEFORE
    /// `rte_eal_init` fires, so a misconfigured topology surfaces as a
    /// loud cold-path error. Mutually exclusive with `lcores`.
    std::span<eph::dpdk::LcorePin const> pins{};

    /// Strictness of the typed-pin validator. Has no effect when
    /// `pins` is empty.
    eph::utils::CpuPinPolicy pin_policy{};

    /// Raw lcore list (one entry per `-l` argument, or one entry using
    /// DPDK list/range syntax like `"0-3"`). Mutually exclusive with
    /// `pins`. The same DPDK footgun that bit `EalConfig::lcores` still
    /// applies — multiple entries collapse to the LAST `-l` value;
    /// prefer the typed `pins` path.
    std::vector<std::string> lcores{};

    /// Extra raw EAL argv tokens (e.g. `--log-level=lib.eal:warning`).
    /// Appended verbatim to the assembled argv after the typed
    /// transformations.
    std::vector<std::string> extra_eal_args{};

    /// EAL `argv[0]`. Useful for log identification when multiple apps
    /// share one host.
    std::string_view program_name{"eph_app"};
};

/// @brief Structural validator for `PlatformConfig`. Empty string_view
/// on success; non-empty rejection reason on failure.
///
/// constexpr-evaluable: use in `static_assert(config_ok(cfg))` for
/// compile-time configs.
[[nodiscard]] constexpr std::string_view
validate(const PlatformConfig& cfg) noexcept {
    if (cfg.queues == 0)
        return "queues must be >= 1";
    if (!cfg.pins.empty() && !cfg.lcores.empty())
        return "pins and lcores are mutually exclusive";
    if (cfg.program_name.empty())
        return "program_name must be non-empty";
    // Empty `pci` is ALLOWED here — resolved at create time against
    // the daemon registry. Validate-only checks that the call shape is
    // structurally sane; live-NIC reachability is a separate runtime
    // concern.
    return {};
}

/// For use in `static_assert` with constexpr configs:
///   constexpr PlatformConfig cfg{ .queues = 4 };
///   static_assert(config_ok(cfg), "bad PlatformConfig");
[[nodiscard]] constexpr bool config_ok(const PlatformConfig& cfg) noexcept {
    return validate(cfg).empty();
}

/// @brief Daemon-facing config — consumed by `Platform::serve_nic` and
/// the `eph-nicd` binary. Carries the NIC physical state ops cares
/// about (descriptors, RSS, mempool, promiscuous mode, …); apps never
/// see this struct.
///
/// `total_queues` sets the queue pool capacity = max number of peer
/// secondaries that can attach simultaneously. RX/TX 1:1 paired (same
/// assumption as the application side).
struct NicServiceConfig {
    /// PCI BDF of the NIC to bring up. Required.
    std::string_view pci{};

    /// @brief Total queue pairs the daemon configures on the NIC.
    /// = pool capacity = upper bound on the sum of `cfg.queues` across
    /// all attached secondaries. Must be >= 1.
    std::uint16_t total_queues = 16;

    /// 40-byte symmetric Toeplitz RSS key. Defaults to
    /// `kDefaultRssKey`. Override per-NIC if the deployment requires a
    /// site-specific key.
    std::array<std::uint8_t, 40> rss_key = kDefaultRssKey;

    /// True = enable promiscuous mode on the port. HFT default false.
    bool          promiscuous     = false;

    /// RX descriptor ring depth per queue. Clamped to NIC limits at
    /// bring-up time.
    std::uint16_t nb_rx_desc      = 1024;

    /// TX descriptor ring depth per queue. Clamped to NIC limits at
    /// bring-up time.
    std::uint16_t nb_tx_desc      = 1024;

    /// Mempool size. Must be `2^n - 1` (e.g. 1023, 4095, 8191).
    std::uint32_t mbuf_pool_size  = 8191;

    /// Per-lcore mempool cache size. Must be < `mbuf_pool_size`.
    std::uint16_t mbuf_cache_size = 256;

    /// EAL lcore the daemon's primary process runs on. Single-lcore
    /// daemon today; future versions may take a list.
    std::uint16_t daemon_lcore = 0;
};

/// @brief Structural validator for `NicServiceConfig`. Empty
/// `string_view` on success; non-empty rejection reason on failure.
[[nodiscard]] constexpr std::string_view
validate(const NicServiceConfig& cfg) noexcept {
    if (cfg.pci.empty())
        return "pci must be non-empty";
    if (cfg.total_queues == 0)
        return "total_queues must be >= 1";
    if (cfg.nb_rx_desc == 0)
        return "nb_rx_desc must be >= 1";
    if (cfg.nb_tx_desc == 0)
        return "nb_tx_desc must be >= 1";
    if (!detail::is_power_of_two_minus_one(cfg.mbuf_pool_size))
        return "mbuf_pool_size must be 2^n - 1 (e.g. 1023, 4095, 8191)";
    if (cfg.mbuf_cache_size >= cfg.mbuf_pool_size)
        return "mbuf_cache_size must be less than mbuf_pool_size";
    return {};
}

[[nodiscard]] constexpr bool config_ok(const NicServiceConfig& cfg) noexcept {
    return validate(cfg).empty();
}

namespace detail {

/// @brief Internal bring-up validator for `BringupConfig`. Used by
/// `primary_bringup_` / `secondary_bringup_` to surface a structural
/// problem before any DPDK syscall fires. Empty string_view on success.
[[nodiscard]] constexpr std::string_view
validate_bringup_(const BringupConfig& cfg) noexcept {
    if (cfg.nb_rx_queues  == 0) return "nb_rx_queues must be > 0";
    if (cfg.nb_tx_queues  == 0) return "nb_tx_queues must be > 0";
    if (cfg.nb_rx_queues > 1 && cfg.nb_tx_queues == 1)
        return "nb_tx_queues must be >= nb_rx_queues when nb_rx_queues > 1 "
               "(RSS-aware connect pins tx_queue_id = rx_queue_id)";
    if (cfg.nb_rx_desc    == 0) return "nb_rx_desc must be > 0";
    if (cfg.nb_tx_desc    == 0) return "nb_tx_desc must be > 0";
    if (cfg.link_timeout_ms < 0) return "link_timeout_ms must be >= 0";
    if (!is_power_of_two_minus_one(cfg.mbuf_pool_size))
        return "mbuf_pool_size must be 2^n - 1 (e.g. 1023, 4095, 8191)";
    if (cfg.mbuf_cache_size >= cfg.mbuf_pool_size)
        return "mbuf_cache_size must be less than mbuf_pool_size";
    // rx_queue_range: either the {0,0} sentinel ("full range") or a
    // non-empty sub-range bounded by nb_rx_queues. Without this check,
    // a half-set range (lo == hi != 0, or lo > hi, or hi >
    // nb_rx_queues) would silently slip through and corrupt
    // round-robin queue selection at create_and_attach time.
    if (cfg.rx_queue_range.first != 0 || cfg.rx_queue_range.second != 0) {
        if (cfg.rx_queue_range.first >= cfg.rx_queue_range.second)
            return "rx_queue_range: lo must be < hi (or use {0,0} sentinel for full range)";
        if (cfg.rx_queue_range.second > cfg.nb_rx_queues)
            return "rx_queue_range.hi must not exceed nb_rx_queues";
    }
    if (cfg.per_lcore_pools > 256)
        return "per_lcore_pools must be <= RTE_MAX_LCORE (256)";
    // mp_topology and rx_queue_range are mutually exclusive: when both
    // are set, they're two sources of truth that could disagree.
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

} // namespace detail

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

    // ─────────────────────────────────────────────────────────────────
    // Public Platform factories (daemon-led surface)
    // ─────────────────────────────────────────────────────────────────

    /// @brief Application-side factory. Attaches to an already-running
    /// `eph-nicd` daemon as a DPDK secondary, claims `cfg.queues`
    /// queues from the pool, and returns a Platform.
    ///
    /// Owns the EAL session for this process (calls `rte_eal_init`
    /// internally with proc_type=secondary and file_prefix derived
    /// deterministically from `cfg.pci`).
    ///
    /// Failure modes (cold path):
    ///   * `validate(cfg)` rejection (bad queues / pin spec / …)
    ///   * No daemon running on this NIC (no primary on the
    ///     derived file_prefix)
    ///   * Pool exhausted (post-S5: returns
    ///     `ErrorInfo{QueuePoolExhausted}`; today the static
    ///     placeholder may surface a different bring-up error)
    [[nodiscard]] static std::expected<Platform, std::string>
    create(PlatformConfig cfg);

    /// @brief Daemon-side factory. Brings the NIC up as DPDK primary
    /// with the physical state described by `cfg`, and returns a
    /// Platform. The intended caller is the `eph-nicd` binary —
    /// applications MUST use `Platform::create`.
    ///
    /// Owns the EAL session for this process. After this call returns,
    /// secondaries may attach via `Platform::create` against the same
    /// `cfg.pci`.
    [[nodiscard]] static std::expected<Platform, std::string>
    serve_nic(NicServiceConfig cfg);

    /// @brief Block until SIGTERM or SIGINT is received. Used by the
    /// daemon binary after `serve_nic` returns to keep the NIC
    /// primary alive while secondaries attach. Returns on signal
    /// receipt; ~Platform handles the actual NIC teardown.
    void join() noexcept;

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

    /// @brief True iff `rte_eth_dev_start` has been called and not yet
    /// undone — the underlying NIC port is currently up and processing
    /// bursts. Cold getter; safe on moved-from instances (returns
    /// false). The flag is set by `primary_bringup_` after a successful
    /// `rte_eth_dev_start` and cleared by `~Impl` on `rte_eth_dev_stop`.
    /// Secondaries inherit the primary's running state without
    /// re-issuing `dev_start` themselves.
    [[nodiscard]] bool         is_running()       const noexcept;
    /// @brief True iff promiscuous mode was requested
    /// (`PlatformConfig::enable_promiscuous == true`) AND
    /// `rte_eth_promiscuous_enable` returned success during bring-up.
    /// The two-step contract avoids reporting "promiscuous" on PMDs
    /// that silently no-op the request. Cold getter; safe on moved-from
    /// instances (returns false).
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

    // ── Auto-derived MP layout (autojoin / mp_topology-driven) ───────────

    /// @brief True iff this Platform participates in an active
    /// multi-process group — typically because it was created via
    /// `Platform::create_or_join` (autojoin) and the registry now has
    /// at least one peer slot, or because the internal `mp_topology`
    /// machinery was driven directly by an internal helper. Cold
    /// getter, safe on moved-from instances (returns false). Returns
    /// false on single-process Platforms produced by
    /// `Platform::create` / `launch`.
    [[nodiscard]] bool is_multi_process() const noexcept;

    /// @brief This process's `[port_lo, port_hi)` src_port window when
    /// the multi-process topology has been resolved (autojoin or the
    /// internal `mp_topology` path); `std::nullopt` otherwise. Stream
    /// `create_and_attach` consults this to constrain
    /// `find_src_port_for_queue`'s search range — letting the library
    /// auto-pick a non-colliding ephemeral src_port instead of asking
    /// the caller to hand-partition src_port ranges across processes.
    /// Cold getter; safe on moved-from instances (returns nullopt).
    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>>
    port_range() const noexcept;

    /// @brief True iff this Platform resolved to the secondary role —
    /// either because `Platform::create_or_join` lost the EAL race
    /// (autojoin path), or because the internal `secondary_bringup_`
    /// helper was invoked directly. Equivalent to
    /// `rte_eal_process_type() == RTE_PROC_SECONDARY` for live
    /// Platforms. Cold getter consumed by `Stream::create_and_attach`
    /// to gate the FlowDir IPC-fallback path: only secondaries hit
    /// `eph_fd_install` after a local `rte_flow_create` rejection.
    /// Returns false on single-process Platforms and on moved-from
    /// instances.
    [[nodiscard]] bool is_secondary() const noexcept;

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

    /// @brief RAII handle returned by `register_icmp_target`.
    /// Wraps the per-process `IcmpRegistry::Handle` (existing weak_ptr-
    /// based unregister) PLUS an optional `IcmpDirectorySlotGuard`
    /// that releases the cross-process directory slot. The directory
    /// guard is only populated when the Platform was created with
    /// `mp_topology` set; single-process Platforms produce a handle
    /// whose directory portion is a no-op default.
    ///
    /// Both portions are move-only; the type inherits move-
    /// only and lets default member dtor do the work — `local_` and
    /// `dir_` each handle their own cleanup independently and in the
    /// declaration order given here (`dir_` last → released first
    /// per reverse-construction order; this drops the cross-proc
    /// slot before the local registry slot, so any in-flight forward
    /// from a peer that observed gen=N now finds gen=N+1 and drops
    /// stale before we tear down the local target).
    ///
    /// `engaged()` reports the local side (the cross-proc slot may
    /// or may not be engaged independently — irrelevant to most
    /// callers).
    class IcmpTargetHandle {
    public:
        IcmpTargetHandle() noexcept = default;

        explicit IcmpTargetHandle(
            ::eph::dpdk::detail::IcmpRegistry::Handle local) noexcept
            : local_(std::move(local)) {}

        IcmpTargetHandle(
            ::eph::dpdk::detail::IcmpRegistry::Handle  local,
            ::eph::dpdk::detail::IcmpDirectorySlotGuard dir) noexcept
            : local_(std::move(local)), dir_(std::move(dir)) {}

        IcmpTargetHandle(const IcmpTargetHandle&)            = delete;
        IcmpTargetHandle& operator=(const IcmpTargetHandle&) = delete;
        IcmpTargetHandle(IcmpTargetHandle&&) noexcept            = default;
        IcmpTargetHandle& operator=(IcmpTargetHandle&&) noexcept = default;
        ~IcmpTargetHandle() noexcept = default;

        [[nodiscard]] bool engaged() const noexcept { return local_.engaged(); }

    private:
        // Order matters: dir_ destructs first (++gen on directory) so
        // any in-flight IPC forward from a peer drops stale at the
        // owner-side dispatch, BEFORE local_ frees the registry slot.
        ::eph::dpdk::detail::IcmpRegistry::Handle   local_{};
        ::eph::dpdk::detail::IcmpDirectorySlotGuard dir_{};
    };

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

    /// @brief Internal: per-port DPDK bring-up. Owns the
    /// enumerate→mempool→configure→queues→start sequence and the RSS
    /// fallback path. Reads `config.proc_type` to pick the primary vs
    /// secondary code path (secondary skips configure/start in favour of
    /// `lookup_mempool_secondary`). `primary_bringup_` /
    /// `secondary_bringup_` synthesize the BringupConfig from caller
    /// input and invoke this method.
    [[nodiscard]] static std::expected<Platform, std::string>
    bringup_port_(const detail::BringupConfig& config);

    /// @brief Internal primary bring-up. Reads the optional
    /// `mp_topology` from the synthesized BringupConfig to reserve the
    /// cross-process registry, derive `rx_queue_range` from `self()`,
    /// reserve the ICMP directory, then delegates to `bringup_port_`
    /// for the per-port DPDK bring-up.
    [[nodiscard]] static std::expected<Platform, std::string>
    primary_bringup_(detail::BringupConfig config);

    /// @brief Internal secondary bring-up. After the cooperative-MP
    /// removal the only entry path is `Platform::create_or_join`'s
    /// secondary branch, which always pre-claims a slot via
    /// `try_claim_free_slot` before dispatch — so `registry_preclaimed`
    /// is always `true` in production. The flag is retained as a
    /// hidden seam for unit tests that want to drive a fresh CAS-claim
    /// path against a synthetic registry.
    [[nodiscard]] static std::expected<Platform, std::string>
    secondary_bringup_(detail::BringupConfig config, bool registry_preclaimed);
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
    /// `Platform::create_or_join` (which dispatches to
    /// `primary_bringup_` / `secondary_bringup_` internally) when the
    /// resolved `BringupConfig` carries an `mp_topology`. Held in
    /// `std::optional` so the
    /// non-MP path leaves it empty without paying the (still small)
    /// cost of a default-constructed handle. **Declared before every
    /// other DPDK-resource-owning field** so it outlives them on
    /// destruction (reverse construction order): mempool, port state
    /// and pollers tear down first; the registry's hugepage memzone
    /// release happens last, when no other process state could still
    /// be reading it.
    std::optional<::eph::dpdk::detail::MpRegistryHandle> mp_registry;

    /// @brief Cross-process ICMP target directory. Populated only
    /// when `cfg.mp_topology` is set (single-process Platforms leave
    /// this empty and ICMP path stays purely intra-process — same
    /// behavior as reshape stage 2 and earlier). Same declaration-
    /// order rationale as `mp_registry`: outlives every other DPDK-
    /// resource-owning field so the directory's hugepage memzone is
    /// released last, after pollers / streams / IPC handlers are
    /// torn down.
    std::optional<::eph::dpdk::detail::IcmpDirectoryHandle> icmp_directory;

    /// @brief RAII handle for the `eph_icmp_dispatch` IPC action that
    /// receives forwarded ICMP Frag Needed messages from peer
    /// secondaries. Registered in `Platform::create_*` if and only if
    /// `mp_topology` is set AND `icmp_directory` came up cleanly.
    /// `bool(action) == false` after a degraded registration is the
    /// signal that ICMP cross-proc forward will fall back to silent
    /// drop (= reshape stage 2 behavior).
    std::optional<::eph::dpdk::detail::MpIpcAction> icmp_dispatch_action;

    /// @brief This process's index within MpTopology. Cached on
    /// `Platform::create_*` so the Poller closure can short-circuit
    /// "I am the owner" after a directory lookup (the look-up race
    /// where directory says self_proc_index but local registry
    /// already moved on). 0xFF = mp_topology not set, ICMP cross-
    /// proc path inactive.
    uint8_t self_proc_index = 0xFF;

    /// @brief Primary-side bookkeeping of rules installed on behalf
    /// of secondaries via the eph_fd_install IPC fallback path.
    /// Populated only on the primary; secondary processes leave it
    /// in default-constructed state (empty).
    ::eph::net::dpdk::detail::RemoteFlowRulesMap remote_flow_rules;

    /// @brief RAII handle for the `eph_fd_install` action. Registered
    /// only on primary (via `primary_bringup_`, the impl_ method that
    /// `Platform::create_or_join` dispatches to when the EAL race
    /// resolves this peer as primary, and only when mp_topology + IPC
    /// handler bring-up both succeed). Secondary never registers this
    /// — it is only the requester side.
    std::optional<::eph::dpdk::detail::MpIpcAction> fd_install_action;

    /// @brief RAII handle for `eph_fd_destroy`. Same primary-only
    /// rule as fd_install_action.
    std::optional<::eph::dpdk::detail::MpIpcAction> fd_destroy_action;

    /// @brief S5: hugepage-backed queue-pair allocator. Populated only
    /// on the daemon (primary) side by `Platform::serve_nic`. Owns the
    /// `eph_qalloc/<file_prefix>` memzone — secondary applications go
    /// through the IPC path (`eph_queue_claim` / `eph_queue_release`)
    /// and never directly touch this object. Declared before any
    /// resource-owning field that the IPC handlers might reference so
    /// that on shutdown the action handlers (declared below) unregister
    /// FIRST and the allocator memzone frees LAST among the S5 group.
    std::optional<::eph::dpdk::detail::QueueAllocator> queue_allocator;

    /// @brief S5: RAII handle for `eph_queue_claim`. Registered only
    /// on the daemon (primary). Application-side `Platform::create`
    /// calls `mp_ipc_request_sync` against this action, the handler
    /// runs the allocator's claim() and replies with a granted range.
    std::optional<::eph::dpdk::detail::MpIpcAction> queue_claim_action;

    /// @brief S5: RAII handle for `eph_queue_release`. Same primary-
    /// only rule. Secondary `~Platform` sends a one-way release via
    /// `mp_ipc_send_oneway`; daemon free's the slot + refreshes RETA.
    std::optional<::eph::dpdk::detail::MpIpcAction> queue_release_action;

    /// @brief S5: secondary-side bookkeeping. Records the queue range
    /// granted by the daemon's QueueAllocator on attach. Used to
    /// drive the `eph_queue_release` IPC on this Platform's
    /// destruction. Default `{0, 0, 0}` sentinel = "no claim active"
    /// (applies to single-process / primary instances; ~Impl skips
    /// the release IPC in that case).
    ::eph::dpdk::detail::QueueRange owned_queues_range{};

    /// @brief Cold-path NIC physical state. Holds every field the
    /// bring-up body reads — port_id, queue counts, descriptor counts,
    /// mbuf pool, RSS / promiscuous flags, file_prefix, per_lcore_pools.
    /// Internally typed as `detail::BringupConfig` so the cluster of
    /// `impl_->config.<field>` accessors keeps working unchanged across
    /// the public-API reshape; the public-facing `PlatformConfig`
    /// (post-reshape lean shape) is no longer a 1:1 mirror of these
    /// fields and would not satisfy the bring-up body's needs on its
    /// own.
    detail::BringupConfig config;

    /// @brief Resolved DPDK process role for THIS Platform instance.
    /// Captured from the role-specific bring-up entry point at create
    /// time and read by `Platform::is_secondary()` and `~Impl`'s
    /// secondary-aware cleanup branch. Default Primary so an
    /// untouched Impl behaves as the dominant single-process case.
    ProcType resolved_proc_type = ProcType::Primary;

    /// @brief Resolved RX-queue range `[lo, hi)` this process owns.
    /// `{0, 0}` is the sentinel meaning "use the full range
    /// `[0, nb_rx_queues)`". MP bring-up paths populate this from the
    /// MpTopology slot they synthesize; single-process create() leaves
    /// it at the sentinel. Read by `Platform::effective_rx_queue_range()`.
    std::pair<uint16_t, uint16_t> resolved_rx_queue_range{0, 0};

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

    /// @brief Typed-pin session guards owned by `Platform::launch`
    /// (and `create_or_join` via delegation). Empty when EAL was init'd
    /// externally — i.e. the caller used the bare `Platform::create`
    /// path and is responsible for managing its own pins via typed
    /// `EalGuard::init`. Released via
    /// reverse-order field destruction AFTER `~Impl`'s explicit body
    /// finishes — pin release only touches the process-wide CPU
    /// registry global, EAL-independent, so timing relative to
    /// `eal_cleanup` is harmless.
    std::vector<eph::utils::PinGuard> pin_session_guards;

    /// @brief Set to `true` when `Platform::launch` (or
    /// `create_or_join`) called `eal_init` itself. `~Impl`'s body reads
    /// this to decide whether to invoke `eal_cleanup` after DPDK
    /// resource teardown. Single Source of Truth: only one Platform
    /// per process can have `owns_eal_init=true`; multiple Platforms
    /// sharing one EAL must use `Platform::create` (which leaves this
    /// flag at false).
    bool owns_eal_init{false};

    /// True iff MP topology is active and at least one peer (other
    /// than self) is still attached. Used as the gate for primary
    /// teardown of shared port / mempool / memzone state. See
    /// eph-net-dpdk/docs/dpdk-mp-teardown-protocol.md for full rationale.
    [[nodiscard]] bool defer_for_peers() const noexcept {
        return mp_registry.has_value() &&
               !mp_registry->is_last_alive_proc();
    }

    ~Impl() {
        // ── S5: secondary-side queue release ────────────────────────────
        // If this Platform was an application-side secondary that
        // claimed queues from the daemon's QueueAllocator, fire-and-
        // forget a release IPC before any cleanup() syscalls touch the
        // EAL state. Best-effort: if the daemon already exited the
        // sendmsg fails silently and the daemon's reset-on-create
        // contract reclaims the slot on the next bring-up.
        if (resolved_proc_type == ProcType::Secondary &&
            !owned_queues_range.empty()) {
            ::eph::dpdk::detail::QueueReleaseRequest req{};
            req.version       = 1;
            req.lo            = owned_queues_range.lo;
            req.hi            = owned_queues_range.hi;
            req.generation    = owned_queues_range.generation;
            req.requester_pid = static_cast<int32_t>(::getpid());
            auto sr = ::eph::dpdk::detail::mp_ipc_send_oneway(
                ::eph::dpdk::detail::kQueueReleaseActionName, req);
            if (!sr) {
                SPDLOG_LOGGER_DEBUG(detail::platform_logger(),
                    "~Impl(secondary): queue release IPC failed: {} — "
                    "daemon may have exited; primary's reset-on-create "
                    "will reclaim the slot on next bring-up.",
                    sr.error().detail);
            } else {
                SPDLOG_LOGGER_DEBUG(detail::platform_logger(),
                    "~Impl(secondary): released queue range=[{},{}) gen={} "
                    "back to daemon", owned_queues_range.lo,
                    owned_queues_range.hi, owned_queues_range.generation);
            }
        }

        // ── S5: daemon-side allocator global handoff ────────────────────
        // Clear the process-level pointer if it still points at us, so
        // any in-flight claim/release thunk that loads the global after
        // this point sees null and replies / drops cleanly. Done before
        // queue_claim_action / queue_release_action's RAII fires
        // rte_mp_action_unregister (which blocks until in-flight
        // handlers return). Same pattern as the FlowDir / ICMP handoff
        // below.
        if (queue_allocator.has_value()) {
            auto* expected_alloc = &*queue_allocator;
            ::eph::dpdk::detail::g_active_queue_allocator.compare_exchange_strong(
                expected_alloc, nullptr, std::memory_order_acq_rel);
            ::eph::dpdk::detail::g_active_qalloc_port_id.store(
                uint16_t{0xFFFF}, std::memory_order_release);
        }

        cleanup();
        // Clear process-level ICMP IPC globals if they still point at
        // us. CAS so a concurrently-created replacement Platform isn't
        // clobbered (the contract is "one Platform per process at a
        // time", but unit tests that destruct/reconstruct in sequence
        // still benefit from explicit handoff). After this point the
        // field destructors run in reverse declaration order:
        // icmp_dispatch_action's RAII fires rte_mp_action_unregister
        // (which blocks until any in-flight handler finishes), then
        // icmp_directory's RAII frees the memzone.
        if (icmp_directory.has_value()) {
            auto* expected_dir = &*icmp_directory;
            ::eph::dpdk::detail::g_active_icmp_directory.compare_exchange_strong(
                expected_dir, nullptr, std::memory_order_acq_rel);
        }
        if (auto* reg = icmp_registry_sp.get()) {
            auto* expected_reg = reg;
            ::eph::dpdk::detail::g_active_icmp_registry.compare_exchange_strong(
                expected_reg, nullptr, std::memory_order_acq_rel);
        }
        if (self_proc_index != 0xFF) {
            uint8_t expected_idx = self_proc_index;
            ::eph::dpdk::detail::g_active_self_proc_index.compare_exchange_strong(
                expected_idx, uint8_t{0xFF}, std::memory_order_acq_rel);
        }
        // FlowDir IPC handoff: clear the primary-only global pointing
        // at our `remote_flow_rules`, then destroy any rules still
        // tracked (a secondary that died after install but before its
        // RAII destroy IPC arrived would otherwise leak NIC state).
        // Order: clear global first → fd_install/destroy_action's
        // RAII (declared after this destructor body — fires next on
        // reverse declaration order) is rte_mp_action_unregister,
        // which blocks until any in-flight handler returns; we want
        // those handlers to see global=null and reply error rather
        // than touch `remote_flow_rules` mid-cleanup.
        {
            auto* expected_rules = &remote_flow_rules;
            ::eph::net::dpdk::detail::g_active_remote_flow_rules
                .compare_exchange_strong(
                    expected_rules, nullptr, std::memory_order_acq_rel);
        }
        remote_flow_rules.destroy_all();

        // MP teardown gate — icmp_directory has no peer-aware structure
        // of its own (mp_registry handles its memzone via release_()).
        // Must run BEFORE icmp_directory's field destructor fires.
        if (defer_for_peers() && icmp_directory.has_value()) {
            icmp_directory->disable_memzone_free();
        }

        // EAL ownership: `~Impl` must NOT call `eal_cleanup` — field
        // destruction in reverse declaration order continues AFTER
        // this body, and `mp_registry` / `icmp_directory` destructors
        // (declared earliest, destroyed latest) touch hugepage
        // memzones which `rte_eal_cleanup` would have already torn
        // down. The actual `eal_cleanup` call lives in `~Platform`,
        // which runs AFTER `impl_.reset()` finishes destroying every
        // Impl field — see the explicit `Platform::~Platform()` body.
    }

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
        if (resolved_proc_type == ProcType::Secondary) {
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

        // PRIMARY teardown — DPDK MP teardown gate.
        // Full rationale (why eph defers stop/close/free when peers
        // are still attached): see eph-net-dpdk/docs/dpdk-mp-teardown-protocol.md.
        // Single-process path (`mp_registry` empty) short-circuits to
        // false and falls through to the original stop/close/free below
        // — byte-equal to pre-fix behavior.
        if (defer_for_peers()) {
            const uint32_t alive = mp_registry->count_alive_procs();
            SPDLOG_LOGGER_INFO(log,
                "primary cleanup: {} peers still attached — deferring "
                "rte_eth_dev_stop/close + mempool_free per DPDK MP "
                "teardown protocol. port stays running; physical "
                "teardown happens when the last process exits.",
                alive - 1);
            // Local bookkeeping — don't touch shared port / mempool state.
            // Per-lcore pool view aliases primary-owned objects that are
            // still in use by attached peers; just zero our local handles.
            port_started = false;
            mempool = nullptr;
            for (auto& slot : per_lcore_pool) slot = nullptr;
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

inline Platform::~Platform() {
    // Explicit teardown order: destroy Impl fully FIRST (~Impl body
    // + reverse-order field destructors release every DPDK resource),
    // THEN run eal_cleanup. Implicit destructor would invert this and
    // SEGV in mp_registry's memzone access after rte_eal_cleanup.
    //
    // MP teardown gate (paired with Impl::cleanup() gate): when peers
    // are still attached, defer rte_eal_cleanup too — it would close
    // active devices and dangle peers' shared io_cq state. See
    // eph-net-dpdk/docs/dpdk-mp-teardown-protocol.md for full rationale.
    if (impl_) {
        const bool owns_eal = impl_->owns_eal_init;
        // Snapshot the gate BEFORE impl_.reset(): mp_registry's dtor
        // will clear self slot during reset, so the post-reset query
        // would be meaningless.
        const bool defer_eal_for_peers = impl_->defer_for_peers();
        impl_.reset();   // triggers ~Impl → all DPDK resources gone
        if (owns_eal) {
            [[maybe_unused]] auto log = detail::platform_logger();
            if (defer_eal_for_peers) {
                SPDLOG_LOGGER_INFO(log,
                    "~Platform: deferring rte_eal_cleanup — peers still "
                    "attached. OS releases per-process hugepage mappings "
                    "on exit; shared state stays alive for peers.");
            } else {
                SPDLOG_LOGGER_DEBUG(log,
                    "~Platform: owns_eal_init=true — running eal_cleanup");
                [[maybe_unused]] bool ok = ::eph::dpdk::eal_cleanup();
            }
        }
    }
}

inline Platform::Platform(Platform&&) noexcept            = default;
inline Platform& Platform::operator=(Platform&&) noexcept = default;

// ─────────────────────────────────────────────────────────────────────────────
// Platform::launch — one-shot EAL+Platform factory
// ─────────────────────────────────────────────────────────────────────────────
//
// Cold-path orchestrator over typed-pin validation + eal_init +
// Platform::create. The returned Platform owns the EAL session: pin
// guards live in `Impl::pin_session_guards`, the `owns_eal_init` flag
// directs `~Impl` to call `eal_cleanup` after DPDK resource release.
//
// Failure rollback is staged so each phase only undoes what it set up:
//
//   pin_lcores fails    → no eal_init touched, return unexpected
//   eal_init fails      → pin_guards local-destruct rolls back CPU
//                          registry, return unexpected
//   Platform::create    → eal_cleanup() to undo eal_init, then
//   fails                  pin_guards local-destruct, return unexpected
//
// On success, pin_guards transfer into Impl and owns_eal_init is set.

[[nodiscard]] inline std::expected<Platform, std::string>
Platform::bringup_port_(const detail::BringupConfig& config) {
    [[maybe_unused]] auto log = detail::platform_logger();

    if (auto err = detail::validate_bringup_(config); !err.empty()) {
        SPDLOG_LOGGER_ERROR(log, "Invalid BringupConfig: {}", err);
        return std::unexpected(std::string{err});
    }

    // Surface non-fatal misconfigurations (undersized rings, promiscuous mode,
    // zero link-timeout, etc.) at WARN so operators see them in production
    // logs. Advisory only — does not block construction.
    for (const auto& w : config.warnings()) {
        SPDLOG_LOGGER_WARN(log, "BringupConfig advisory: {}", w);
    }

    auto impl = std::make_unique<Impl>();
    // Project the v2 config into v3 + resolved fields. The v2-only
    // identity fields (`proc_type`, `rx_queue_range`) move to dedicated
    // `Impl::resolved_*` members; `mp_topology` is consumed by the
    // role-specific entry points BEFORE they call us, so by the time we
    // land here it is always reset.
    impl->config.port_id                    = config.port_id;
    impl->config.file_prefix                = config.file_prefix;
    impl->config.nb_rx_queues               = config.nb_rx_queues;
    impl->config.nb_tx_queues               = config.nb_tx_queues;
    impl->config.nb_rx_desc                 = config.nb_rx_desc;
    impl->config.nb_tx_desc                 = config.nb_tx_desc;
    impl->config.mbuf_pool_size             = config.mbuf_pool_size;
    impl->config.mbuf_cache_size            = config.mbuf_cache_size;
    impl->config.enable_promiscuous         = config.enable_promiscuous;
    impl->config.enable_rx_checksum_offload = config.enable_rx_checksum_offload;
    impl->config.enable_strict_rx_checksum  = config.enable_strict_rx_checksum;
    impl->config.link_timeout_ms            = config.link_timeout_ms;
    impl->config.per_lcore_pools            = config.per_lcore_pools;
    // v3-only fields stay at default (single-process / auto). The v2
    // dispatcher does not synthesize MP topology — that happens in
    // `primary_bringup_` / `secondary_bringup_` (dispatched from
    // `Platform::create_or_join` once the EAL race resolves the role),
    // and the resolved proc_type / rx_queue_range below capture the
    // outcome.
    impl->resolved_proc_type        = config.proc_type;
    impl->resolved_rx_queue_range   = config.rx_queue_range;

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
Platform::primary_bringup_(detail::BringupConfig config) {
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
    // `is_multi_process()` and `port_range()` consult it for the
    // stream-attach src_port narrowing path.
    std::optional<::eph::dpdk::detail::MpRegistryHandle> reg;
    std::optional<::eph::dpdk::detail::IcmpDirectoryHandle> icmp_dir;
    uint8_t captured_self_idx = 0xFF;   // mp_topology not set
    if (config.mp_topology.has_value()) {
        if (auto err = detail::validate_bringup_(config); !err.empty()) {
            SPDLOG_LOGGER_ERROR(log,
                "primary_bringup_: invalid BringupConfig: {}", err);
            return std::unexpected(std::string{err});
        }
        if (config.file_prefix.empty()) {
            SPDLOG_LOGGER_ERROR(log,
                "primary_bringup_: mp_topology requires a non-empty "
                "file_prefix (used as the registry memzone name "
                "eph_mp/<file_prefix>)");
            return std::unexpected(std::string{
                "primary_bringup_: mp_topology set but file_prefix is empty"});
        }
        auto r = ::eph::dpdk::detail::MpRegistryHandle::create_primary(
            config.file_prefix, *config.mp_topology);
        if (!r) {
            SPDLOG_LOGGER_ERROR(log,
                "primary_bringup_: registry reserve failed: {}",
                r.error().detail);
            return std::unexpected(std::string{r.error().detail});
        }
        reg = std::move(*r);
        const auto& self = config.mp_topology->self();
        config.rx_queue_range = {self.queue_lo, self.queue_hi};
        captured_self_idx = config.mp_topology->self_index;

        // ICMP cross-process directory (degrade-on-failure).
        // Reserve a parallel hugepage memzone `eph_mp_icmp/<prefix>`
        // so secondary processes that receive a Frag Needed for an
        // owner-elsewhere stream can look up the owner_proc and
        // forward via IPC. Failure here only degrades cross-proc
        // ICMP forwarding — Platform creation continues so the rest
        // of the eph stack remains usable.
        auto icmp_r = ::eph::dpdk::detail::IcmpDirectoryHandle::create_primary(
            config.file_prefix);
        if (icmp_r) {
            icmp_dir = std::move(*icmp_r);
        } else {
            SPDLOG_LOGGER_ERROR(log,
                "primary_bringup_: IcmpDirectory reserve failed: {} "
                "— ICMP cross-process forwarding degraded to per-process drop",
                icmp_r.error().detail);
        }

        // Consumed: clear so create()'s validate_config doesn't trip
        // the mp_topology⇄rx_queue_range mutual-exclusion check.
        config.mp_topology.reset();

        SPDLOG_LOGGER_INFO(log,
            "primary_bringup_: mp_topology derived "
            "rx_queue_range=[{},{}) self_index={} (registry attached, "
            "icmp_directory={})",
            config.rx_queue_range.first, config.rx_queue_range.second,
            captured_self_idx,
            icmp_dir.has_value() ? "ready" : "degraded");
    }

    auto p = bringup_port_(config);
    if (!p) return p;  // reg's RAII frees the memzone on early-out

    if (reg.has_value() && p->impl_) {
        p->impl_->mp_registry      = std::move(reg);
        p->impl_->self_proc_index  = captured_self_idx;
        if (icmp_dir.has_value()) {
            p->impl_->icmp_directory = std::move(icmp_dir);
            // Wire up process-level globals BEFORE installing the IPC
            // handler — otherwise an in-flight forward from a peer
            // could race into a thunk that loads nullptr globals.
            ::eph::dpdk::detail::g_active_icmp_directory.store(
                &*p->impl_->icmp_directory, std::memory_order_release);
            ::eph::dpdk::detail::g_active_icmp_registry.store(
                p->impl_->icmp_registry_sp.get(), std::memory_order_release);
            ::eph::dpdk::detail::g_active_self_proc_index.store(
                captured_self_idx, std::memory_order_release);

            p->impl_->icmp_dispatch_action.emplace(
                ::eph::dpdk::detail::kIcmpDispatchActionName,
                &::eph::dpdk::detail::on_icmp_dispatch_thunk);
            // bool(icmp_dispatch_action) == false is a SPDLOG_ERROR
            // path inside MpIpcAction; we leave globals set anyway so
            // any LATER-arriving secondary that registers successfully
            // can still send to us if our registration eventually
            // succeeds via a future Platform reload.
        }

        // FlowDir secondary-fallback IPC handlers — primary only.
        // Wire the global pointing at this Platform's
        // RemoteFlowRulesMap so the static thunks can find it,
        // then register the two `eph_fd_install` / `eph_fd_destroy`
        // actions. Same degrade-on-failure semantics as ICMP.
        ::eph::net::dpdk::detail::g_active_remote_flow_rules.store(
            &p->impl_->remote_flow_rules, std::memory_order_release);
        p->impl_->fd_install_action.emplace(
            ::eph::net::dpdk::kFdInstallActionName,
            &::eph::net::dpdk::detail::on_fd_install_thunk);
        p->impl_->fd_destroy_action.emplace(
            ::eph::net::dpdk::kFdDestroyActionName,
            &::eph::net::dpdk::detail::on_fd_destroy_thunk);
    }
    return p;
}

[[nodiscard]] inline std::expected<Platform, std::string>
Platform::secondary_bringup_(detail::BringupConfig config,
                             bool registry_preclaimed) {
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
    // `docs/dpdk-multiprocess.md`.

    if (auto err = detail::validate_bringup_(config); !err.empty()) {
        SPDLOG_LOGGER_ERROR(log,
            "secondary_bringup_: invalid BringupConfig: {}", err);
        return std::unexpected(std::string{err});
    }

    if (config.file_prefix.empty()) {
        SPDLOG_LOGGER_ERROR(log,
            "secondary_bringup_: file_prefix is empty "
            "(must match the primary's EAL --file-prefix)");
        return std::unexpected(std::string{
            "secondary_bringup_: file_prefix must be non-empty and match "
            "the primary's EAL --file-prefix (otherwise rte_mempool_lookup "
            "cannot find the primary's shared mempool)"});
    }

    // ── mp_topology attach (cold path) ──────────────────────────────────
    //
    // Mirror of primary_bringup_'s auto-derivation, but in lookup mode:
    // the primary already wrote the registry header; we attach, cross-
    // validate that our declared spec matches the primary's view of
    // this self_index, derive cfg.rx_queue_range from `self()`, and
    // clear `cfg.mp_topology` so the rest of secondary bring-up sees a
    // "manual partition" config. The registry handle is moved into the
    // Impl after the rest of secondary attach succeeds; the handle's
    // RAII destructor releases the slot on early-out.
    std::optional<::eph::dpdk::detail::MpRegistryHandle> reg;
    std::optional<::eph::dpdk::detail::IcmpDirectoryHandle> icmp_dir;
    uint8_t captured_self_idx = 0xFF;
    if (config.mp_topology.has_value()) {
        auto r = ::eph::dpdk::detail::MpRegistryHandle::attach_secondary(
            config.file_prefix, *config.mp_topology, registry_preclaimed);
        if (!r) {
            SPDLOG_LOGGER_ERROR(log,
                "secondary_bringup_{}: registry attach failed: {}",
                registry_preclaimed ? " (preclaimed)" : "",
                r.error().detail);
            return std::unexpected(std::string{r.error().detail});
        }
        reg = std::move(*r);
        const auto& self = config.mp_topology->self();
        config.rx_queue_range = {self.queue_lo, self.queue_hi};
        captured_self_idx = config.mp_topology->self_index;

        // Cross-process ICMP directory attach (degrade-on-failure).
        // The primary should have reserved the memzone; if not, drop
        // back to per-process ICMP.
        auto icmp_r = ::eph::dpdk::detail::IcmpDirectoryHandle::attach_secondary(
            config.file_prefix);
        if (icmp_r) {
            icmp_dir = std::move(*icmp_r);
        } else {
            SPDLOG_LOGGER_ERROR(log,
                "secondary_bringup_: IcmpDirectory attach failed: {} "
                "— ICMP cross-process forwarding degraded to per-process drop",
                icmp_r.error().detail);
        }

        config.mp_topology.reset();
        SPDLOG_LOGGER_INFO(log,
            "secondary_bringup_: mp_topology derived "
            "rx_queue_range=[{},{}) self_index={} (registry attached, "
            "icmp_directory={})",
            config.rx_queue_range.first, config.rx_queue_range.second,
            captured_self_idx,
            icmp_dir.has_value() ? "ready" : "degraded");
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
    // keep parity with the primary path's advisory output.
    for (const auto& w : config.warnings()) {
        SPDLOG_LOGGER_WARN(log, "BringupConfig advisory: {}", w);
    }

    if (!rte_eth_dev_is_valid_port(config.port_id)) {
        SPDLOG_LOGGER_ERROR(log,
            "secondary_bringup_: port_id={} not valid from "
            "secondary — primary may not have started this port yet, "
            "or file_prefix may not match",
            config.port_id);
        return std::unexpected(std::format(
            "secondary_bringup_: port_id {} not visible from secondary "
            "(is the primary running with the same --file-prefix, "
            "and did it start this port?)",
            config.port_id));
    }

    auto impl = std::make_unique<Impl>();
    // Same v2-into-v3 projection as Platform::create — see that body
    // for the per-field rationale.
    impl->config.port_id                    = config.port_id;
    impl->config.file_prefix                = config.file_prefix;
    impl->config.nb_rx_queues               = config.nb_rx_queues;
    impl->config.nb_tx_queues               = config.nb_tx_queues;
    impl->config.nb_rx_desc                 = config.nb_rx_desc;
    impl->config.nb_tx_desc                 = config.nb_tx_desc;
    impl->config.mbuf_pool_size             = config.mbuf_pool_size;
    impl->config.mbuf_cache_size            = config.mbuf_cache_size;
    impl->config.enable_promiscuous         = config.enable_promiscuous;
    impl->config.enable_rx_checksum_offload = config.enable_rx_checksum_offload;
    impl->config.enable_strict_rx_checksum  = config.enable_strict_rx_checksum;
    impl->config.link_timeout_ms            = config.link_timeout_ms;
    impl->config.per_lcore_pools            = config.per_lcore_pools;
    impl->resolved_proc_type      = config.proc_type;          // Secondary
    impl->resolved_rx_queue_range = config.rx_queue_range;     // from MpTopology slot

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
                    "secondary_bringup_: caller nb_rx_queues={} exceeds live "
                    "max_rx_queues={} on port {} — rr_counter may hand out "
                    "queue ids the NIC has no rings for; check that the "
                    "secondary's BringupConfig matches the primary's",
                    config.nb_rx_queues, dev_info.max_rx_queues,
                    config.port_id);
            }
        } else {
            SPDLOG_LOGGER_DEBUG(log,
                "secondary_bringup_: rte_eth_dev_info_get probe rejected "
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
        impl->mp_registry     = std::move(reg);
        impl->self_proc_index = captured_self_idx;
        if (icmp_dir.has_value()) {
            impl->icmp_directory = std::move(icmp_dir);
            // Wire process-level globals before installing the IPC
            // handler — same race-free ordering as primary_bringup_.
            ::eph::dpdk::detail::g_active_icmp_directory.store(
                &*impl->icmp_directory, std::memory_order_release);
            ::eph::dpdk::detail::g_active_icmp_registry.store(
                impl->icmp_registry_sp.get(), std::memory_order_release);
            ::eph::dpdk::detail::g_active_self_proc_index.store(
                captured_self_idx, std::memory_order_release);

            impl->icmp_dispatch_action.emplace(
                ::eph::dpdk::detail::kIcmpDispatchActionName,
                &::eph::dpdk::detail::on_icmp_dispatch_thunk);
        }
    }

    SPDLOG_LOGGER_INFO(log,
        "secondary_bringup_ ready (port={}, file_prefix='{}', "
        "rx_queue_range=[{},{}), dispatch_mode={})",
        config.port_id, config.file_prefix,
        config.rx_queue_range.first, config.rx_queue_range.second,
        ::eph::net::dpdk::rx_dispatch_mode_name(impl->dispatch_mode));

    return Platform(std::move(impl));
}

// ─────────────────────────────────────────────────────────────────────
// Public entry-point implementations (daemon-led model)
// ─────────────────────────────────────────────────────────────────────
//
// Two factories:
//
//   * `Platform::create(PlatformConfig)` — application secondary-attach.
//     Derives file_prefix from cfg.pci, runs eal_init with
//     proc_type=Secondary + allowed_devs={pci}, then secondary_bringup_.
//     Today the queue claim is a static placeholder — the secondary
//     just claims queues `0..(cfg.queues-1)`. S5 replaces this with the
//     QueueAllocator + RETA-tracking IPC protocol.
//
//   * `Platform::serve_nic(NicServiceConfig)` — daemon primary entry.
//     eal_init with proc_type=Primary, then primary_bringup_ with an
//     `MpTopology::uniform` covering `cfg.total_queues` slots so
//     secondaries can attach. Owns the EAL session
//     (`Impl::owns_eal_init = true`).

namespace detail {

/// @brief Internal helper: lower a `PlatformConfig` (lean app-side
/// shape) into a `BringupConfig` for the secondary path. The caller
/// (`Platform::create`) supplies the file_prefix it derived.
[[nodiscard]] inline BringupConfig
bringup_from_platform_(const PlatformConfig& cfg,
                       std::string_view      file_prefix) {
    BringupConfig bcfg{};
    bcfg.proc_type   = ProcType::Secondary;
    bcfg.file_prefix = file_prefix;
    // S5 lifts the placeholder: cfg.queues drives a real claim against
    // the daemon's QueueAllocator, and the daemon hands back the actual
    // queue range. Today we project naively into a uniform topology so
    // the existing secondary_bringup_ machinery still works.
    bcfg.port_id        = 0;            // single-port today
    bcfg.nb_rx_queues   = cfg.queues;
    bcfg.nb_tx_queues   = cfg.queues;
    // Secondary doesn't reconfigure descriptor counts / mbuf pool —
    // those are primary-owned. Leave the defaults; the bring-up body
    // queries the live NIC anyway.
    return bcfg;
}

/// @brief Internal helper: lower a `NicServiceConfig` (daemon-side
/// shape) into a `BringupConfig` for the primary path.
[[nodiscard]] inline BringupConfig
bringup_from_nic_service_(const NicServiceConfig& cfg,
                          std::string_view        file_prefix) {
    BringupConfig bcfg{};
    bcfg.proc_type           = ProcType::Primary;
    bcfg.file_prefix         = file_prefix;
    bcfg.port_id             = 0;       // single-port today
    bcfg.nb_rx_queues        = cfg.total_queues;
    bcfg.nb_tx_queues        = cfg.total_queues;
    bcfg.nb_rx_desc          = cfg.nb_rx_desc;
    bcfg.nb_tx_desc          = cfg.nb_tx_desc;
    bcfg.mbuf_pool_size      = cfg.mbuf_pool_size;
    bcfg.mbuf_cache_size     = cfg.mbuf_cache_size;
    bcfg.enable_promiscuous  = cfg.promiscuous;
    // Always reserve enough registry slots so any future peer fits.
    // Daemon takes slot 0 in this placeholder; S5 refines so the daemon
    // takes no queue at all.
    bcfg.mp_topology = MpTopology::uniform(
        /*self_index=*/0,
        /*total_procs=*/cfg.total_queues,
        /*nb_rx_queues=*/cfg.total_queues);
    return bcfg;
}

/// @brief Internal: derive `"eph_" + sanitize(pci)` from a BDF, or
/// surface the bdf-sanitize error. Helper shared by both factories.
[[nodiscard]] inline std::expected<std::string, std::string>
derive_file_prefix_(std::string_view pci) {
    auto san = ::eph::dpdk::detail::sanitize_bdf_for_file_prefix(pci);
    if (!san)
        return std::unexpected(std::format(
            "cannot derive file_prefix from pci='{}': {}",
            pci, san.error().detail));
    return std::string{"eph_"} + *san;
}

} // namespace detail

[[nodiscard]] inline std::expected<Platform, std::string>
Platform::create(PlatformConfig cfg) {
    [[maybe_unused]] auto* log = detail::platform_logger();

    if (auto err = ::eph::dpdk::validate(cfg); !err.empty()) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::create: invalid PlatformConfig: {}", err);
        return std::unexpected(std::string{err});
    }
    if (cfg.pci.empty()) {
        // S6 will resolve "default = true" toml automatically; today
        // the empty path is a hard error so users see a clear signal.
        SPDLOG_LOGGER_ERROR(log,
            "Platform::create: pci is empty — automatic default-NIC "
            "selection from /etc/eph/*.toml is not yet wired (S6); "
            "supply cfg.pci explicitly for now");
        return std::unexpected(std::string{
            "Platform::create: pci must be non-empty (default-NIC "
            "selection from toml is not yet wired)"});
    }

    auto fp_r = detail::derive_file_prefix_(cfg.pci);
    if (!fp_r) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::create: {}", fp_r.error());
        return std::unexpected(std::move(fp_r.error()));
    }
    const std::string derived_prefix = std::move(*fp_r);
    SPDLOG_LOGGER_INFO(log,
        "Platform::create: derived file_prefix='{}' from pci='{}', "
        "queues={}", derived_prefix, cfg.pci, cfg.queues);

    // ── 1. Pin lcores (typed path) ──────────────────────────────────
    if (!cfg.pins.empty() && !cfg.lcores.empty()) {
        return std::unexpected(std::string{
            "Platform::create: pins and lcores are mutually exclusive"});
    }
    std::vector<eph::utils::PinGuard> pin_guards;
    if (!cfg.pins.empty()) {
        auto pg = pin_lcores(cfg.pins, cfg.pin_policy);
        if (!pg) {
            SPDLOG_LOGGER_ERROR(log,
                "Platform::create: pin_lcores rejected: {}", pg.error());
            return std::unexpected(std::format(
                "Platform::create: pin_lcores: {}", pg.error()));
        }
        pin_guards = std::move(*pg);
    }

    // ── 2. Assemble EalConfig (Secondary, library-derived) ──────────
    EalConfig eal_cfg{};
    eal_cfg.program_name  = std::string{cfg.program_name};
    eal_cfg.proc_type     = ProcType::Secondary;
    eal_cfg.proc_type_set = true;
    eal_cfg.file_prefix   = derived_prefix;
    eal_cfg.allowed_devs  = {std::string{cfg.pci}};
    eal_cfg.lcores        = cfg.lcores;
    eal_cfg.extra_args    = cfg.extra_eal_args;
    if (!cfg.pins.empty()) {
        eal_cfg.extra_args.push_back(build_lcore_argv(cfg.pins));
    }

    auto argv_owned = build_eal_argv(eal_cfg);
    std::vector<char*> argv;
    argv.reserve(argv_owned.size());
    for (auto& s : argv_owned) argv.push_back(s.data());

    auto eal_r = eal_init(static_cast<int>(argv.size()), argv.data());
    if (!eal_r) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::create: eal_init failed: {} — pin_guards roll "
            "back automatically on return", eal_r.error());
        return std::unexpected(std::format(
            "Platform::create: eal_init failed: {}", eal_r.error()));
    }

    // ── 3. S5: claim queue range via daemon IPC ─────────────────────
    //
    // Replaces the pre-S5 static placeholder (which claimed
    // `0..cfg.queues-1` blindly). Sends `eph_queue_claim` to the
    // daemon's QueueAllocator and reads the granted `[lo, hi)` range
    // back. Failure modes:
    //   * Daemon not running on this NIC → mp_ipc_request_sync times
    //     out; surface as the user-visible "daemon IPC failed" error.
    //   * Pool exhausted → reply.ok == 0 with reply.error ==
    //     "QueuePoolExhausted"; surface verbatim.
    //   * Wire mismatch (cross-version IPC) → InvalidConfig from
    //     parse_payload; surface as a structural bring-up error.
    ::eph::dpdk::detail::QueueClaimRequest claim_req{};
    claim_req.version       = 1;
    claim_req.count         = cfg.queues;
    claim_req.requester_pid = static_cast<int32_t>(::getpid());

    auto reply_r = ::eph::dpdk::detail::mp_ipc_request_sync<
        ::eph::dpdk::detail::QueueClaimRequest,
        ::eph::dpdk::detail::QueueClaimReply>(
        ::eph::dpdk::detail::kQueueClaimActionName,
        claim_req,
        std::chrono::milliseconds{2000});
    if (!reply_r) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::create: queue claim IPC failed: {} — is the "
            "eph-nicd daemon running on pci='{}'? (file_prefix='{}')",
            reply_r.error().detail, cfg.pci, derived_prefix);
        [[maybe_unused]] bool ok = eal_cleanup();
        return std::unexpected(std::format(
            "Platform::create: queue claim IPC failed: {}",
            reply_r.error().detail));
    }
    if (!reply_r->ok) {
        // Daemon-formatted error; "QueuePoolExhausted" is the canonical
        // pool-empty reply.
        SPDLOG_LOGGER_ERROR(log,
            "Platform::create: daemon rejected claim for {} queue(s): {}",
            cfg.queues, reply_r->error);
        [[maybe_unused]] bool ok = eal_cleanup();
        return std::unexpected(std::format(
            "Platform::create: claim rejected: {}",
            reply_r->error));
    }
    const ::eph::dpdk::detail::QueueRange granted{
        reply_r->lo, reply_r->hi, reply_r->generation};
    SPDLOG_LOGGER_INFO(log,
        "Platform::create: daemon granted queue range=[{},{}) gen={} "
        "for pci='{}' (requested count={})",
        granted.lo, granted.hi, granted.generation, cfg.pci, cfg.queues);

    // ── 4. Bring up secondary with the daemon-granted range ─────────
    detail::BringupConfig bcfg = detail::bringup_from_platform_(
        cfg, derived_prefix);
    // Steer the secondary's RR queue selection at the granted range
    // (instead of "[0, cfg.queues)" placeholder) so two concurrent
    // secondaries with non-zero offsets each operate on their own
    // queues.
    bcfg.rx_queue_range = std::pair<uint16_t, uint16_t>{granted.lo, granted.hi};
    // The bring-up validator requires `rx_queue_range.hi <= nb_rx_queues`.
    // Bump nb_rx_queues to accommodate the upper bound — the secondary
    // doesn't actually configure rings (primary owns that), the field
    // is only used to bound the round-robin selector.
    if (bcfg.nb_rx_queues < granted.hi) {
        bcfg.nb_rx_queues = granted.hi;
        bcfg.nb_tx_queues = granted.hi;
    }

    auto plat_r = Platform::secondary_bringup_(
        std::move(bcfg), /*registry_preclaimed=*/false);
    if (!plat_r) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::create: secondary_bringup_ failed: {} — "
            "rolling back EAL + releasing claim", plat_r.error());
        // Best-effort release IPC: roll back the claim we just got so
        // the daemon's pool isn't leaked on bring-up failure.
        ::eph::dpdk::detail::QueueReleaseRequest rel{};
        rel.version       = 1;
        rel.lo            = granted.lo;
        rel.hi            = granted.hi;
        rel.generation    = granted.generation;
        rel.requester_pid = static_cast<int32_t>(::getpid());
        (void)::eph::dpdk::detail::mp_ipc_send_oneway(
            ::eph::dpdk::detail::kQueueReleaseActionName, rel);
        [[maybe_unused]] bool ok = eal_cleanup();
        return std::unexpected(std::format(
            "Platform::create: {}", plat_r.error()));
    }

    // ── 5. Transfer EAL ownership and record granted range ──────────
    Platform plat = std::move(*plat_r);
    if (plat.impl_) {
        plat.impl_->pin_session_guards = std::move(pin_guards);
        plat.impl_->owns_eal_init      = true;
        plat.impl_->owned_queues_range = granted;
    }
    SPDLOG_LOGGER_INFO(log,
        "Platform::create: ready (pci='{}', file_prefix='{}', "
        "queues=[{},{}) gen={})",
        cfg.pci, derived_prefix, granted.lo, granted.hi, granted.generation);
    return plat;
}

[[nodiscard]] inline std::expected<Platform, std::string>
Platform::serve_nic(NicServiceConfig cfg) {
    [[maybe_unused]] auto* log = detail::platform_logger();

    if (auto err = ::eph::dpdk::validate(cfg); !err.empty()) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::serve_nic: invalid NicServiceConfig: {}", err);
        return std::unexpected(std::string{err});
    }

    auto fp_r = detail::derive_file_prefix_(cfg.pci);
    if (!fp_r) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::serve_nic: {}", fp_r.error());
        return std::unexpected(std::move(fp_r.error()));
    }
    const std::string derived_prefix = std::move(*fp_r);
    SPDLOG_LOGGER_INFO(log,
        "Platform::serve_nic: derived file_prefix='{}' from pci='{}', "
        "total_queues={}, daemon_lcore={}",
        derived_prefix, cfg.pci, cfg.total_queues, cfg.daemon_lcore);

    // ── 1. Assemble EalConfig (Primary, library-owned) ──────────────
    EalConfig eal_cfg{};
    eal_cfg.program_name  = "eph_nicd";
    eal_cfg.proc_type     = ProcType::Primary;
    eal_cfg.proc_type_set = true;
    eal_cfg.file_prefix   = derived_prefix;
    eal_cfg.allowed_devs  = {std::string{cfg.pci}};
    // Single-lcore daemon today; future iterations may take a list.
    eal_cfg.lcores        = {std::format("{}", cfg.daemon_lcore)};

    auto argv_owned = build_eal_argv(eal_cfg);
    std::vector<char*> argv;
    argv.reserve(argv_owned.size());
    for (auto& s : argv_owned) argv.push_back(s.data());

    auto eal_r = eal_init(static_cast<int>(argv.size()), argv.data());
    if (!eal_r) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::serve_nic: eal_init failed: {}", eal_r.error());
        return std::unexpected(std::format(
            "Platform::serve_nic: eal_init failed: {}", eal_r.error()));
    }

    // ── 2. Bring up primary ─────────────────────────────────────────
    detail::BringupConfig bcfg =
        detail::bringup_from_nic_service_(cfg, derived_prefix);

    auto plat_r = Platform::primary_bringup_(std::move(bcfg));
    if (!plat_r) {
        SPDLOG_LOGGER_ERROR(log,
            "Platform::serve_nic: primary_bringup_ failed: {} — "
            "rolling back EAL", plat_r.error());
        [[maybe_unused]] bool ok = eal_cleanup();
        return std::unexpected(std::format(
            "Platform::serve_nic: {}", plat_r.error()));
    }

    Platform plat = std::move(*plat_r);
    if (plat.impl_) {
        plat.impl_->owns_eal_init = true;
    }

    // ── 3. S5: bring up the QueueAllocator + IPC handlers ───────────
    //
    // The allocator owns a hugepage memzone keyed by `derived_prefix`
    // and exposes `eph_queue_claim` / `eph_queue_release` IPC actions
    // that secondary applications drive from `Platform::create`.
    //
    // Failure modes are degrade-on-failure: a memzone reservation
    // failure aborts (we can't run without an allocator), but action
    // registration failures (e.g. `--no-shconf` builds) leave
    // `bool(action) == false` and secondaries surface a clean
    // "daemon IPC unavailable" error on `Platform::create`.
    if (plat.impl_) {
        auto alloc_r = ::eph::dpdk::detail::QueueAllocator::create_primary(
            derived_prefix, cfg.total_queues);
        if (!alloc_r) {
            SPDLOG_LOGGER_ERROR(log,
                "Platform::serve_nic: QueueAllocator::create_primary failed: "
                "{} — rolling back EAL", alloc_r.error());
            // plat goes out of scope; ~Platform tears down primary_bringup_'s
            // Impl AND fires eal_cleanup via owns_eal_init.
            return std::unexpected(std::format(
                "Platform::serve_nic: {}", alloc_r.error()));
        }
        plat.impl_->queue_allocator = std::move(*alloc_r);

        // Wire the process-level globals BEFORE installing the IPC
        // handlers so a freshly-arrived claim msg never observes a
        // null allocator. Same race-free ordering as ICMP / FlowDir.
        ::eph::dpdk::detail::g_active_queue_allocator.store(
            &*plat.impl_->queue_allocator, std::memory_order_release);
        ::eph::dpdk::detail::g_active_qalloc_port_id.store(
            plat.impl_->config.port_id, std::memory_order_release);

        plat.impl_->queue_claim_action.emplace(
            ::eph::dpdk::detail::kQueueClaimActionName,
            &::eph::dpdk::detail::on_queue_claim_thunk);
        plat.impl_->queue_release_action.emplace(
            ::eph::dpdk::detail::kQueueReleaseActionName,
            &::eph::dpdk::detail::on_queue_release_thunk);

        // Initial RETA: empty claimed-set → all buckets point at queue 0
        // ("sink queue" — daemon doesn't poll it). Best-effort; on PMDs
        // that reject runtime RETA updates the bring-up RETA stays in
        // place and the contract becomes "RETA is configured once and
        // any in-flight peer queues stay stable" — still correct.
        (void)::eph::dpdk::detail::refresh_reta_for_claimed_(
            *plat.impl_->queue_allocator, plat.impl_->config.port_id);

        SPDLOG_LOGGER_INFO(log,
            "Platform::serve_nic: QueueAllocator ready (total_queues={}, "
            "claim_action={}, release_action={})",
            cfg.total_queues,
            bool(*plat.impl_->queue_claim_action) ? "registered" : "DEGRADED",
            bool(*plat.impl_->queue_release_action) ? "registered" : "DEGRADED");
    }

    SPDLOG_LOGGER_INFO(log,
        "Platform::serve_nic: ready (pci='{}', file_prefix='{}', "
        "total_queues={})",
        cfg.pci, derived_prefix, cfg.total_queues);
    return plat;
}

inline void Platform::join() noexcept {
    [[maybe_unused]] auto* log = detail::platform_logger();
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
    int sig = 0;
    sigwait(&set, &sig);
    SPDLOG_LOGGER_INFO(log,
        "Platform::join: signal {} received, returning for graceful "
        "shutdown", sig);
    // S5/S6 will add cross-process notify (rte_mp_sendmsg "I'm leaving")
    // here. For now, just return — ~Platform handles rte_eth_dev_stop /
    // rte_eal_cleanup.
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
    const auto& r = impl_->resolved_rx_queue_range;
    // Sentinel {0, 0} means "use full range [0, nb_rx_queues)".
    // MP bring-up paths populate this from the MpTopology slot they
    // synthesize; single-process create() leaves it at the sentinel.
    if (r.first == 0 && r.second == 0)
        return {uint16_t{0}, impl_->config.nb_rx_queues};
    return r;
}

inline bool Platform::is_multi_process() const noexcept {
    return impl_ && impl_->mp_registry.has_value();
}

inline std::optional<std::pair<uint32_t, uint32_t>>
Platform::port_range() const noexcept {
    if (!impl_ || !impl_->mp_registry.has_value()) return std::nullopt;
    const auto& slot = impl_->mp_registry->self();
    return std::pair{slot.port_lo, slot.port_hi};
}

inline bool Platform::is_secondary() const noexcept {
    return impl_ && impl_->resolved_proc_type == ProcType::Secondary;
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
    [[maybe_unused]] auto log = detail::platform_logger();
    if (!impl_ || !impl_->icmp_registry_sp) {
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "Platform::register_icmp_target: Platform is moved-from"});
    }
    // Local register first — if this fails (registry full, duplicate
    // tuple, null stream/cb), bail before touching the cross-proc
    // directory.
    auto local = impl_->icmp_registry_sp->register_target(tuple, proto, stream, cb);
    if (!local) return std::unexpected(local.error());

    // mp_topology not set, or icmp_directory degraded → return local-
    // only compound handle. Behavior matches reshape stage 2.
    if (!impl_->icmp_directory.has_value() ||
        impl_->self_proc_index == 0xFF) {
        return IcmpTargetHandle{std::move(*local)};
    }

    // Dual-register: tell the cross-proc directory we own this tuple.
    // On failure, log + fall back to local-only — losing cross-proc
    // forwarding isn't worth aborting the stream attach.
    auto slot = impl_->icmp_directory->register_target(
        tuple, proto, impl_->self_proc_index);
    if (!slot) {
        SPDLOG_LOGGER_WARN(log,
            "register_icmp_target: cross-proc directory register failed: "
            "{} — falling back to per-process ICMP only",
            slot.error().detail);
        return IcmpTargetHandle{std::move(*local)};
    }

    ::eph::dpdk::detail::IcmpDirectorySlotGuard dir_guard(
        &*impl_->icmp_directory, *slot);
    return IcmpTargetHandle{std::move(*local), std::move(dir_guard)};
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

/// @brief std::formatter for the internal BringupConfig — compact one-line summary.
template <>
struct std::formatter<eph::dpdk::detail::BringupConfig> : std::formatter<std::string> {
    auto format(const eph::dpdk::detail::BringupConfig& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("BringupConfig(port={}, q={}rx/{}tx, desc={}rx/{}tx, "
                        "pool={}, promisc={})",
                c.port_id, c.nb_rx_queues, c.nb_tx_queues,
                c.nb_rx_desc, c.nb_tx_desc, c.mbuf_pool_size,
                c.enable_promiscuous ? "true" : "false"),
            ctx);
    }
};
