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
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace eph::net {

namespace detail {
/// @brief Lazily-initialized logger for the RateLimiter subsystem.
/// @return Pointer to the "net.rate_limiter" spdlog logger.
inline spdlog::logger* rate_limiter_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.rate_limiter");
        if (!lg) lg = spdlog::stdout_color_mt("net.rate_limiter");
        return lg;
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
    /// @brief Configuration for a token bucket rate limiter.
    ///
    /// Mirrors the constructor parameters for declarative configuration
    /// and pre-construction validation, consistent with CircuitBreaker::Config
    /// and TransportConfig.
    struct Config {
        double      rate_per_sec = 0.0;  ///< Sustained rate (tokens/second). 0 = no refill.
        std::size_t burst        = 1;    ///< Maximum burst capacity (tokens). Must be >= 1.

        /// @brief Validate configuration, returning an error description or
        ///        empty string on success.
        [[nodiscard]] constexpr std::string_view validate() const noexcept {
            if (rate_per_sec < 0.0)
                return "rate_per_sec must be >= 0";
            if (burst == 0)
                return "burst must be >= 1";
            return {};
        }

        /// Multi-line formatted dump for logging/debugging.
        [[nodiscard]] std::string dump() const {
            return std::format(
                "RateLimiter::Config:\n"
                "  rate: {:.2f}/s, burst: {}",
                rate_per_sec, burst);
        }

        /// JSON-formatted config for monitoring system integration.
        [[nodiscard]] std::string to_json() const {
            return std::format(
                "{{\"rate_per_sec\":{:.2f},\"burst\":{}}}",
                rate_per_sec, burst);
        }

        /// Defaulted equality -- all fields must match exactly.
        [[nodiscard]] friend bool operator==(const Config&,
                                             const Config&) = default;

        /// Check for non-fatal contradictions or likely misconfigurations.
        /// Returns a list of warning messages (empty if no issues).
        /// Unlike validate() which blocks construction, these are advisory.
        [[nodiscard]] std::vector<std::string> warnings() const {
            std::vector<std::string> w;
            if (rate_per_sec > 1'000'000'000.0)
                w.emplace_back(std::format(
                    "rate_per_sec={:.0f} exceeds 1 billion — verify this is "
                    "intentional (sub-nanosecond token generation)", rate_per_sec));
            if (burst > 1'000'000)
                w.emplace_back(std::format(
                    "burst={} is very large — may allow excessive request "
                    "spikes to the exchange", burst));
            if (rate_per_sec == 0.0 && burst > 0)
                w.emplace_back("rate_per_sec=0 with non-zero burst — tokens will "
                               "never refill after initial burst is consumed");
            return w;
        }
    };

    /// @brief Construct a token bucket rate limiter from a Config.
    /// @param config  Rate limiter configuration.
    explicit RateLimiter(Config config) noexcept
        : config_{config}
        , rate_per_ns_{config.rate_per_sec / 1'000'000'000.0}
        , burst_{static_cast<double>(config.burst)}
        , tokens_{static_cast<double>(config.burst)}
        , last_refill_{std::chrono::steady_clock::now()}
    {
        SPDLOG_LOGGER_DEBUG(detail::rate_limiter_logger(),
                     "RateLimiter created: rate={:.2f}/s burst={} rate_per_ns={:.12f}",
                     config.rate_per_sec, config.burst, rate_per_ns_);
    }

    /// @brief Construct a token bucket rate limiter.
    /// @param rate_per_sec  Sustained rate (tokens/second). Must be >= 0.
    ///                      A rate of 0 means no tokens are ever generated
    ///                      (only the initial burst is available).
    /// @param burst         Maximum burst size (tokens). Must be >= 1.
    explicit RateLimiter(double rate_per_sec, std::size_t burst) noexcept
        : RateLimiter(Config{rate_per_sec, burst})
    {}

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

    /// @brief Acquire @p n tokens, blocking (via yield) until available or timeout.
    ///
    /// Intended for convenience paths where callers prefer blocking over
    /// retry logic. Uses yield() to avoid busy-spinning.
    ///
    /// @param n        Number of tokens to consume (default 1).
    /// @param timeout  Maximum duration to wait (default 10s). Prevents infinite
    ///                 spin when rate is zero or tokens cannot refill fast enough.
    /// @return true if tokens were acquired, false if the timeout elapsed first.
    /// @warning This method spins with std::this_thread::yield(). Do not call
    ///          on hot paths -- prefer try_acquire() with explicit backoff.
    [[nodiscard]] bool acquire(std::size_t n = 1,
                               std::chrono::milliseconds timeout =
                                   std::chrono::milliseconds{10000}) noexcept {
        SPDLOG_LOGGER_DEBUG(detail::rate_limiter_logger(),
                     "acquire({}) blocking until tokens available (timeout={}ms)",
                     n, timeout.count());
        auto deadline = Clock::now() + timeout;
        while (!try_acquire(n)) {
            if (Clock::now() >= deadline) [[unlikely]] {
                SPDLOG_LOGGER_WARN(detail::rate_limiter_logger(),
                    "acquire({}) timed out after {}ms", n, timeout.count());
                return false;
            }
            std::this_thread::yield();
        }
        SPDLOG_LOGGER_DEBUG(detail::rate_limiter_logger(), "acquire({}) completed", n);
        return true;
    }

    /// @brief Check if the limiter has no tokens available.
    ///
    /// Convenience predicate for monitoring and condition checks.
    /// Performs a refill before checking, same as available().
    ///
    /// @return true if available tokens < 1.0 (cannot satisfy try_acquire(1)).
    [[nodiscard]] bool is_exhausted() noexcept {
        return available() < 1.0;
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

    /// @brief Read-only access to the limiter's immutable configuration.
    /// @return Const reference to the Config used at construction.
    [[nodiscard]] const Config& config() const noexcept { return config_; }

    /// @brief Read-only access to the limiter's burst capacity.
    /// @return Maximum token capacity (matches the burst value from Config).
    [[nodiscard]] double burst() const noexcept { return burst_; }

    /// @brief Read-only access to the refill rate in tokens per nanosecond.
    /// @return Internal rate expressed in tokens/ns. Multiply by 1e9 for tokens/sec.
    [[nodiscard]] double rate_per_ns() const noexcept { return rate_per_ns_; }

    /// @brief JSON-formatted snapshot of current limiter state for monitoring.
    ///
    /// Includes both configuration and live state (available tokens).
    /// Thread-safe: takes a lock to read consistent state.
    [[nodiscard]] std::string to_json() {
        std::lock_guard<std::mutex> lock(mu_);
        refill_locked();
        return std::format(
            "{{\"available_tokens\":{:.2f},\"config\":{}}}",
            tokens_,
            config_.to_json());
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

    Config    config_;        ///< Limiter configuration (immutable after construction).
    double    rate_per_ns_;   ///< Tokens generated per nanosecond
    double    burst_;         ///< Maximum token capacity
    double    tokens_;        ///< Current available tokens (fractional for smooth accumulation)
    TimePoint last_refill_;   ///< Timestamp of last token refill

    mutable std::mutex mu_;   ///< Protects all mutable state
};

} // namespace eph::net

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for RateLimiter::Config
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::net::RateLimiter::Config> : std::formatter<std::string> {
    auto format(const eph::net::RateLimiter::Config& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("RateLimiter::Config(rate={:.2f}/s, burst={})",
                c.rate_per_sec, c.burst),
            ctx);
    }
};
