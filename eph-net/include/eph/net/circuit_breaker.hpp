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
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace eph::net {

namespace detail {
/// @brief Lazily-initialized logger for the CircuitBreaker subsystem.
/// @return Pointer to the "net.circuit_breaker" spdlog logger.
inline spdlog::logger* circuit_breaker_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.circuit_breaker");
        if (!lg) lg = spdlog::stdout_color_mt("net.circuit_breaker");
        return lg;
    }();
    return l.get();
}
} // namespace detail

/// @brief Circuit breaker state.
///
/// Represents the three states of the circuit breaker automaton:
/// Closed (normal), Open (tripped), and HalfOpen (probing recovery).
enum class CircuitState : uint8_t {
    Closed,    ///< Normal operation — calls are allowed.
    Open,      ///< Tripped — calls are blocked until open_duration elapses.
    HalfOpen,  ///< Probing — limited test calls allowed to check recovery.
};

/// @brief Convert a CircuitState value to a human-readable name.
/// @param s  The circuit state to convert.
/// @return Null-terminated string view such as "CLOSED", "OPEN", "HALF_OPEN".
[[nodiscard]] inline constexpr std::string_view circuit_state_name(CircuitState s) noexcept {
    switch (s) {
    case CircuitState::Closed:   return "CLOSED";
    case CircuitState::Open:     return "OPEN";
    case CircuitState::HalfOpen: return "HALF_OPEN";
    }
    return "UNKNOWN";
}

/// Thread-safe circuit breaker for protecting against broken endpoints.
///
/// Designed for exchange connectivity paths where repeated failures to a
/// down endpoint waste resources and may trigger IP bans.
class CircuitBreaker {
public:
    /// @brief Configuration for the circuit breaker.
    ///
    /// All fields are immutable after construction of the CircuitBreaker.
    struct Config {
        std::size_t               failure_threshold;    ///< Trip to Open after N consecutive failures.
        std::chrono::milliseconds open_duration;        ///< How long to stay Open before probing (ms precision for HFT use).
        std::size_t               half_open_max_calls;  ///< Max concurrent test calls in HalfOpen.

        /// @brief Default configuration: 5 failures, 30s cooldown, 1 probe call.
        constexpr Config() noexcept
            : failure_threshold{5}, open_duration{30000}, half_open_max_calls{1} {}

        /// @brief Construct with explicit values.
        /// @param thresh  Number of consecutive failures before tripping to Open.
        /// @param dur     Duration to remain Open before transitioning to HalfOpen.
        /// @param ho_max  Maximum concurrent probe calls allowed in HalfOpen state.
        constexpr Config(std::size_t thresh, std::chrono::milliseconds dur,
                         std::size_t ho_max = 1) noexcept
            : failure_threshold{thresh}, open_duration{dur}, half_open_max_calls{ho_max} {}

        /// @brief Convenience constructor accepting std::chrono::seconds.
        /// @param thresh  Number of consecutive failures before tripping to Open.
        /// @param dur     Duration to remain Open before transitioning to HalfOpen.
        /// @param ho_max  Maximum concurrent probe calls allowed in HalfOpen state.
        constexpr Config(std::size_t thresh, std::chrono::seconds dur,
                         std::size_t ho_max = 1) noexcept
            : failure_threshold{thresh}
            , open_duration{std::chrono::duration_cast<std::chrono::milliseconds>(dur)}
            , half_open_max_calls{ho_max} {}

        /// @brief Validate configuration, returning an error description or
        ///        empty string on success.
        ///
        /// Consistent with TransportConfig::validate() and SocketConfig::validate().
        [[nodiscard]] constexpr std::string_view validate() const noexcept {
            if (failure_threshold == 0)
                return "failure_threshold must be > 0";
            if (open_duration.count() < 0)
                return "open_duration must be >= 0";
            if (half_open_max_calls == 0)
                return "half_open_max_calls must be > 0";
            return {};
        }

        /// Multi-line formatted dump for logging/debugging.
        [[nodiscard]] std::string dump() const {
            return std::format(
                "CircuitBreaker::Config:\n"
                "  failure_threshold: {}, open_duration: {}ms, "
                "half_open_max_calls: {}",
                failure_threshold, open_duration.count(),
                half_open_max_calls);
        }

