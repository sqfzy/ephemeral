#pragma once

/// @file detail/platform_config.hpp
/// Public Config types + their constexpr validators, extracted from
/// `eph/dpdk/platform.hpp` (T2.1, partial split — 2026-05-05).
///
/// What this header carries:
///   - `detail::is_power_of_two_minus_one()` — DPDK mempool size constraint
///   - `detail::next_valid_pool_size()`     — round up to next valid pool
///   - `kDefaultRssKey`                     — symmetric Toeplitz default
///   - `PlatformConfig`                     — application-side / tenant config
///   - `validate(PlatformConfig)` + `config_ok(PlatformConfig)`
///   - `NicServiceConfig`                   — daemon-side / NIC physical state
///   - `validate(NicServiceConfig)` + `config_ok(NicServiceConfig)`
///
/// Why this is split out: `platform.hpp` was 3515 lines. The config
/// section is self-contained (no dependency on the Platform class
/// itself or DPDK PMD types — only standard library + small in-house
/// types like `LcorePin` / `CpuPinPolicy`), so extracting it here lets
/// readers grok the user-facing config surface without scrolling past
/// 3000 lines of bring-up / runtime / IPC code.
///
/// What stayed behind in `platform.hpp`:
///   - `detail::clamp_desc(...)` — uses `rte_eth_desc_lim` from
///     `<rte_ethdev.h>`; tighter coupling to DPDK PMD.
///   - `detail::platform_logger()` — namespaced under detail along with
///     the other internal helpers in platform.hpp.
///   - `detail::validate(BringupConfig)` — references internal
///     `BringupConfig` defined later in platform.hpp.
///
/// Re-included by `platform.hpp`, so existing callers see no API change.

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eph/dpdk/lcore_pin.hpp"   // LcorePin
#include "eph/utils/cpu.hpp"        // CpuPinPolicy

namespace eph::dpdk {

namespace detail {

/// @brief Check if n is of the form (2^k - 1) for some k >= 1.
///
/// DPDK mempool sizes must satisfy this constraint for optimal bucket
/// hashing. Rejects 0 (which is technically 2^0-1 but not a valid pool
/// size).
///
/// @param n  Value to test
/// @return true if n == 2^k - 1 for some k >= 1
[[nodiscard]] constexpr bool is_power_of_two_minus_one(uint32_t n) noexcept {
    if (n == 0) return false;
    uint32_t m = n + 1;
    return m > 0 && (m & (m - 1)) == 0;
}

/// @brief Compute the next valid DPDK mempool size >= n satisfying
///        the 2^k-1 constraint.
///
/// @param n  Minimum desired pool size
/// @return Smallest value >= n of the form 2^k - 1 (k >= 1, so never 0).
///         `n == 0` promotes to 1 (= 2^1 - 1), matching the "k >= 1"
///         invariant that `is_power_of_two_minus_one` enforces.
[[nodiscard]] constexpr uint32_t next_valid_pool_size(uint32_t n) noexcept {
    if (n == 0) return 1;
    uint32_t m = n + 1;
    if (m == 0) return UINT32_MAX;
    if ((m & (m - 1)) == 0) return m - 1;
    return (1u << (32 - std::countl_zero(m))) - 1u;
}

}  // namespace detail

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
    /// Empty = resolved at create time by scanning the DPDK runtime
    /// directory (`/var/run/dpdk/eph_*/eph-pci.txt`) for the unique
    /// running `eph-nicd` daemon — single-NIC hosts get a zero-config
    /// `Platform::create({})` call site. Multi-NIC hosts get a typed
    /// error listing all candidate BDFs and must specify `.pci`
    /// explicitly. See `detail/default_nic_scan.hpp` for the resolver.
    std::string_view pci{};

    // ── Resource ask ────────────────────────────────────────────────
    /// @brief Bidirectional queue pairs requested. Each queue pair
    /// is one RX + one TX queue, bound 1:1 (the assumption table in
    /// the design plan calls this out — the library does not support
    /// asymmetric RX-heavy / TX-light layouts).
    ///
    /// Pool exhausted ⇒ `Platform::create` returns
    /// `ErrorInfo{QueuePoolExhausted}` (the daemon's `QueueAllocator`
    /// rejects the claim; the application sees the typed error and
    /// can either retry with a smaller `queues` ask or surface the
    /// capacity exhaustion to the operator).
    ///
    /// Allocation: secondaries send an `eph_queue_claim` IPC request
    /// to the daemon, which carves a contiguous `[lo, hi)` half-open
    /// range out of its hugepage-backed bitmap (S5 QueueAllocator,
    /// landed 2026-04-30). Release on Platform destruction is fire-
    /// and-forget via `eph_queue_release`. ABA-safe: each claim
    /// increments a per-slot generation; stale releases from prior
    /// incarnations are silently ignored.
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

    /// @brief T2.3: enable HMAC-SHA256 entry tags on the cross-process
    /// MpRegistry. Default `false` to preserve byte-for-byte single-
    /// tenant behaviour.
    ///
    /// When `true`:
    ///   - Daemon generates a 32-byte CSPRNG key at `serve_nic` time and
    ///     writes it to `/run/eph/<sanitize_bdf(pci)>.key` (mode 0440,
    ///     root:root by default — operators can chown to a dedicated
    ///     `eph` group via the systemd unit's `ExecStartPost=`).
    ///   - Daemon sets `header.hmac_enabled=1` and signs every
    ///     `ProcSlot` write with HMAC-SHA256 over the slot's data field.
    ///   - Tenants on attach read the header's `hmac_enabled` bit; if
    ///     set, they read the key file and verify every cross-process
    ///     read. A tamper or missing key file surfaces as
    ///     `Error::InvalidConfig` with a "registry-hmac-mismatch"
    ///     diagnostic.
    ///
    /// Threat model: protects against a compromised secondary tampering
    /// with another secondary's slot (or accidental wild-pointer writes
    /// landing in the shared hugepage segment). Single-LP single-tenant
    /// deployments do not need this — every secondary is in the same
    /// trust domain. Multi-LP / multi-strategy deployments SHOULD enable
    /// it.
    ///
    /// Performance: cold path only (registry read/write happens at
    /// attach + on rare slot-claim events). Hot path (rx/tx burst) is
    /// not affected.
    bool          enable_registry_hmac = false;
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

}  // namespace eph::dpdk
