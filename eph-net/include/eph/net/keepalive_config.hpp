#pragma once

/// @file keepalive_config.hpp
/// @brief Common TCP keepalive configuration shared by Kernel and DPDK
///        stream backends.
///
/// Extracted from the DPDK-only `cfg.dpdk.tcp_low_level.keepalive_interval
/// / keepalive_probes` pair (`eph::dpdk::TcpConfig` low-level wire fields)
/// and surfaced at the public `StreamConfig` level. The kernel backend
/// also honours these via `setsockopt(TCP_KEEPIDLE / TCP_KEEPINTVL /
/// TCP_KEEPCNT)` — previously the kernel surface had no keepalive knob.
///
/// Note: the underlying low-level field path was renamed from `cfg.legacy.
/// keepalive_*` to `cfg.dpdk.tcp_low_level.keepalive_*` in the T3.19 reshape;
/// this header pre-dates the rename and the comment now reflects the new
/// path so future readers don't grep for the obsolete `legacy` prefix.
///
/// `interval == 0` is the disabled sentinel. Per the project-wide
/// post-v3.3 convention, breaking-change defaults stay opt-in: nothing
/// fires unless the caller explicitly sets a non-zero interval.

#include <chrono>
#include <cstdint>
#include <expected>

#include "eph/core/error.hpp"

namespace eph::net {

/// @brief Configuration for TCP keepalive probes.
///
/// `interval` controls both the idle-before-first-probe time and the
/// inter-probe gap, mirroring the DPDK PMD's TCP session machinery
/// where the same value drives both timers. `probes` bounds the number
/// of unanswered probes before the connection is declared dead.
struct KeepaliveConfig {
    /// @brief Time between keepalive probes (and idle-before-first-probe).
    ///        0 = disabled (no keepalive). Typical value: 30s for HFT
    ///        venues that disconnect after ~60s of silence.
    std::chrono::milliseconds interval{};

    /// @brief Maximum unanswered probes before declaring the peer dead.
    ///        Must be in [1, 10] when `interval > 0`.
    uint8_t probes{3};

    /// @brief True iff keepalive is disabled (interval == 0).
    [[nodiscard]] bool empty() const noexcept {
        return interval == std::chrono::milliseconds::zero();
    }

    /// @brief Hard upper bound on `interval`: 1 day (86 400 000 ms).
    ///
    /// Rationale: the kernel backend lowers `interval` to seconds via
    /// `chrono::ceil<seconds>(interval).count()` and feeds the result
    /// into a `setsockopt(TCP_KEEPIDLE / TCP_KEEPINTVL, int)` call —
    /// values larger than `INT_MAX` seconds would silently truncate
    /// in the static_cast<int> on the way to setsockopt. The DPDK
    /// backend converts the same `chrono::milliseconds` into a TSC
    /// cycle delta inside `tick_keepalive`; on a 3 GHz host
    /// `chrono::milliseconds::max()` overflows uint64_t cycles by a
    /// factor of ~3e9 and wraps to a tiny delta, firing the probe
    /// every poll cycle. A keepalive interval longer than a day is
    /// also unconditionally a config bug (every HFT venue we know
    /// of disconnects after ≤120 s of silence), so reject at the
    /// boundary rather than producing a corrupted downstream value.
    static constexpr std::chrono::milliseconds kMaxInterval{
        std::chrono::hours{24}};

    /// @brief Validate the config. `noexcept` + allocation-free.
    ///
    /// @return `{}` on success. `ErrorInfo{InvalidConfig, "..."}` when:
    ///   * `interval` is negative — the chrono type permits negative
    ///     values but neither backend (kernel `setsockopt(TCP_KEEPIDLE)`
    ///     nor DPDK `tick_keepalive`) has defined behaviour for them;
    ///     reject at the config boundary so callers don't discover it
    ///     as a wrapped TSC delta or an EINVAL setsockopt months later.
    ///   * `interval` exceeds `kMaxInterval` (1 day) — silently truncates
    ///     when lowered to `int` seconds (kernel) or wraps the TSC-cycle
    ///     conversion (DPDK); see `kMaxInterval` for the full rationale.
    ///   * `probes` is 0 or > 10 while `interval > 0`.
    ///
    /// Disabled (default-constructed) is always valid — it asks the
    /// backend to skip keepalive entirely. The lower bound on `probes`
    /// prevents the "interval set, probes left at 0" foot-gun where the
    /// kernel would treat the connection as immediately dead.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    validate() const noexcept {
        if (interval < std::chrono::milliseconds::zero()) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "KeepaliveConfig: interval must be >= 0 (use 0 to disable)"});
        }
        if (interval > kMaxInterval) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "KeepaliveConfig: interval exceeds 1-day upper bound "
                "(would silently truncate to int seconds for setsockopt "
                "or wrap the DPDK TSC-cycle conversion)"});
        }
        if (empty()) {
            return {}; // disabled = always valid
        }
        if (probes == 0 || probes > 10) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "KeepaliveConfig: probes must be in [1, 10] when "
                "interval is non-zero"});
        }
        return {};
    }
};

} // namespace eph::net
