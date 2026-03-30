#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace eph::utils {

/// Exponential Moving Average — O(1) per update, zero allocation.
/// Used for smoothing noisy HFT signals (prices, imbalances, latencies).
///
/// Formula: ema = alpha * value + (1 - alpha) * prev_ema
/// First value initializes directly (no warm-up period).
class Ema {
  public:
    /// @param alpha Smoothing factor in (0, 1]. Larger = more responsive.
    ///              For period N: alpha = 2/(N+1).
    explicit Ema(double alpha) : alpha_(alpha) {
        if (alpha <= 0.0 || alpha > 1.0) {
            throw std::invalid_argument("EMA alpha must be in (0.0, 1.0]");
        }
    }

    /// Create from period (number of samples). alpha = 2/(period+1).
    /// @param period Must be >= 1.
    [[nodiscard]] static Ema from_period(std::size_t period) {
        if (period < 1) {
            throw std::invalid_argument("EMA period must be >= 1");
        }
        return Ema(2.0 / static_cast<double>(period + 1));
    }

    /// Update with a new value. Returns the new EMA.
    /// First call seeds the EMA directly with `value` (no warm-up).
    [[nodiscard]] double update(double value) noexcept {
        if (!initialized_) [[unlikely]] {
            value_ = value;
            initialized_ = true;
            SPDLOG_DEBUG("EMA initialized with seed value={:.6f}, alpha={:.4f}",
                         value, alpha_);
        } else {
            value_ = alpha_ * value + (1.0 - alpha_) * value_;
            SPDLOG_TRACE("EMA update: input={:.6f}, ema={:.6f}", value, value_);
        }
        return value_;
    }

    /// Current EMA value. Only meaningful after at least one update().
    [[nodiscard]] double value() const noexcept { return value_; }

    /// Whether at least one value has been recorded.
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    /// Smoothing factor.
    [[nodiscard]] constexpr double alpha() const noexcept { return alpha_; }

    /// Reset to uninitialized state.
    void reset() noexcept {
        value_ = 0.0;
        initialized_ = false;
        SPDLOG_DEBUG("EMA reset (alpha={:.4f})", alpha_);
    }

  private:
    double alpha_;
    double value_ = 0.0;
    bool initialized_ = false;
};

/// Dual EMA crossover detector — generates signals when fast EMA crosses slow EMA.
///
/// A bullish (golden) cross occurs when the fast EMA crosses above the slow EMA.
/// A bearish (death) cross occurs when the fast EMA crosses below the slow EMA.
/// Both EMAs must be initialized before any signal can be generated.
class EmaCrossover {
  public:
    enum class Signal : int8_t {
        None = 0,
        BullishCross = 1,   ///< Fast crossed above slow.
        BearishCross = -1,  ///< Fast crossed below slow.
    };

    /// @param fast_period Period for the fast (responsive) EMA. Must be >= 1.
    /// @param slow_period Period for the slow (smooth) EMA. Must be >= fast_period.
    EmaCrossover(std::size_t fast_period, std::size_t slow_period)
        : fast_(Ema::from_period(fast_period)),
          slow_(Ema::from_period(slow_period)) {
        if (fast_period > slow_period) {
            throw std::invalid_argument(
                "fast_period should be <= slow_period");
        }
        SPDLOG_DEBUG(
            "EmaCrossover created: fast_period={}, slow_period={}, "
            "fast_alpha={:.4f}, slow_alpha={:.4f}",
            fast_period, slow_period, fast_.alpha(), slow_.alpha());
    }

    /// Update with a new price. Returns crossover signal (if any).
    /// Both EMAs must have been initialized (i.e., at least one prior update)
    /// before a crossover can be detected, so the first call always returns None.
    [[nodiscard]] Signal update(double price) noexcept {
        const double prev_fast = fast_.value();
        const double prev_slow = slow_.value();
        const bool was_initialized =
            fast_.initialized() && slow_.initialized();

        (void)fast_.update(price);
        (void)slow_.update(price);

        if (!was_initialized) [[unlikely]] {
            SPDLOG_DEBUG("EmaCrossover: seeded with price={:.6f}", price);
            return Signal::None;
        }

        // Detect crossing: compare previous relative position to current.
        const bool was_above = prev_fast >= prev_slow;
        const bool now_above = fast_.value() >= slow_.value();

        if (was_above && !now_above) {
            SPDLOG_DEBUG(
                "EmaCrossover: bearish cross at price={:.6f} "
                "(fast={:.6f}, slow={:.6f})",
                price, fast_.value(), slow_.value());
            return Signal::BearishCross;
        }
        if (!was_above && now_above) {
            SPDLOG_DEBUG(
                "EmaCrossover: bullish cross at price={:.6f} "
                "(fast={:.6f}, slow={:.6f})",
                price, fast_.value(), slow_.value());
            return Signal::BullishCross;
        }

        return Signal::None;
    }

    /// Current fast EMA value.
    [[nodiscard]] double fast() const noexcept { return fast_.value(); }

    /// Current slow EMA value.
    [[nodiscard]] double slow() const noexcept { return slow_.value(); }

  private:
    Ema fast_;
    Ema slow_;
};

}  // namespace eph::utils
