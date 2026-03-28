#pragma once

/// @file risk_check.hpp
/// Pre-trade risk checks for order validation against configurable limits.
///
/// Provides stateless threshold comparisons for single-order qty/notional,
/// per-symbol position limits, aggregate exposure, and rate limiting.
/// Header-only, zero-allocation on the hot path.

#include <cmath>
#include <cstdint>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/fix/position.hpp"

namespace eph::fix {

/// Configurable risk thresholds.  A value of 0.0 (or 0) disables the check.
struct RiskLimits {
    double max_order_qty         = 0.0;  ///< Max single order quantity.
    double max_order_notional    = 0.0;  ///< Max single order notional (qty * price).
    double max_position_qty      = 0.0;  ///< Max absolute position per symbol.
    double max_position_notional = 0.0;  ///< Max position notional per symbol.
    double max_total_exposure    = 0.0;  ///< Max total exposure across all symbols.
    int    max_orders_per_second = 0;    ///< Rate limit (0 = no limit).
};

/// Reason an order was rejected by the risk checker.
enum class RiskRejectReason : uint8_t {
    kOk = 0,
    kOrderQtyExceeded,
    kOrderNotionalExceeded,
    kPositionQtyExceeded,
    kPositionNotionalExceeded,
    kTotalExposureExceeded,
    kRateLimitExceeded,
};

/// Human-readable name for a reject reason.
constexpr std::string_view risk_reject_name(RiskRejectReason r) noexcept
{
    switch (r) {
        case RiskRejectReason::kOk:                       return "Ok";
        case RiskRejectReason::kOrderQtyExceeded:         return "OrderQtyExceeded";
        case RiskRejectReason::kOrderNotionalExceeded:    return "OrderNotionalExceeded";
        case RiskRejectReason::kPositionQtyExceeded:      return "PositionQtyExceeded";
        case RiskRejectReason::kPositionNotionalExceeded: return "PositionNotionalExceeded";
        case RiskRejectReason::kTotalExposureExceeded:    return "TotalExposureExceeded";
        case RiskRejectReason::kRateLimitExceeded:        return "RateLimitExceeded";
    }
    return "Unknown";
}

/// Pre-trade risk checker.
///
/// Validates orders against configurable limits before sending.
/// Thread-safety: none -- same single-threaded assumption as PositionTracker.
class RiskChecker {
public:
    explicit RiskChecker(RiskLimits limits) noexcept
        : limits_(limits)
    {
        SPDLOG_DEBUG("RiskChecker created: max_order_qty={} max_order_notional={} "
                     "max_position_qty={} max_position_notional={} "
                     "max_total_exposure={} max_orders_per_second={}",
                     limits_.max_order_qty, limits_.max_order_notional,
                     limits_.max_position_qty, limits_.max_position_notional,
                     limits_.max_total_exposure, limits_.max_orders_per_second);
    }

    /// Check if an order passes all risk limits.
    ///
    /// @param symbol     Instrument identifier.
    /// @param side       FIX Side: '1' = Buy, '2' = Sell.
    /// @param qty        Order quantity (must be > 0).
    /// @param price      Order price (must be > 0).
    /// @param positions  Current position state for exposure checks.
    /// @return kOk if the order is allowed, or the first violated limit.
    [[nodiscard]] RiskRejectReason check_order(
        std::string_view symbol,
        char side,
        double qty,
        double price,
        const PositionTracker& positions) const noexcept
    {
        SPDLOG_DEBUG("check_order: symbol={} side={} qty={} price={}",
                     symbol, side, qty, price);

        const double notional = qty * price;

        // 1. Single order quantity check.
        if (limits_.max_order_qty > 0.0 && qty > limits_.max_order_qty) {
            SPDLOG_WARN("risk reject: order qty {} exceeds limit {} for symbol={}",
                        qty, limits_.max_order_qty, symbol);
            return RiskRejectReason::kOrderQtyExceeded;
        }

        // 2. Single order notional check.
        if (limits_.max_order_notional > 0.0 && notional > limits_.max_order_notional) {
            SPDLOG_WARN("risk reject: order notional {} exceeds limit {} for symbol={}",
                        notional, limits_.max_order_notional, symbol);
            return RiskRejectReason::kOrderNotionalExceeded;
        }

        // 3. Per-symbol position quantity check (post-fill projection).
        if (limits_.max_position_qty > 0.0) {
            const auto& pos = positions.get(symbol);
            const double signed_qty = (side == '1') ? qty : -qty;
            const double projected_qty = std::abs(pos.qty + signed_qty);
            if (projected_qty > limits_.max_position_qty) {
                SPDLOG_WARN("risk reject: projected position qty {} exceeds limit {} "
                            "for symbol={}", projected_qty, limits_.max_position_qty, symbol);
                return RiskRejectReason::kPositionQtyExceeded;
            }
        }

        // 4. Per-symbol position notional check (post-fill projection).
        if (limits_.max_position_notional > 0.0) {
            const auto& pos = positions.get(symbol);
            const double signed_qty = (side == '1') ? qty : -qty;
            const double projected_qty = std::abs(pos.qty + signed_qty);
            // Use order price as best estimate for projected notional.
            const double projected_notional = projected_qty * price;
            if (projected_notional > limits_.max_position_notional) {
                SPDLOG_WARN("risk reject: projected position notional {} exceeds limit {} "
                            "for symbol={}", projected_notional, limits_.max_position_notional,
                            symbol);
                return RiskRejectReason::kPositionNotionalExceeded;
            }
        }

        // 5. Total exposure check (current exposure + this order's notional contribution).
        if (limits_.max_total_exposure > 0.0) {
            const double current_exposure = positions.net_exposure();
            // Conservative: add order notional to current exposure.
            // A reducing order may lower exposure, but we use worst-case for safety.
            if (current_exposure + notional > limits_.max_total_exposure) {
                SPDLOG_WARN("risk reject: total exposure {} + order notional {} "
                            "exceeds limit {}",
                            current_exposure, notional, limits_.max_total_exposure);
                return RiskRejectReason::kTotalExposureExceeded;
            }
        }

        // 6. Rate limit check is left to the caller to implement with a
        //    timestamp-based sliding window.  This stateless checker only
        //    validates the threshold field is available for external use.
        //    (A stateful rate limiter would require clock injection for testability.)

        SPDLOG_DEBUG("check_order: symbol={} passed all risk checks", symbol);
        return RiskRejectReason::kOk;
    }

    /// Update limits at runtime (e.g. from a control channel).
    void set_limits(RiskLimits limits) noexcept
    {
        limits_ = limits;
        SPDLOG_DEBUG("RiskChecker::set_limits updated");
    }

    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }

private:
    RiskLimits limits_;
};

}  // namespace eph::fix
