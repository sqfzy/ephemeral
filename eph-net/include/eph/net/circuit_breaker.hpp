#pragma once

/// @file circuit_breaker.hpp
/// Three-state circuit breaker for exchange endpoint protection.
///
/// Prevents hammering broken exchange endpoints by tracking consecutive
/// failures and backing off. Classic three-state model:
///
///   Closed --[failures >= threshold]--> Open
///   Open   --[open_duration elapsed]--> HalfOpen
///   HalfOpen --[success]--------------> Closed
///   HalfOpen --[failure]--------------> Open (timer reset)
///
/// Thread-safe via std::mutex (not on hot data path — same rationale as
/// RateLimiter). Uses steady_clock for monotonic timing.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <spdlog/spdlog.h>

namespace eph::net {

/// Circuit breaker state.
enum class CircuitState : uint8_t {
    Closed,    ///< Normal operation — calls are allowed.
    Open,      ///< Tripped — calls are blocked until open_duration elapses.
    HalfOpen,  ///< Probing — limited test calls allowed to check recovery.
};

/// Thread-safe circuit breaker for protecting against broken endpoints.
///
/// Designed for exchange connectivity paths where repeated failures to a
/// down endpoint waste resources and may trigger IP bans.
class CircuitBreaker {
public:
    /// Configuration for the circuit breaker.
    struct Config {
        std::size_t          failure_threshold;    ///< Trip to Open after N consecutive failures.
        std::chrono::seconds open_duration;        ///< How long to stay Open before probing.
        std::size_t          half_open_max_calls;  ///< Max concurrent test calls in HalfOpen.

        /// Default configuration: 5 failures, 30s cooldown, 1 probe call.
        constexpr Config() noexcept
            : failure_threshold{5}, open_duration{30}, half_open_max_calls{1} {}

        constexpr Config(std::size_t thresh, std::chrono::seconds dur,
                         std::size_t ho_max = 1) noexcept
            : failure_threshold{thresh}, open_duration{dur}, half_open_max_calls{ho_max} {}
    };

    /// Construct a circuit breaker with the given configuration.
    explicit CircuitBreaker(Config config = Config{}) noexcept
        : config_{config}
        , state_{CircuitState::Closed}
        , failure_count_{0}
        , half_open_calls_{0}
        , opened_at_{}
    {
        SPDLOG_DEBUG("CircuitBreaker created: threshold={} open_duration={}s half_open_max={}",
                     config_.failure_threshold, config_.open_duration.count(),
                     config_.half_open_max_calls);
    }

    /// Check if a call is allowed through the breaker.
    ///
    /// In Closed state, always allows. In Open state, checks if open_duration
    /// has elapsed and transitions to HalfOpen if so. In HalfOpen state,
    /// allows up to half_open_max_calls concurrent test calls.
    ///
    /// @return true if the call should proceed, false if the circuit is open.
    [[nodiscard]] bool allow() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return allow_locked();
    }

    /// Record a successful call result.
    ///
    /// In Closed state, resets the consecutive failure count.
    /// In HalfOpen state, transitions to Closed (endpoint recovered).
    void record_success() noexcept {
        std::lock_guard<std::mutex> lock(mu_);

        switch (state_) {
        case CircuitState::Closed:
            // Reset failure count — endpoint is healthy.
            if (failure_count_ > 0) {
                SPDLOG_DEBUG("CircuitBreaker: success in Closed, resetting failure_count from {}",
                             failure_count_);
                failure_count_ = 0;
            }
            break;

        case CircuitState::HalfOpen:
            // Probe succeeded — endpoint has recovered, close the circuit.
            SPDLOG_INFO("CircuitBreaker: success in HalfOpen, transitioning to Closed");
            state_ = CircuitState::Closed;
            failure_count_ = 0;
            half_open_calls_ = 0;
            break;

        case CircuitState::Open:
            // Shouldn't normally happen (calls are blocked in Open), but
            // handle gracefully — ignore.
            SPDLOG_WARN("CircuitBreaker: record_success called in Open state (unexpected)");
            break;
        }
    }

