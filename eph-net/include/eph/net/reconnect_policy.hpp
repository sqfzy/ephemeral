#pragma once

/// @file reconnect_policy.hpp
/// Exponential-backoff reconnection policy with ±jitter.
///
/// Config-struct-driven `ReconnectPolicy` owned by `KernelTcpStream` /
/// `DpdkTcpStream`.
///
/// Key design points:
///   - Owns its own `ReconnectPolicyConfig` value (no external reference).
///   - `next_backoff()` is synchronous and side-effect-only (no sleep, no
///     connection attempt). The caller drives the actual connect, which
///     keeps this class trivially unit-testable.
///   - Jitter range is configurable via `jitter_factor_` (default ±25%).
///   - No spdlog dependency — pure math. Logging lives at the call site.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

namespace eph::net {

// ---------------------------------------------------------------------------
// ReconnectPolicyConfig
// ---------------------------------------------------------------------------

/// @brief Static configuration for `ReconnectPolicy`.
///
/// Defaults mirror what `TransportConfig` historically used (100ms initial,
/// 1600ms cap = 16x, ±25% jitter, unbounded attempts). Consumers can override
/// any field via designated initializers.
struct ReconnectPolicyConfig {
    /// @brief Backoff applied before the first retry attempt.
    std::chrono::milliseconds initial_backoff{100};

    /// @brief Hard cap on per-attempt backoff after exponential growth.
    /// Must be >= `initial_backoff`; validated by `ReconnectPolicy`'s ctor.
    std::chrono::milliseconds max_backoff{1600};

    /// @brief Exponential growth factor applied after each failed attempt.
    /// Must be > 1.0 (strictly positive growth). Default is classic doubling.
    double multiplier{2.0};

    /// @brief Symmetric jitter fraction. `0.25` means ±25% around the base.
    /// Must be in [0.0, 1.0). `0.0` disables jitter (deterministic timing).
    double jitter_factor{0.25};

