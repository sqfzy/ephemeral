#pragma once

/// @file risk_check.hpp
/// Pre-trade risk checks for order validation against configurable limits.
///
/// Provides stateless threshold comparisons for single-order qty/notional,
/// per-symbol position limits, aggregate exposure, and rate limiting.
/// Header-only, zero-allocation on the hot path.

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/fix/position.hpp"

namespace eph::fix {

/// @brief Internal implementation details for the risk check module.
namespace detail {
/// @brief Get or create the spdlog logger for risk checking.
/// @return Raw pointer to the "fix.risk_check" logger (never null after first call).
inline spdlog::logger* risk_check_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("fix.risk_check");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("fix.risk_check");
        }
    }();
    return l.get();
}
} // namespace detail

/// @brief Configurable risk thresholds.  A value of 0.0 (or 0) disables the check.
///
/// All thresholds are checked in order by RiskChecker::check_order(). A zero
/// value means the corresponding check is skipped (disabled).
struct RiskLimits {
    double max_order_qty         = 0.0;  ///< Max single order quantity.
    double max_order_notional    = 0.0;  ///< Max single order notional (qty * price).
    double max_position_qty      = 0.0;  ///< Max absolute position per symbol.
    double max_position_notional = 0.0;  ///< Max position notional per symbol.
    double max_total_exposure    = 0.0;  ///< Max total exposure across all symbols.
    int    max_orders_per_second = 0;    ///< Rate limit (0 = no limit).

    /// Validate limits, returning an error description or empty string on success.
    /// A value of 0.0 (or 0) disables the corresponding check, which is valid.
    /// Negative values are invalid.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (max_order_qty < 0.0)
            return "max_order_qty must be >= 0 (0 disables)";
        if (max_order_notional < 0.0)
            return "max_order_notional must be >= 0 (0 disables)";
        if (max_position_qty < 0.0)
            return "max_position_qty must be >= 0 (0 disables)";
        if (max_position_notional < 0.0)
            return "max_position_notional must be >= 0 (0 disables)";
        if (max_total_exposure < 0.0)
            return "max_total_exposure must be >= 0 (0 disables)";
        if (max_orders_per_second < 0)
            return "max_orders_per_second must be >= 0 (0 disables)";
        return {};
    }

    /// Multi-line formatted dump for logging/debugging.
    /// Zero-valued limits are shown as "disabled".
    [[nodiscard]] std::string dump() const {
        auto fmtd = [](double v) -> std::string {
            return v > 0.0 ? std::format("{:.2f}", v) : std::string("disabled");
        };
        auto fmti = [](int v) -> std::string {
            return v > 0 ? std::format("{}", v) : std::string("disabled");
        };
        return std::format(
            "RiskLimits:\n"
            "  max_order_qty: {}\n"
            "  max_order_notional: {}\n"
            "  max_position_qty: {}\n"
            "  max_position_notional: {}\n"
            "  max_total_exposure: {}\n"
            "  max_orders_per_second: {}",
            fmtd(max_order_qty), fmtd(max_order_notional),
            fmtd(max_position_qty), fmtd(max_position_notional),
            fmtd(max_total_exposure), fmti(max_orders_per_second));
    }

    /// JSON-formatted limits for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"max_order_qty\":{:.6g},\"max_order_notional\":{:.6g},"
            "\"max_position_qty\":{:.6g},\"max_position_notional\":{:.6g},"
            "\"max_total_exposure\":{:.6g},\"max_orders_per_second\":{}}}",
            max_order_qty, max_order_notional,
            max_position_qty, max_position_notional,
            max_total_exposure, max_orders_per_second);
    }

    /// Check for non-fatal contradictions or likely misconfigurations.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        // All limits disabled means no risk protection
        if (max_order_qty <= 0.0 && max_order_notional <= 0.0 &&
            max_position_qty <= 0.0 && max_position_notional <= 0.0 &&
            max_total_exposure <= 0.0 && max_orders_per_second <= 0) {
            w.emplace_back("all risk limits are disabled (0) -- "
                           "no pre-trade risk protection is active");
        }
        // Order notional without position limit
        if (max_order_notional > 0.0 && max_total_exposure <= 0.0)
            w.emplace_back("max_order_notional is set but max_total_exposure "
                           "is disabled -- aggregate exposure is unchecked");
        // Position qty without order qty
        if (max_position_qty > 0.0 && max_order_qty <= 0.0)
            w.emplace_back("max_position_qty is set but max_order_qty is "
                           "disabled -- a single large order could fill the "
                           "entire position limit");
        return w;
    }

    /// Defaulted equality -- all fields must match exactly.
    [[nodiscard]] friend bool operator==(const RiskLimits&,
                                         const RiskLimits&) = default;
};

