#pragma once

/// @file backoff.hpp
/// Generic retry-backoff strategies — the delay-sequence layer of the
/// `eph::utils::retry` facility (the blocking driver lives in `retry.hpp`).
///
/// This is the eph analogue of Rust `backon`'s `Backoff` trait: a strategy is
/// just a stateful generator of delay durations. The `Backoff` concept models
/// exactly one operation — `next_delay()` — returning the wait before the next
/// attempt, or `std::nullopt` once the strategy is exhausted. Two concrete
/// strategies ship here:
///
///   - `ExponentialBackoff` — exponential growth + ±jitter + cap + attempt
///     budget. This is the math formerly known as `eph::net::ReconnectPolicy`
///     (moved here so the same code backs both `eph::utils::retry` and
///     `eph::net::ReconnectOrchestrator` — one source of truth for the
///     saturating-cast UB guards rather than a copy per consumer).
///   - `ConstantBackoff` — a fixed delay (optionally jittered) per attempt.
///
/// Design points (mirrors the rest of eph-utils — see `rate_limiter.hpp` /
/// `kill_switch.hpp`):
///   - Nested `Config` struct; designated-initializer friendly.
///   - `noexcept` throughout; NaN/Inf-safe construction clamps.
///   - Not internally synchronized: each retry session / each connection owns
///     its own backoff instance (matches the HFT one-policy-per-connection
///     model). Share across threads only behind external synchronization.
///   - **No spdlog dependency — pure math.** Observability lives at the call
///     site (`retry()` logs each attempt; `ReconnectOrchestrator` logs at its
///     state transitions). Keeping this header log-free lets it be reused in
///     any context, including ones that have not initialized a logger.
///   - `next_delay()` is the single advancing operation. The first call
///     returns (roughly) the initial delay; each subsequent call advances the
///     strategy and decrements the remaining attempt budget. Once the budget
///     is exhausted it returns `std::nullopt` and the state is **absorbing**:
///     further calls keep returning `nullopt` without advancing internal
///     state (no spurious base growth, no attempt over-count).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>