    /// @brief Maximum number of retries before `should_reconnect()` goes false.
    /// `0` means "unlimited" — caller decides when to give up.
    uint32_t max_attempts{0};
};

// ---------------------------------------------------------------------------
// ReconnectPolicy
// ---------------------------------------------------------------------------

/// @brief Stateful exponential-backoff reconnection policy.
///
/// Intended usage:
///
///     ReconnectPolicy policy{cfg};
///     while (policy.should_reconnect()) {
///         auto delay = policy.next_backoff();
///         sleep_for(delay);
///         if (try_connect()) { policy.reset(); break; }
///     }
///
/// `next_backoff()` advances internal state: the first call returns (roughly)
/// `initial_backoff`, each subsequent call multiplies by `multiplier_` up to
/// `max_backoff_`, and increments `attempts_`. `reset()` rewinds to the
/// initial state after a successful reconnect.
///
/// Thread-safety: not internally synchronized. Each connection owns its own
/// policy.
class ReconnectPolicy {
public:
    /// @brief Construct a policy from the given config.
    ///
    /// Validation clamps obviously-bad values rather than returning an error,
    /// so that a zero-arg default-constructed config is always usable:
    ///   - `initial_backoff <= 0` is clamped to `kFallbackInitial` (100ms).
    ///     A non-positive initial backoff would feed the multiplier with a
    ///     non-positive base every call (-100ms × 2 = -200ms; std::min of
    ///     that and max_backoff stays negative when max_backoff defaults to
    ///     a positive cap), marching `current_base_` monotonically more
    ///     negative until `count() * multiplier` exceeds INT64 range and
    ///     the `static_cast<int64_t>` reinterpret is UB
    ///     (per [conv.fpint]). Zero is functionally a hot busy-loop —
    ///     same fix.
    ///   - `multiplier <= 1.0` is bumped to `2.0`
    ///   - `multiplier` non-finite (NaN/Inf) is bumped to `2.0`. Without
    ///     this guard, NaN slips past `<= 1.0` (NaN comparisons are
    ///     false), then `current_base_.count() * NaN = NaN` flows into
    ///     `static_cast<int64_t>(NaN)` inside `next_backoff` which is
    ///     UB per [conv.fpint]. +Inf has the symmetric problem: it
    ///     bypasses the bound and explodes the cast.
    ///   - `jitter_factor < 0` is clamped to `0`
    ///   - `jitter_factor` non-finite (NaN/Inf) is clamped to `0`. Same
    ///     class of NaN-slip bug — `apply_jitter` then hands NaN to
    ///     `std::uniform_real_distribution(NaN, NaN)` which is UB.
    ///   - `jitter_factor >= 1` is clamped to `0.999`
    ///   - `max_backoff < initial_backoff` is set to `initial_backoff`
    explicit ReconnectPolicy(ReconnectPolicyConfig cfg) noexcept
        : cfg_(cfg)
        , current_base_{}
        , attempts_(0)
    {
        constexpr std::chrono::milliseconds kFallbackInitial{100};
        if (cfg_.initial_backoff.count() <= 0) {
            cfg_.initial_backoff = kFallbackInitial;
        }
        // NaN-safe clamp: every comparison against NaN returns false,
        // so `<= 1.0` would NOT trigger on a NaN multiplier and the
        // poisoned value would reach next_backoff's float→int64 cast
        // (UB). isfinite catches NaN and ±Inf together; the legacy
        // `<= 1.0` then handles the finite-but-too-small case.
        if (!std::isfinite(cfg_.multiplier) || cfg_.multiplier <= 1.0) {
            cfg_.multiplier = 2.0;
        }
        // Same NaN-slip rationale for jitter_factor — apply_jitter calls
        // std::uniform_real_distribution(low, high) with low/high
        // computed from jitter_factor, and a NaN distribution is UB.
        if (!std::isfinite(cfg_.jitter_factor) || cfg_.jitter_factor < 0.0) {
            cfg_.jitter_factor = 0.0;
        }
        if (cfg_.jitter_factor >= 1.0) cfg_.jitter_factor = 0.999;
        if (cfg_.max_backoff < cfg_.initial_backoff) {
            cfg_.max_backoff = cfg_.initial_backoff;
        }
        current_base_ = cfg_.initial_backoff;
    }

    /// @brief Whether another attempt is permitted.
    ///
    /// Returns true unless `max_attempts > 0 && attempts_ >= max_attempts`.
    /// Callers loop on this; they should still bound their outer retry loop
    /// separately if max_attempts is unlimited (0).
    [[nodiscard]] bool should_reconnect() const noexcept {
        if (cfg_.max_attempts == 0) return true;
        return attempts_ < cfg_.max_attempts;
    }

    /// @brief Return the backoff duration for the NEXT attempt and advance.
    ///
    /// Side effects:
    ///   - increments `attempts_`
    ///   - multiplies `current_base_` by `multiplier_` (capped at `max_backoff_`)
    ///
    /// Return value is `current_base_` with ±`jitter_factor` applied. If
    /// `jitter_factor_ == 0` the result is exactly `current_base_`.
    [[nodiscard]] std::chrono::milliseconds next_backoff() noexcept {
        auto base = current_base_;
        ++attempts_;

        // Advance base for the NEXT call. This is intentional: the returned
        // value uses the pre-advance base, so the first call returns roughly
        // initial_backoff as documented.
        //
        // Saturated FP→int64 conversion: `current_base_.count() * multiplier`
        // is a double; once `current_base_` approaches INT64_MAX (e.g. when
        // `max_backoff = milliseconds::max()` and we doubled ~63 times), the
        // product exceeds 9.22e18 and `static_cast<int64_t>(huge)` becomes
        // UB per [conv.fpint] — implementations may return INT64_MIN, leak
        // a wrap to negative, or anything else. Clamp to INT64_MAX before
        // the cast so the cap-by-`std::min(next, max_backoff)` below sees
        // a well-defined value. This complements the non-positive
        // initial_backoff clamp in the ctor (commit aebf0622) — together
        // they cover both the negative-march and the positive-explode
        // overflow paths.
        const double scaled = current_base_.count() * cfg_.multiplier;
        constexpr double kInt64MaxAsDouble =
            static_cast<double>(std::numeric_limits<int64_t>::max());
        const int64_t scaled_int =
            (scaled >= kInt64MaxAsDouble)
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(scaled);
        auto next = std::chrono::milliseconds{scaled_int};
        current_base_ = std::min(next, cfg_.max_backoff);

        return apply_jitter(base);
    }