    /// Record a failed call result.
    ///
    /// In Closed state, increments the consecutive failure count and trips
    /// to Open if the threshold is reached. In HalfOpen state, transitions
    /// back to Open (probe failed, endpoint still broken).
    void record_failure() noexcept {
        std::lock_guard<std::mutex> lock(mu_);

        switch (state_) {
        case CircuitState::Closed:
            ++failure_count_;
            SPDLOG_DEBUG("CircuitBreaker: failure in Closed, count={}/{}", failure_count_,
                         config_.failure_threshold);

            if (failure_count_ >= config_.failure_threshold) {
                trip_locked();
            }
            break;

        case CircuitState::HalfOpen:
            // Probe failed — endpoint is still broken, re-open with fresh timer.
            SPDLOG_WARN("CircuitBreaker: failure in HalfOpen, re-opening circuit");
            trip_locked();
            break;

        case CircuitState::Open:
            // Shouldn't normally happen (calls are blocked), handle gracefully.
            SPDLOG_WARN("CircuitBreaker: record_failure called in Open state (unexpected)");
            break;
        }
    }

    /// Current circuit state.
    ///
    /// Note: in Open state, this evaluates the timeout and may report HalfOpen
    /// if the open_duration has elapsed (without actually transitioning — the
    /// transition happens in allow()).
    [[nodiscard]] CircuitState state() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_ == CircuitState::Open && open_duration_elapsed_locked()) {
            return CircuitState::HalfOpen;
        }
        return state_;
    }

    /// Current consecutive failure count.
    [[nodiscard]] std::size_t failure_count() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return failure_count_;
    }

    /// Force-reset the circuit breaker to Closed state.
    ///
    /// Clears failure count and half-open call tracking.
    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        SPDLOG_INFO("CircuitBreaker: manual reset to Closed (was state={}, failures={})",
                    static_cast<int>(state_), failure_count_);
        state_ = CircuitState::Closed;
        failure_count_ = 0;
        half_open_calls_ = 0;
    }

private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Check if open_duration has elapsed since the circuit was opened.
    /// MUST be called with mu_ held.
    [[nodiscard]] bool open_duration_elapsed_locked() const noexcept {
        return Clock::now() - opened_at_ >= config_.open_duration;
    }

    /// Transition to Open state with a fresh timer.
    /// MUST be called with mu_ held.
    void trip_locked() noexcept {
        state_ = CircuitState::Open;
        opened_at_ = Clock::now();
        half_open_calls_ = 0;
        SPDLOG_WARN("CircuitBreaker: tripped to Open, failures={}, cooldown={}s",
                    failure_count_, config_.open_duration.count());
    }

    /// Check if a call is allowed (lock must already be held).
    [[nodiscard]] bool allow_locked() noexcept {
        switch (state_) {
        case CircuitState::Closed:
            return true;

        case CircuitState::Open:
            if (open_duration_elapsed_locked()) {
                // Transition to HalfOpen — allow limited probe calls.
                SPDLOG_INFO("CircuitBreaker: open_duration elapsed, transitioning to HalfOpen");
                state_ = CircuitState::HalfOpen;
                half_open_calls_ = 1;
                return true;
            }
            SPDLOG_TRACE("CircuitBreaker: call blocked (Open)");
            return false;

        case CircuitState::HalfOpen:
            if (half_open_calls_ < config_.half_open_max_calls) {
                ++half_open_calls_;
                SPDLOG_TRACE("CircuitBreaker: allowing HalfOpen call {}/{}",
                             half_open_calls_, config_.half_open_max_calls);
                return true;
            }
            SPDLOG_TRACE("CircuitBreaker: call blocked (HalfOpen, max test calls reached)");
            return false;
        }

        // Unreachable — all enum values handled above.
        return false;
    }

    Config       config_;            ///< Breaker configuration (immutable after construction).
    CircuitState state_;             ///< Current circuit state.
    std::size_t  failure_count_;     ///< Consecutive failure count.
    std::size_t  half_open_calls_;   ///< Number of test calls issued in current HalfOpen period.
    TimePoint    opened_at_;         ///< When the circuit was last tripped to Open.

    mutable std::mutex mu_;          ///< Protects all mutable state.
};

} // namespace eph::net