/// @brief Reason an order was rejected by the risk checker.
enum class RiskRejectReason : uint8_t {
    kOk = 0,                    ///< Order passed all risk checks.
    kOrderQtyExceeded,          ///< Single order quantity exceeds max_order_qty.
    kOrderNotionalExceeded,     ///< Single order notional exceeds max_order_notional.
    kPositionQtyExceeded,       ///< Projected position quantity exceeds max_position_qty.
    kPositionNotionalExceeded,  ///< Projected position notional exceeds max_position_notional.
    kTotalExposureExceeded,     ///< Projected total exposure exceeds max_total_exposure.
    kRateLimitExceeded,         ///< Order rate exceeds max_orders_per_second.
    kInvalidInput,              ///< Non-finite, zero, or negative qty/price.
};

/// @brief Human-readable name for a reject reason.
/// @param r  The reject reason to convert.
/// @return A string_view description of the reason.
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
        case RiskRejectReason::kInvalidInput:             return "InvalidInput";
    }
    return "Unknown";
}

/// @brief Pre-trade risk checker.
///
/// Validates orders against configurable limits before sending.
/// Thread-safety: none -- same single-threaded assumption as PositionTracker.
class RiskChecker {
public:
    /// @brief Construct a risk checker with the given thresholds.
    /// @param limits  Risk thresholds to enforce (zero values disable individual checks).
    explicit RiskChecker(RiskLimits limits) noexcept
        : limits_(limits)
    {
        SPDLOG_LOGGER_DEBUG(detail::risk_check_logger(), "RiskChecker created: max_order_qty={} max_order_notional={} "
                     "max_position_qty={} max_position_notional={} "
                     "max_total_exposure={} max_orders_per_second={}",
                     limits_.max_order_qty, limits_.max_order_notional,
                     limits_.max_position_qty, limits_.max_position_notional,
                     limits_.max_total_exposure, limits_.max_orders_per_second);
    }

    /// @brief Check if an order passes all risk limits.
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
        SPDLOG_LOGGER_DEBUG(detail::risk_check_logger(), "check_order: symbol={} side={} qty={} price={}",
                     symbol, side, qty, price);