    /// @brief Reset attempt counter and backoff to the initial configured
    ///        state. Call after a successful connect.
    void reset() noexcept {
        attempts_ = 0;
        current_base_ = cfg_.initial_backoff;
    }

    /// @brief Current attempt count since construction or last `reset()`.
    [[nodiscard]] uint32_t attempts() const noexcept { return attempts_; }

    /// @brief Accessor for the post-clamp config (for debugging / tests).
    [[nodiscard]] const ReconnectPolicyConfig& config() const noexcept {
        return cfg_;
    }

private:
    /// @brief Apply symmetric jitter to a duration.
    ///
    /// Picks a uniformly random value in
    /// `[base * (1 - jf), base * (1 + jf))`. When `jitter_factor_ == 0`,
    /// returns `base` unchanged (deterministic). Negative / zero durations
    /// pass through unchanged.
    std::chrono::milliseconds apply_jitter(
        std::chrono::milliseconds base) noexcept
    {
        if (base.count() <= 0 || cfg_.jitter_factor == 0.0) return base;

        // Thread-local RNG: seeded with multiple entropy sources so that
        // simultaneous reconnects across threads do not lockstep. This matches
        // the legacy policy's behavior.
        thread_local std::mt19937_64 rng([] {
            std::seed_seq seeds{
                std::random_device{}(),
                std::random_device{}(),
                static_cast<unsigned>(
                    std::chrono::steady_clock::now().time_since_epoch().count())
            };
            return std::mt19937_64{seeds};
        }());

        auto count = static_cast<double>(base.count());
        auto low  = count * (1.0 - cfg_.jitter_factor);
        auto high = count * (1.0 + cfg_.jitter_factor);
        std::uniform_real_distribution<double> dist(low, high);
        const double sample = dist(rng);
        // Saturated FP→int64 conversion: when `current_base_` is at
        // saturation (INT64_MAX, see the `scaled` clamp in next_backoff)
        // the upper jitter end is `INT64_MAX * 1.25 ≈ 1.15e19`, which is
        // representable as a double but overflows int64_t. A naked
        // `static_cast<int64_t>(huge)` is UB per [conv.fpint] — typical
        // x86 GCC returns INT64_MIN, after which the `< 0` clamp below
        // turns the negative into 0 — defeating the backoff entirely
        // (the orchestrator then sees delay_ms <= 0 and schedules an
        // immediate retry, exactly opposite to the documented "saturate"
        // intent). Mirror the same kInt64MaxAsDouble clamp used in
        // next_backoff so both the `current_base_` advance and the
        // jitter draw share one saturating-cast policy.
        constexpr double kInt64MaxAsDouble =
            static_cast<double>(std::numeric_limits<int64_t>::max());
        constexpr double kInt64MinAsDouble =
            static_cast<double>(std::numeric_limits<int64_t>::min());
        int64_t picked;
        if (sample >= kInt64MaxAsDouble) {
            picked = std::numeric_limits<int64_t>::max();
        } else if (sample <= kInt64MinAsDouble) {
            picked = 0;  // negative-going drift → clamp to 0 (next branch handles)
        } else {
            picked = static_cast<int64_t>(sample);
        }
        // Never return a negative duration even if floating-point ever drifts.
        if (picked < 0) picked = 0;
        return std::chrono::milliseconds{picked};
    }

    ReconnectPolicyConfig cfg_;
    std::chrono::milliseconds current_base_;
    uint32_t attempts_;
};

} // namespace eph::net