        /// JSON-formatted config for monitoring system integration.
        [[nodiscard]] std::string to_json() const {
            return std::format(
                "{{\"failure_threshold\":{},\"open_duration_ms\":{},"
                "\"half_open_max_calls\":{}}}",
                failure_threshold, open_duration.count(),
                half_open_max_calls);
        }

        /// Defaulted equality -- all fields must match exactly.
        [[nodiscard]] friend bool operator==(const Config&,
                                             const Config&) = default;

        /// Check for non-fatal contradictions or likely misconfigurations.
        /// Returns a list of warning messages (empty if no issues).
        /// Unlike validate() which blocks construction, these are advisory.
        [[nodiscard]] std::vector<std::string> warnings() const {
            std::vector<std::string> w;
            if (failure_threshold == 1)
                w.emplace_back("failure_threshold=1 — circuit will trip on any "
                               "single failure (consider threshold >= 3 for "
                               "transient error tolerance)");
            if (open_duration.count() == 0)
                w.emplace_back("open_duration=0ms — circuit transitions to "
                               "HalfOpen immediately (useful for testing, not "
                               "production)");
            if (half_open_max_calls > 10)
                w.emplace_back(std::format(
                    "half_open_max_calls={} is unusually high — more probe "
                    "calls increase load on a potentially degraded endpoint",
                    half_open_max_calls));
            if (open_duration > std::chrono::minutes{10})
                w.emplace_back(std::format(
                    "open_duration={}ms (>{} min) — long cooldown delays "
                    "recovery detection", open_duration.count(),
                    std::chrono::duration_cast<std::chrono::minutes>(
                        open_duration).count()));
            return w;
        }
    };

    /// @brief Construct a circuit breaker with the given configuration.
    /// @param config  Breaker configuration (threshold, duration, probe limit).
    explicit CircuitBreaker(Config config = Config{}) noexcept
        : config_{config}
        , state_{CircuitState::Closed}
        , failure_count_{0}
        , half_open_calls_{0}
        , opened_at_{}
    {
        SPDLOG_LOGGER_DEBUG(detail::circuit_breaker_logger(),
                     "CircuitBreaker created: threshold={} open_duration={}ms half_open_max={}",
                     config_.failure_threshold, config_.open_duration.count(),
                     config_.half_open_max_calls);
    }

    /// @brief Check if a call is allowed through the breaker.
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

    /// @brief Record a successful call result.
    ///
    /// In Closed state, resets the consecutive failure count.
    /// In HalfOpen state, transitions to Closed (endpoint recovered).
    /// @note Safe to call from any state; a call in Open is unexpected
    ///       but handled gracefully with a warning log.
    void record_success() noexcept {
        std::lock_guard<std::mutex> lock(mu_);

        switch (state_) {
        case CircuitState::Closed:
            // Reset failure count — endpoint is healthy.
            if (failure_count_ > 0) {
                SPDLOG_LOGGER_DEBUG(detail::circuit_breaker_logger(),
                             "CircuitBreaker: success in Closed, resetting failure_count from {}",
                             failure_count_);
                failure_count_ = 0;
            }
            break;

        case CircuitState::HalfOpen:
            // Probe succeeded — endpoint has recovered, close the circuit.
            SPDLOG_LOGGER_INFO(detail::circuit_breaker_logger(),
                        "CircuitBreaker: success in HalfOpen, transitioning to Closed");
            state_ = CircuitState::Closed;
            failure_count_ = 0;
            half_open_calls_ = 0;
            break;

        case CircuitState::Open:
            // Shouldn't normally happen (calls are blocked in Open), but
            // handle gracefully — ignore.
            SPDLOG_LOGGER_WARN(detail::circuit_breaker_logger(),
                        "CircuitBreaker: record_success called in Open state (unexpected)");
            break;
        }
    }