namespace eph::utils {

// ---------------------------------------------------------------------------
// Backoff concept
// ---------------------------------------------------------------------------

/// @brief A retry-backoff strategy: a stateful generator of inter-attempt
///        delays.
///
/// `next_delay()` returns the duration to wait before the next attempt, or
/// `std::nullopt` once the strategy is exhausted (the caller should then stop
/// retrying). The duration unit is `std::chrono::milliseconds` — retry/backoff
/// delays are inherently millisecond-to-second scale, and pinning the unit
/// keeps the concept simple and the consumers (`retry()`,
/// `ReconnectOrchestrator`) monomorphic.
template <class B>
concept Backoff = requires(B b) {
    { b.next_delay() } -> std::same_as<std::optional<std::chrono::milliseconds>>;
};

// ---------------------------------------------------------------------------
// detail::apply_symmetric_jitter — shared saturating-cast jitter draw
// ---------------------------------------------------------------------------

namespace detail {

/// @brief Apply symmetric ±`jitter_factor` jitter to a duration.
///
/// Picks a uniformly random value in `[base*(1-jf), base*(1+jf))`. When
/// `jitter_factor == 0` (or `base <= 0`), returns `base` unchanged
/// (deterministic). Shared by every `Backoff` strategy so the saturating
/// float→int64 cast — the load-bearing UB guard — exists in exactly one place.
///
/// @param base          The (already-grown) base delay to perturb.
/// @param jitter_factor Symmetric fraction in [0, 1). Caller must have already
///                      sanitized NaN/Inf/out-of-range at construction time.
[[nodiscard]] inline std::chrono::milliseconds
apply_symmetric_jitter(std::chrono::milliseconds base,
                       double jitter_factor) noexcept {
    if (base.count() <= 0 || jitter_factor == 0.0) return base;

    // Thread-local RNG seeded from multiple entropy sources so simultaneous
    // backoffs across threads do not lockstep. Matches the legacy
    // ReconnectPolicy behavior exactly.
    thread_local std::mt19937_64 rng([] {
        std::seed_seq seeds{
            std::random_device{}(),
            std::random_device{}(),
            static_cast<unsigned>(
                std::chrono::steady_clock::now().time_since_epoch().count())};
        return std::mt19937_64{seeds};
    }());

    const auto count = static_cast<double>(base.count());
    const auto low   = count * (1.0 - jitter_factor);
    const auto high  = count * (1.0 + jitter_factor);
    std::uniform_real_distribution<double> dist(low, high);
    const double sample = dist(rng);

    // Saturated FP→int64 conversion. When `base` is at saturation
    // (INT64_MAX, reachable via ExponentialBackoff with an unbounded
    // max_backoff after ~63 doublings) the upper jitter end is
    // `INT64_MAX * (1+jf) ≈ 1.15e19`, representable as a double but
    // overflowing int64_t. A naked `static_cast<int64_t>(huge)` is UB per
    // [conv.fpint] — typical x86 GCC returns INT64_MIN, which the `< 0`
    // clamp below would turn into 0, defeating the backoff (the driver then
    // sees delay <= 0 and retries immediately, the exact opposite of the
    // intended "saturate to ~forever"). Clamp to INT64_MAX before the cast.
    constexpr double kInt64MaxAsDouble =
        static_cast<double>(std::numeric_limits<int64_t>::max());
    constexpr double kInt64MinAsDouble =
        static_cast<double>(std::numeric_limits<int64_t>::min());
    int64_t picked;
    if (sample >= kInt64MaxAsDouble) {
        picked = std::numeric_limits<int64_t>::max();
    } else if (sample <= kInt64MinAsDouble) {
        picked = 0;  // negative-going drift → clamp (next branch handles)
    } else {
        picked = static_cast<int64_t>(sample);
    }
    if (picked < 0) picked = 0;  // never return a negative duration
    return std::chrono::milliseconds{picked};
}

}  // namespace detail

// ---------------------------------------------------------------------------
// ExponentialBackoff
// ---------------------------------------------------------------------------

/// @brief Exponential-growth backoff with ±jitter, a delay cap, and an
///        optional attempt budget.
///
/// Formerly `eph::net::ReconnectPolicy`. Intended usage with the blocking
/// driver:
///
/// @code
///   auto r = eph::utils::retry(
///       [&]{ return connect_once(); },
///       eph::utils::ExponentialBackoff{{.initial_backoff = 100ms,
///                                       .max_backoff     = 2s,
///                                       .max_attempts    = 5}});
/// @endcode
///
/// Or directly as a delay generator (e.g. inside `ReconnectOrchestrator`):
///
/// @code
///   ExponentialBackoff b{cfg};
///   while (auto delay = b.next_delay()) { schedule_after(*delay); }
///   // nullopt → budget exhausted
/// @endcode
class ExponentialBackoff {
public:
    /// @brief Static configuration for `ExponentialBackoff`.
    struct Config {
        /// @brief Delay before the first retry attempt.
        std::chrono::milliseconds initial_backoff{100};

        /// @brief Hard cap on per-attempt delay after exponential growth.
        /// Clamped up to `initial_backoff` if smaller.
        std::chrono::milliseconds max_backoff{1600};

        /// @brief Exponential growth factor applied after each attempt.
        /// `1.0` means **constant** delay (no growth); `2.0` is classic
        /// doubling. Values `< 1.0`, NaN, and ±Inf are sanitized to `2.0`
        /// (see ctor). Note: `1.0` is explicitly *legal* — a generic backoff
        /// must be able to express a constant delay without being silently
        /// promoted to doubling.
        double multiplier{2.0};

        /// @brief Symmetric jitter fraction. `0.25` = ±25%. Must be in
        /// `[0.0, 1.0)`; `0.0` disables jitter (deterministic timing).
        double jitter_factor{0.25};

