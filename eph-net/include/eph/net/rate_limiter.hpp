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

#include <spdlog/spdlog.h>

namespace eph::net {

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
        SPDLOG_DEBUG("RateLimiter created: rate={:.2f}/s burst={} rate_per_ns={:.12f}",
                     rate_per_sec, burst, rate_per_ns_);
    }

    /// Try to acquire @p n tokens without blocking.
    /// @return true if tokens were consumed, false if insufficient tokens.
    [[nodiscard]] bool try_acquire(std::size_t n = 1) noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        refill_locked();

        const auto need = static_cast<double>(n);
        if (tokens_ >= need) {
            tokens_ -= need;
            SPDLOG_TRACE("try_acquire({}) succeeded, tokens_remaining={:.2f}", n, tokens_);
            return true;
        }

        SPDLOG_TRACE("try_acquire({}) failed, tokens_available={:.2f}", n, tokens_);
        return false;
    }

    /// Acquire @p n tokens, blocking (via yield) until available.
    ///
    /// Intended for convenience paths where callers prefer blocking over
    /// retry logic. Uses yield() to avoid busy-spinning.
    void acquire(std::size_t n = 1) noexcept {
        SPDLOG_DEBUG("acquire({}) blocking until tokens available", n);
        while (!try_acquire(n)) {
            std::this_thread::yield();
        }
        SPDLOG_DEBUG("acquire({}) completed", n);
    }

    /// Approximate number of currently available tokens.
    ///
    /// Performs a refill before reading — the value is approximate because
    /// another thread may consume tokens between the read and the caller
    /// acting on the result.
    [[nodiscard]] double available() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        const_cast<RateLimiter*>(this)->refill_locked();
        return tokens_;
    }

    /// Reset token count to full burst capacity.
    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        tokens_ = burst_;
        last_refill_ = std::chrono::steady_clock::now();
        SPDLOG_DEBUG("RateLimiter reset to burst={:.0f}", burst_);
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
