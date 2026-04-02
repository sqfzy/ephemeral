#pragma once

/// @file rate_limiter.hpp
/// Token bucket rate limiter for exchange API request throttling.
///
/// Exchanges enforce strict rate limits (e.g., Binance 1200 req/min) and will
/// IP-ban clients that exceed them. This rate limiter sits in front of send()
/// to ensure outbound requests stay within configured limits.
///
/// Algorithm: classic token bucket with configurable rate and burst capacity.
/// Tokens accumulate at a steady rate up to the burst cap. Each request
/// consumes one or more tokens. When tokens are exhausted, requests are either
/// rejected (try_acquire) or blocked (acquire) until tokens refill.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace eph::net {

namespace detail {
/// @brief Lazily-initialized logger for the RateLimiter subsystem.
/// @return Pointer to the "net.rate_limiter" spdlog logger.
inline spdlog::logger* rate_limiter_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("net.rate_limiter");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("net.rate_limiter");
        }
    }();
    return l.get();
}
} // namespace detail

/// Thread-safe token bucket rate limiter.
///
/// Designed for controlling outbound request rates to exchange APIs.
/// Uses steady_clock for portable sub-microsecond timing — sufficient
/// precision for rate limiting (not on the hot data path).
class RateLimiter {
public:
    /// @param rate_per_sec  Sustained rate (tokens/second). Must be >= 0.
    ///                      A rate of 0 means no tokens are ever generated
    ///                      (only the initial burst is available).
    /// @param burst         Maximum burst size (tokens). Must be >= 1.
    explicit RateLimiter(double rate_per_sec, std::size_t burst) noexcept
        : rate_per_ns_{rate_per_sec / 1'000'000'000.0}
        , burst_{static_cast<double>(burst)}
        , tokens_{static_cast<double>(burst)}
        , last_refill_{std::chrono::steady_clock::now()}
    {
        SPDLOG_LOGGER_DEBUG(detail::rate_limiter_logger(),
                     "RateLimiter created: rate={:.2f}/s burst={} rate_per_ns={:.12f}",
                     rate_per_sec, burst, rate_per_ns_);
    }

    /// @brief Try to acquire @p n tokens without blocking.
    /// @param n  Number of tokens to consume (default 1).
    /// @return true if tokens were consumed, false if insufficient tokens.
    [[nodiscard]] bool try_acquire(std::size_t n = 1) noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        refill_locked();

        const auto need = static_cast<double>(n);
        if (tokens_ >= need) {
            tokens_ -= need;
            SPDLOG_LOGGER_TRACE(detail::rate_limiter_logger(),
                         "try_acquire({}) succeeded, tokens_remaining={:.2f}", n, tokens_);
            return true;
        }

        SPDLOG_LOGGER_TRACE(detail::rate_limiter_logger(),
                     "try_acquire({}) failed, tokens_available={:.2f}", n, tokens_);
        return false;
    }

    /// @brief Acquire @p n tokens, blocking (via yield) until available.
    ///
    /// Intended for convenience paths where callers prefer blocking over
    /// retry logic. Uses yield() to avoid busy-spinning.
    ///
    /// @param n  Number of tokens to consume (default 1).
    /// @warning This method spins with std::this_thread::yield(). Do not call
    ///          on hot paths -- prefer try_acquire() with explicit backoff.
    void acquire(std::size_t n = 1) noexcept {
        SPDLOG_LOGGER_DEBUG(detail::rate_limiter_logger(),
                     "acquire({}) blocking until tokens available", n);
        while (!try_acquire(n)) {
            std::this_thread::yield();
        }
        SPDLOG_LOGGER_DEBUG(detail::rate_limiter_logger(), "acquire({}) completed", n);
    }

    /// @brief Approximate number of currently available tokens.
    ///
    /// Performs a refill before reading -- the value is approximate because
    /// another thread may consume tokens between the read and the caller
    /// acting on the result. Non-const because refill mutates internal state.
    ///
    /// @return Fractional token count (>= 0.0, <= burst).
    [[nodiscard]] double available() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        refill_locked();
        return tokens_;
    }

    /// @brief Reset token count to full burst capacity.
    ///
    /// Refill timestamp is also reset, so the next refill interval starts now.
    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        tokens_ = burst_;
        last_refill_ = std::chrono::steady_clock::now();
        SPDLOG_LOGGER_DEBUG(detail::rate_limiter_logger(), "RateLimiter reset to burst={:.0f}", burst_);
    }

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Refill tokens based on elapsed time since last refill.
    /// MUST be called with mu_ held.
    void refill_locked() noexcept {
        const auto now = Clock::now();
        const auto elapsed_ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_refill_).count());

        if (elapsed_ns > 0.0) {
            const double new_tokens = elapsed_ns * rate_per_ns_;
            tokens_ = std::min(burst_, tokens_ + new_tokens);
            last_refill_ = now;
        }
    }

    double    rate_per_ns_;   ///< Tokens generated per nanosecond
    double    burst_;         ///< Maximum token capacity
    double    tokens_;        ///< Current available tokens (fractional for smooth accumulation)
    TimePoint last_refill_;   ///< Timestamp of last token refill

    mutable std::mutex mu_;   ///< Protects all mutable state
};

} // namespace eph::net