        /// @brief Maximum number of delays produced before `next_delay()`
        /// returns `std::nullopt`. `0` means unlimited.
        uint32_t max_attempts{0};
    };

    /// @brief Construct from config, sanitizing obviously-broken values so a
    /// default-constructed config is always usable:
    ///   - `initial_backoff <= 0` → `100ms` (a non-positive base feeds the
    ///     multiplier a non-positive value every call, marching `current_base_`
    ///     monotonically more negative until `count()*multiplier` exceeds the
    ///     int64 range and the float→int64 cast is UB per [conv.fpint]; zero is
    ///     a hot busy-loop — same fix).
    ///   - `multiplier < 1.0` → `2.0`. `1.0` is kept (constant backoff).
    ///   - `multiplier` non-finite (NaN/Inf) → `2.0`. NaN slips past `< 1.0`
    ///     (NaN comparisons are false) and would poison the float→int64 cast;
    ///     `isfinite` catches NaN and ±Inf together.
    ///   - `jitter_factor < 0` or non-finite → `0`. A NaN jitter would reach
    ///     `std::uniform_real_distribution(NaN, NaN)` which is UB.
    ///   - `jitter_factor >= 1` → `0.999`.
    ///   - `max_backoff < initial_backoff` → `initial_backoff`.
    explicit ExponentialBackoff(Config cfg) noexcept
        : cfg_(cfg), current_base_{}, attempts_(0) {
        constexpr std::chrono::milliseconds kFallbackInitial{100};
        if (cfg_.initial_backoff.count() <= 0) {
            cfg_.initial_backoff = kFallbackInitial;
        }
        // isfinite catches NaN and ±Inf; `< 1.0` then handles the finite
        // too-small case. `== 1.0` (constant backoff) is intentionally left
        // untouched.
        if (!std::isfinite(cfg_.multiplier) || cfg_.multiplier < 1.0) {
            cfg_.multiplier = 2.0;
        }
        if (!std::isfinite(cfg_.jitter_factor) || cfg_.jitter_factor < 0.0) {
            cfg_.jitter_factor = 0.0;
        }
        if (cfg_.jitter_factor >= 1.0) cfg_.jitter_factor = 0.999;
        if (cfg_.max_backoff < cfg_.initial_backoff) {
            cfg_.max_backoff = cfg_.initial_backoff;
        }
        current_base_ = cfg_.initial_backoff;
    }

    /// @brief Return the delay for the next attempt and advance, or
    ///        `std::nullopt` if the attempt budget is exhausted.
    ///
    /// The first call returns (roughly) `initial_backoff`; each subsequent
    /// call multiplies the base by `multiplier` (capped at `max_backoff`) and
    /// increments the attempt count. The returned value uses the *pre-advance*
    /// base with ±jitter applied. Exhaustion is **absorbing**: after the first
    /// `nullopt`, internal state is frozen (no further base growth or attempt
    /// increment), so over-polling a finished backoff is harmless.
    [[nodiscard]] std::optional<std::chrono::milliseconds> next_delay() noexcept {
        if (cfg_.max_attempts != 0 && attempts_ >= cfg_.max_attempts) {
            return std::nullopt;  // exhausted (absorbing)
        }

        auto base = current_base_;
        ++attempts_;

        // Advance the base for the NEXT call. Saturated FP→int64 conversion:
        // once `current_base_` approaches INT64_MAX (unbounded max_backoff,
        // ~63 doublings), `count() * multiplier` exceeds the int64 range and a
        // naked `static_cast<int64_t>` is UB per [conv.fpint]. Clamp to
        // INT64_MAX so the subsequent `std::min` sees a well-defined value.
        // With multiplier == 1.0 the base never grows — constant backoff.
        const double scaled = static_cast<double>(current_base_.count()) * cfg_.multiplier;
        constexpr double kInt64MaxAsDouble =
            static_cast<double>(std::numeric_limits<int64_t>::max());
        const int64_t scaled_int =
            (scaled >= kInt64MaxAsDouble)
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(scaled);
        current_base_ =
            std::min(std::chrono::milliseconds{scaled_int}, cfg_.max_backoff);

        return detail::apply_symmetric_jitter(base, cfg_.jitter_factor);
    }

