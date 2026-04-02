#pragma once

/// @file reconnect_policy.hpp
/// Reconnection strategy with exponential backoff and jitter.
///
/// Extracted from Transport as an independent, testable component.
/// Owns backoff state and attempt counting; does NOT own TCP/TLS
/// connection logic — that stays in TransportCore.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/transport/transport_types.hpp"
#include "eph/transport/detail/message_types.hpp"

namespace eph::net {

/// Reconnection strategy: exponential backoff with ±25% jitter.
///
/// Usage:
///   ReconnectPolicy policy(config);
///   while (need_reconnect) {
///       if (policy.attempt([&] { return core.do_connect(); })) break;
///       if (policy.exhausted()) { /* give up */ break; }
///   }
///   policy.reset();  // after successful connect
class ReconnectPolicy {
public:
    explicit ReconnectPolicy(const TransportConfig& config) noexcept
        : config_(config)
        , current_backoff_(config.reconnect_interval)
        , max_backoff_(config.max_reconnect_backoff.count() > 0
                           ? config.max_reconnect_backoff
                           : config.reconnect_interval * 16)
    {}

    /// Execute one reconnection attempt with backoff.
    ///
    /// 1. Sleeps for current_backoff_ (with jitter)
    /// 2. Calls connect_fn
    /// 3. On success: increments total_reconnects_, returns true
    /// 4. On failure: increments attempt_, doubles backoff, calls
    ///    on_reconnect_attempt callback, returns false
    ///
    /// @param connect_fn  Callable returning expected<void, ConnectionErrorInfo>
    /// @return true if reconnection succeeded
    [[nodiscard]] bool attempt(
        std::function<std::expected<void, ConnectionErrorInfo>()> connect_fn) noexcept
    {
        auto log = detail::transport_logger();
        ++attempt_;

        // Exponential backoff with ±25% jitter
        auto jittered = apply_jitter(current_backoff_);
        SPDLOG_LOGGER_INFO(log,
            "Reconnect attempt {}/{}: backoff {}ms (jittered from {}ms)",
            attempt_, config_.max_reconnect_attempts,
            jittered.count(), current_backoff_.count());

        std::this_thread::sleep_for(jittered);

        // Attempt connection
        auto result = connect_fn();
        if (result) {
            ++total_reconnects_;
            SPDLOG_LOGGER_INFO(log,
                "Reconnect succeeded on attempt {} (total: {})",
                attempt_, total_reconnects_);
            return true;
        }

        SPDLOG_LOGGER_WARN(log,
            "Reconnect attempt {} failed: {}",
            attempt_, result.error().message());

        // Notify callback — can abort further attempts by returning false
        if (config_.on_reconnect_attempt) {
            try {
                bool should_continue = config_.on_reconnect_attempt(
                    attempt_, config_.max_reconnect_attempts,
                    result.error().message());
                if (!should_continue) {
                    SPDLOG_LOGGER_INFO(log,
                        "Reconnect aborted by on_reconnect_attempt callback");
                    attempt_ = config_.max_reconnect_attempts; // force exhausted
                    return false;
                }
            } catch (const std::exception& e) {
                SPDLOG_LOGGER_ERROR(log,
                    "on_reconnect_attempt callback threw: {}", e.what());
            }
        }

        // Double backoff for next attempt (capped at resolved max)
        current_backoff_ = std::min(current_backoff_ * 2, max_backoff_);

        return false;
    }

    /// Check if max attempts exhausted.
    [[nodiscard]] bool exhausted() const noexcept {
        return attempt_ >= config_.max_reconnect_attempts;
    }

    /// Reset state after successful connection.
    void reset() noexcept {
        attempt_ = 0;
        current_backoff_ = config_.reconnect_interval;
    }

    /// Current attempt count (since last reset).
    [[nodiscard]] int attempts() const noexcept { return attempt_; }

    /// Total successful reconnections over transport lifetime.
    [[nodiscard]] uint64_t total_reconnects() const noexcept {
        return total_reconnects_;
    }

private:
    /// Apply ±25% jitter to a duration.
    static std::chrono::milliseconds apply_jitter(
        std::chrono::milliseconds base) noexcept
    {
        if (base.count() <= 0) return base;
        // Thread-local RNG to avoid contention
        thread_local std::mt19937 rng(std::random_device{}());
        auto lo = base * 75 / 100;
        auto hi = base * 125 / 100;
        std::uniform_int_distribution<int64_t> dist(lo.count(), hi.count());
        return std::chrono::milliseconds{dist(rng)};
    }

    const TransportConfig& config_;
    int attempt_{0};
    uint64_t total_reconnects_{0};
    std::chrono::milliseconds current_backoff_;
    /// Resolved max backoff: config value if > 0, otherwise 16x base interval.
    /// Pre-computed at construction to avoid repeated zero-check in attempt().
    std::chrono::milliseconds max_backoff_;
};

} // namespace eph::net