        // Reject non-finite inputs — NaN/Inf would bypass all comparisons.
        if (!std::isfinite(qty) || !std::isfinite(price) ||
            qty <= 0.0 || price <= 0.0) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::risk_check_logger(), "risk reject: invalid qty={} or price={} for symbol={}",
                        qty, price, symbol);
            return RiskRejectReason::kInvalidInput;
        }

        // Validate Side: only FIX 4.4 codes '1' (Buy) and '2' (Sell) are
        // legal. Without this guard, the ternary `(side == '1') ? qty : -qty`
        // silently projects any other byte (Binance-native 'B'/'S', NUL,
        // garbage) as Sell, which can hide a real Buy-direction breach of
        // max_position_qty / max_total_exposure. PositionTracker::on_fill
        // already rejects unknown sides — the matching invariant must hold
        // here so the projected position used by check_order does not drift
        // away from the on_fill state machine.
        if (side != '1' && side != '2') [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::risk_check_logger(),
                "risk reject: invalid side='{}' (0x{:02x}) for symbol={} — "
                "must be FIX '1' (Buy) or '2' (Sell)",
                side, static_cast<uint8_t>(side), symbol);
            return RiskRejectReason::kInvalidInput;
        }

        const double notional = qty * price;

        // 1. Single order quantity check.
        if (limits_.max_order_qty > 0.0 && qty > limits_.max_order_qty) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::risk_check_logger(), "risk reject: order qty {} exceeds limit {} for symbol={}",
                        qty, limits_.max_order_qty, symbol);
            return RiskRejectReason::kOrderQtyExceeded;
        }

        // 2. Single order notional check.
        if (limits_.max_order_notional > 0.0 && notional > limits_.max_order_notional) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::risk_check_logger(), "risk reject: order notional {} exceeds limit {} for symbol={}",
                        notional, limits_.max_order_notional, symbol);
            return RiskRejectReason::kOrderNotionalExceeded;
        }

        // 3. Per-symbol position quantity check (post-fill projection).
        if (limits_.max_position_qty > 0.0) {
            const auto& pos = positions.get(symbol);
            const double signed_qty = (side == '1') ? qty : -qty;
            const double projected_qty = std::abs(pos.qty + signed_qty);
            if (projected_qty > limits_.max_position_qty) [[unlikely]] {
                SPDLOG_LOGGER_WARN(detail::risk_check_logger(), "risk reject: projected position qty {} exceeds limit {} "
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
            if (projected_notional > limits_.max_position_notional) [[unlikely]] {
                SPDLOG_LOGGER_WARN(detail::risk_check_logger(), "risk reject: projected position notional {} exceeds limit {} "
                            "for symbol={}", projected_notional, limits_.max_position_notional,
                            symbol);
                return RiskRejectReason::kPositionNotionalExceeded;
            }
        }

        // 5. Total exposure check: compute projected post-trade exposure.
        //    A sell against a long (or buy against a short) reduces exposure,
        //    so we project the symbol's new notional rather than blindly adding.
        if (limits_.max_total_exposure > 0.0) {
            const double current_exposure = positions.net_exposure();
            const auto&  pos              = positions.get(symbol);
            const double signed_qty       = (side == '1') ? qty : -qty;
            const double projected_abs_qty = std::abs(pos.qty + signed_qty);
            // Use order price as best estimate for projected notional.
            const double projected_symbol_notional = projected_abs_qty * price;
            // Recompute current symbol notional from live qty/avg_price to
            // avoid relying on pos.notional which may be stale.
            const double current_notional = std::isfinite(pos.avg_price)
                ? std::abs(pos.qty) * pos.avg_price
                : pos.notional;  // Fallback to cached value if avg_price corrupt
            // Replace this symbol's current notional with its projected notional.
            const double projected_exposure =
                current_exposure - current_notional + projected_symbol_notional;
            // Fail-safe NaN/inf guard. Both `current_notional` and
            // `projected_symbol_notional` are computed via floating-
            // point multiplications that can overflow to +inf when
            // pos.qty * pos.avg_price or projected_abs_qty * price
            // exceeds ~1e308, and `current_exposure - inf` can wrap
            // to NaN. Without this guard `NaN > limit` returns false
            // and the order PASSES the exposure check — exactly the
            // wrong direction for a fail-safe risk gate. Treat any
            // non-finite projected exposure as a hard reject (the
            // numbers feeding it are pathological anyway; better to
            // surface the data-quality issue at the trade gate than
            // let the rocket fly).
            if (!std::isfinite(projected_exposure)) [[unlikely]] {
                SPDLOG_LOGGER_WARN(detail::risk_check_logger(),
                    "risk reject: projected total exposure non-finite ({}) "
                    "for symbol={} (current={} current_notional={} "
                    "projected_symbol_notional={}) — overflow in "
                    "qty*price math, treating as exposure breach",
                    projected_exposure, symbol, current_exposure,
                    current_notional, projected_symbol_notional);
                return RiskRejectReason::kTotalExposureExceeded;
            }
            if (projected_exposure > limits_.max_total_exposure) [[unlikely]] {
                // Log `current_notional` (the live recompute that drives the
                // projected_exposure math), not `pos.notional` — the cached
                // copy can lag the live qty/avg_price when the snapshot is
                // taken between fills. Reporting the stale value here would
                // make the rejection arithmetic look inconsistent in the log.
                SPDLOG_LOGGER_WARN(detail::risk_check_logger(), "risk reject: projected total exposure {} "
                            "(current={}, symbol {} notional {} -> {}) exceeds limit {}",
                            projected_exposure, current_exposure, symbol,
                            current_notional, projected_symbol_notional,
                            limits_.max_total_exposure);
                return RiskRejectReason::kTotalExposureExceeded;
            }
        }

        // 6. Rate limit check is left to the caller to implement with a
        //    timestamp-based sliding window.  This stateless checker only
        //    validates the threshold field is available for external use.
        //    (A stateful rate limiter would require clock injection for testability.)

        SPDLOG_LOGGER_DEBUG(detail::risk_check_logger(), "check_order: symbol={} passed all risk checks", symbol);
        return RiskRejectReason::kOk;
    }

    /// @brief Update limits at runtime (e.g. from a control channel).
    /// @param limits  New risk thresholds to apply immediately.
    void set_limits(RiskLimits limits) noexcept
    {
        limits_ = limits;
        SPDLOG_LOGGER_DEBUG(detail::risk_check_logger(), "RiskChecker::set_limits updated");
    }

    /// @brief Access current risk limits (read-only).
    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }

private:
    RiskLimits limits_;
};

}  // namespace eph::fix

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::fix::RiskLimits> : std::formatter<std::string> {
    auto format(const eph::fix::RiskLimits& r, auto& ctx) const {
        return std::formatter<std::string>::format(r.dump(), ctx);
    }
};

template <>
struct std::formatter<eph::fix::RiskRejectReason> : std::formatter<std::string_view> {
    auto format(eph::fix::RiskRejectReason r, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::fix::risk_reject_name(r), ctx);
    }
};