    /// @brief Reset attempt count and base to the initial configured state.
    /// Call after a success when reusing the same backoff for a new session.
    void reset() noexcept {
        attempts_ = 0;
        current_base_ = cfg_.initial_backoff;
    }

    /// @brief Number of delays produced since construction / last `reset()`.
    [[nodiscard]] uint32_t attempts() const noexcept { return attempts_; }

    /// @brief Accessor for the post-clamp config (diagnostics / tests).
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

private:
    Config cfg_;
    std::chrono::milliseconds current_base_;
    uint32_t attempts_;
};

// ---------------------------------------------------------------------------
// ConstantBackoff
// ---------------------------------------------------------------------------

/// @brief Fixed-delay backoff (optionally jittered) with an attempt budget.
///
/// The "wait the same amount each time" strategy. Equivalent to
/// `ExponentialBackoff` with `multiplier == 1.0`, but exposed as its own type
/// because that intent reads far more clearly at the call site.
///
/// @code
///   eph::utils::ConstantBackoff cb{{.delay = 200ms, .max_attempts = 3}};
///   auto r = eph::utils::retry(fn, cb);
/// @endcode
class ConstantBackoff {
public:
    /// @brief Static configuration for `ConstantBackoff`.
    struct Config {
        /// @brief Fixed delay before each retry. `0` means retry immediately
        /// (the driver's sleeper skips a zero wait). Negative values are
        /// clamped to `0`.
        std::chrono::milliseconds delay{100};

        /// @brief Symmetric jitter fraction in `[0.0, 1.0)`. `0.0` (default)
        /// disables jitter. NaN/Inf/negative → `0`; `>= 1` → `0.999`.
        double jitter_factor{0.0};

        /// @brief Maximum number of delays produced before `next_delay()`
        /// returns `std::nullopt`. `0` means unlimited.
        uint32_t max_attempts{0};
    };

    explicit ConstantBackoff(Config cfg) noexcept : cfg_(cfg), attempts_(0) {
        if (cfg_.delay.count() < 0) cfg_.delay = std::chrono::milliseconds{0};
        if (!std::isfinite(cfg_.jitter_factor) || cfg_.jitter_factor < 0.0) {
            cfg_.jitter_factor = 0.0;
        }
        if (cfg_.jitter_factor >= 1.0) cfg_.jitter_factor = 0.999;
    }

    /// @brief Return the fixed delay (with optional jitter) and advance, or
    ///        `std::nullopt` once the attempt budget is exhausted (absorbing).
    [[nodiscard]] std::optional<std::chrono::milliseconds> next_delay() noexcept {
        if (cfg_.max_attempts != 0 && attempts_ >= cfg_.max_attempts) {
            return std::nullopt;
        }
        ++attempts_;
        return detail::apply_symmetric_jitter(cfg_.delay, cfg_.jitter_factor);
    }

    void reset() noexcept { attempts_ = 0; }
    [[nodiscard]] uint32_t attempts() const noexcept { return attempts_; }
    [[nodiscard]] const Config& config() const noexcept { return cfg_; }

private:
    Config cfg_;
    uint32_t attempts_;
};

// Both strategies must satisfy the Backoff concept — caught at compile time so
// a refactor that changes a signature is a hard error here, not at the
// consumer.
static_assert(Backoff<ExponentialBackoff>,
              "ExponentialBackoff must satisfy the Backoff concept");
static_assert(Backoff<ConstantBackoff>,
              "ConstantBackoff must satisfy the Backoff concept");

}  // namespace eph::utils
