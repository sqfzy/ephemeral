#pragma once

/// @file position.hpp
/// Per-symbol position tracker for pre-trade risk management.
///
/// Tracks quantity, volume-weighted average entry price, realized PnL,
/// and notional exposure per symbol.  Computes realized PnL on reducing
/// fills using the average-cost method.  Header-only, no allocations on
/// the hot path after warm-up.

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace eph::fix {

/// @brief Internal implementation details for the position module.
namespace detail {
/// @brief Get or create the spdlog logger for position tracking.
/// @return Raw pointer to the "fix.position" logger (never null after first call).
inline spdlog::logger* position_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("fix.position");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("fix.position");
        }
    }();
    return l.get();
}
} // namespace detail

/// @brief Snapshot of a single symbol's position state.
///
/// All fields are updated atomically on each fill by PositionTracker::on_fill().
struct Position {
    double   qty          = 0.0;  ///< Signed quantity: positive = long, negative = short.
    double   avg_price    = 0.0;  ///< Volume-weighted average entry price.
    double   realized_pnl = 0.0;  ///< Cumulative realized PnL from closed portions.
    double   notional     = 0.0;  ///< abs(qty) * avg_price.
    uint64_t trade_count  = 0;    ///< Number of fills processed.
};

/// @brief Tracks per-symbol positions and computes PnL.
///
/// Thread-safety: none -- callers must serialize access externally.
/// This is intentional: the expected use-case is single-threaded
/// order-event processing where external locking would add latency.
class PositionTracker {
public:
    /// @brief Transparent hash for heterogeneous lookup (string_view key without allocation).
    struct StringHash {
        using is_transparent = void;  ///< Enable heterogeneous lookup in unordered containers.
        /// @brief Hash a string_view.
        size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
        /// @brief Hash a std::string (via string_view for consistency).
        size_t operator()(const std::string& s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    /// @brief Record a fill.
    ///
    /// @param symbol  Instrument identifier.
    /// @param side    FIX Side: '1' = Buy, '2' = Sell.
    /// @param qty     Fill quantity (must be > 0).
    /// @param price   Fill price   (must be > 0).
    ///
    /// On a reducing fill (e.g. sell when long), realized PnL is computed as:
    ///   longs:  (price - avg_price) * reduced_qty
    ///   shorts: (avg_price - price) * reduced_qty
    /// When a fill crosses zero (e.g. long 5, sell 8), the closing and
    /// opening portions are handled separately.
    void on_fill(std::string_view symbol, char side,
                 double qty, double price) noexcept
    {
        // Validate inputs -- reject nonsensical fills.
        if (!std::isfinite(price)) {
            SPDLOG_LOGGER_WARN(detail::position_logger(), "on_fill: non-finite price rejected symbol={} side={} qty={} price={}",
                        symbol, side, qty, price);
            return;
        }
        if (qty <= 0.0 || price <= 0.0) {
            SPDLOG_LOGGER_WARN(detail::position_logger(), "on_fill: ignoring invalid fill symbol={} side={} qty={} price={}",
                        symbol, side, qty, price);
            return;
        }
        if (side != '1' && side != '2') {
            SPDLOG_LOGGER_WARN(detail::position_logger(), "on_fill: ignoring unknown side={} for symbol={}", side, symbol);
            return;
        }

        // Signed fill qty: positive for buys, negative for sells.
        const double signed_qty = (side == '1') ? qty : -qty;

        // Heterogeneous lookup: no string allocation when symbol already exists.
        // operator[] requires the key type to be constructible from string_view,
        // so we use find + emplace for the insert path.
        auto it = positions_.find(symbol);
        if (it == positions_.end()) {
            it = positions_.emplace(std::string(symbol), Position{}).first;
        }
        auto& pos = it->second;
        ++pos.trade_count;

        const double old_qty = pos.qty;
        const double new_qty = old_qty + signed_qty;

        // Determine if this fill reduces the existing position.
        // A fill reduces when it has the opposite sign of the current position.
        const bool is_reducing = (old_qty != 0.0) &&
                                 ((old_qty > 0.0) != (signed_qty > 0.0));

        if (!is_reducing) {
            // Pure increase (or first fill): update average price.
            // Use incremental formula to avoid catastrophic cancellation:
            //   avg_price += (price - avg_price) * (|signed_qty| / |new_qty|)
            // When old_qty == 0 this simplifies to avg_price = price.
            if (new_qty != 0.0) {
                const double delta = price - pos.avg_price;
                pos.avg_price += delta * (std::abs(signed_qty) / std::abs(new_qty));
            }
            // Guard against non-finite results from degenerate inputs.
            if (!std::isfinite(pos.avg_price)) {
                SPDLOG_LOGGER_WARN(detail::position_logger(), "on_fill: symbol={} avg_price became non-finite, "
                            "resetting to fill price={}", symbol, price);
                pos.avg_price = price;
            }
        } else {
            // Reducing fill -- may partially or fully close, or cross zero.
            const double close_qty = std::min(std::abs(signed_qty), std::abs(old_qty));

            // Realized PnL on the closed portion.
            if (!std::isfinite(pos.avg_price)) {
                SPDLOG_LOGGER_WARN(detail::position_logger(), "on_fill: avg_price non-finite for symbol={}, "
                            "resetting to fill price and skipping PnL calc", symbol);
                pos.avg_price = price;
            } else if (old_qty > 0.0) {
                // Was long: profit = (sell_price - avg) * close_qty
                pos.realized_pnl += (price - pos.avg_price) * close_qty;
            } else {
                // Was short: profit = (avg - buy_price) * close_qty
                pos.realized_pnl += (pos.avg_price - price) * close_qty;
            }

            SPDLOG_LOGGER_DEBUG(detail::position_logger(), "on_fill: symbol={} closed qty={} realized_pnl={}",
                         symbol, close_qty, pos.realized_pnl);

            // If the fill crosses zero, the remainder opens a new position
            // at the fill price.
            if (std::abs(signed_qty) > std::abs(old_qty)) {
                pos.avg_price = price;
            }
            // If exactly flat or partially closed, avg_price stays the same.

            // Guard against non-finite results from degenerate inputs.
            if (!std::isfinite(pos.avg_price)) {
                SPDLOG_LOGGER_WARN(detail::position_logger(), "on_fill: symbol={} avg_price became non-finite after "
                            "reducing fill, resetting to fill price={}", symbol, price);
                pos.avg_price = price;
            }
        }

        pos.qty      = new_qty;
        pos.notional = std::abs(new_qty) * pos.avg_price;

        SPDLOG_LOGGER_DEBUG(detail::position_logger(), "on_fill: symbol={} side={} fill_qty={} price={} "
                     "pos_qty={} avg_price={} notional={} realized_pnl={}",
                     symbol, side, qty, price,
                     pos.qty, pos.avg_price, pos.notional, pos.realized_pnl);
    }

    /// @brief Get position for a symbol.  Returns a static zero-initialized
    /// Position if the symbol has never been seen.
    [[nodiscard]] const Position& get(std::string_view symbol) const noexcept
    {
        static const Position zero{};
        auto it = positions_.find(symbol);  // heterogeneous lookup — no allocation
        if (it == positions_.end()) return zero;
        return it->second;
    }

    /// @brief Check if a symbol has an open (non-zero) position.
    /// @param symbol  Instrument identifier to check.
    [[nodiscard]] bool has_position(std::string_view symbol) const noexcept
    {
        auto it = positions_.find(symbol);  // heterogeneous lookup — no allocation
        return it != positions_.end() && it->second.qty != 0.0;
    }

    /// @brief Access the full position map.
    [[nodiscard]] const std::unordered_map<std::string, Position, StringHash, std::equal_to<>>&
    positions() const noexcept { return positions_; }

    /// @brief Total unrealized PnL across all positions given current market prices.
    ///
    /// For longs:  (market_price - avg_price) * qty
    /// For shorts: (avg_price - market_price) * abs(qty)
    [[nodiscard]] double total_unrealized_pnl(
        const std::unordered_map<std::string, double>& market_prices) const noexcept
    {
        double total = 0.0;
        for (const auto& [sym, pos] : positions_) {
            if (pos.qty == 0.0) continue;
            auto it = market_prices.find(sym);
            if (it == market_prices.end()) {
                SPDLOG_LOGGER_WARN(detail::position_logger(), "total_unrealized_pnl: no market price for symbol={}", sym);
                continue;
            }
            // Unrealized = (market - avg) * qty  (works for both signs).
            total += (it->second - pos.avg_price) * pos.qty;
        }
        return total;
    }

    /// @brief Sum of realized PnL across all symbols.
    [[nodiscard]] double total_realized_pnl() const noexcept
    {
        double total = 0.0;
        for (const auto& [_, pos] : positions_) {
            total += pos.realized_pnl;
        }
        return total;
    }

    /// @brief Gross notional exposure: sum of abs(notional) across all positions.
    [[nodiscard]] double net_exposure() const noexcept
    {
        double total = 0.0;
        for (const auto& [_, pos] : positions_) {
            total += pos.notional;  // notional is already abs(qty) * avg_price
        }
        return total;
    }

    /// @brief Reset all tracked positions.
    void clear() noexcept
    {
        positions_.clear();
        SPDLOG_LOGGER_DEBUG(detail::position_logger(), "PositionTracker::clear: all positions reset");
    }

private:
    std::unordered_map<std::string, Position, StringHash, std::equal_to<>> positions_;
};

}  // namespace eph::fix
