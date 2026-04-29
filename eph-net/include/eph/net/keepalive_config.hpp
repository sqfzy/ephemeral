#pragma once

/// @file keepalive_config.hpp
/// @brief Common TCP keepalive configuration shared by Kernel and DPDK
///        stream backends.
///
/// Extracted from the DPDK-only `cfg.legacy.keepalive_interval /
/// keepalive_probes` pair (`eph::dpdk::TcpConfig` low-level wire fields)
/// and surfaced at the public `StreamConfig` level. The kernel backend
/// also honours these via `setsockopt(TCP_KEEPIDLE / TCP_KEEPINTVL /
/// TCP_KEEPCNT)` — previously the kernel surface had no keepalive knob.
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

    /// @brief Validate the config. `noexcept` + allocation-free.
    ///
    /// @return `{}` on success. `ErrorInfo{InvalidConfig, "..."}` when:
    ///   * `probes` is 0 or > 10 while `interval > 0`.
    ///
    /// Disabled (default-constructed) is always valid — it asks the
    /// backend to skip keepalive entirely. The lower bound on `probes`
    /// prevents the "interval set, probes left at 0" foot-gun where the
    /// kernel would treat the connection as immediately dead.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    validate() const noexcept {
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