    /// @brief Record a failed call result.
    ///
    /// In Closed state, increments the consecutive failure count and trips
    /// to Open if the threshold is reached. In HalfOpen state, transitions
    /// back to Open (probe failed, endpoint still broken).
    /// @note Safe to call from any state; a call in Open is unexpected
    ///       but handled gracefully with a warning log.
    void record_failure() noexcept {
        std::lock_guard<std::mutex> lock(mu_);

        switch (state_) {
        case CircuitState::Closed:
            ++failure_count_;
            SPDLOG_LOGGER_DEBUG(detail::circuit_breaker_logger(),
                        "CircuitBreaker: failure in Closed, count={}/{}", failure_count_,
                         config_.failure_threshold);

            if (failure_count_ >= config_.failure_threshold) {
                trip_locked();
            }
            break;

        case CircuitState::HalfOpen:
            // Probe failed — endpoint is still broken, re-open with fresh timer.
            SPDLOG_LOGGER_WARN(detail::circuit_breaker_logger(),
                        "CircuitBreaker: failure in HalfOpen, re-opening circuit");
            trip_locked();
            break;

        case CircuitState::Open:
            // Shouldn't normally happen (calls are blocked), handle gracefully.
            SPDLOG_LOGGER_WARN(detail::circuit_breaker_logger(),
                        "CircuitBreaker: record_failure called in Open state (unexpected)");
            break;
        }
    }

    /// @brief Query the current circuit state.
    ///
    /// @return The current CircuitState.
    /// @note In Open state, this evaluates the timeout and may report HalfOpen
    ///       if the open_duration has elapsed (without actually transitioning --
    ///       the transition happens in allow()).
    [[nodiscard]] CircuitState state() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        if (state_ == CircuitState::Open && open_duration_elapsed_locked()) {
            return CircuitState::HalfOpen;
        }
        return state_;
    }

    /// @brief Current consecutive failure count.
    /// @return The number of consecutive failures recorded since the last reset
    ///         or successful call.
    [[nodiscard]] std::size_t failure_count() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return failure_count_;
    }

    /// @brief Force-reset the circuit breaker to Closed state.
    ///
    /// Clears failure count and half-open call tracking.
    /// Idempotent -- safe to call multiple times.
    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        SPDLOG_LOGGER_INFO(detail::circuit_breaker_logger(),
                    "CircuitBreaker: manual reset to Closed (was state={}, failures={})",
                    circuit_state_name(state_), failure_count_);
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
        SPDLOG_LOGGER_WARN(detail::circuit_breaker_logger(),
                    "CircuitBreaker: tripped to Open, failures={}, cooldown={}ms",
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
                SPDLOG_LOGGER_INFO(detail::circuit_breaker_logger(),
                            "CircuitBreaker: open_duration elapsed, transitioning to HalfOpen");
                state_ = CircuitState::HalfOpen;
                half_open_calls_ = 1;
                return true;
            }
            SPDLOG_LOGGER_TRACE(detail::circuit_breaker_logger(),
                         "CircuitBreaker: call blocked (Open)");
            return false;

        case CircuitState::HalfOpen:
            if (half_open_calls_ < config_.half_open_max_calls) {
                ++half_open_calls_;
                SPDLOG_LOGGER_TRACE(detail::circuit_breaker_logger(),
                             "CircuitBreaker: allowing HalfOpen call {}/{}",
                             half_open_calls_, config_.half_open_max_calls);
                return true;
            }
            SPDLOG_LOGGER_TRACE(detail::circuit_breaker_logger(),
                         "CircuitBreaker: call blocked (HalfOpen, max test calls reached)");
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

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for CircuitState
// ─────────────────────────────────────────────────────────────────────────────

/// @brief std::formatter specialization for CircuitState.
///
/// Formats as "CLOSED", "OPEN", or "HALF_OPEN" for use with std::format/spdlog.
template <>
struct std::formatter<eph::net::CircuitState> : std::formatter<std::string_view> {
    auto format(eph::net::CircuitState s, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::net::circuit_state_name(s), ctx);
    }
};

/// @brief std::formatter for CircuitBreaker::Config -- compact one-line summary.
template <>
struct std::formatter<eph::net::CircuitBreaker::Config> : std::formatter<std::string> {
    auto format(const eph::net::CircuitBreaker::Config& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("CircuitBreaker::Config(thresh={}, open={}ms, ho_max={})",
                c.failure_threshold, c.open_duration.count(),
                c.half_open_max_calls),
            ctx);
    }
};
