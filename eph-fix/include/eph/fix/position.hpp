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
#include <format>
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
            auto recovered = spdlog::get("fix.position");
            return recovered ? recovered : spdlog::default_logger();
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

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "Position(qty={:.4f}, avg_price={:.6f}, realized_pnl={:.2f}, "
            "notional={:.2f}, trades={})",
            qty, avg_price, realized_pnl, notional, trade_count);
    }

    /// JSON-formatted position for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"qty\":{:.6g},\"avg_price\":{:.6g},\"realized_pnl\":{:.6g},"
            "\"notional\":{:.6g},\"trade_count\":{}}}",
            qty, avg_price, realized_pnl, notional, trade_count);
    }

    /// Defaulted equality -- all fields must match exactly.
    [[nodiscard]] friend bool operator==(const Position&,
                                         const Position&) = default;
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
        // The isfinite() guards must run BEFORE the `qty <= 0.0` /
        // `price <= 0.0` comparisons, because every comparison against NaN
        // returns false: a NaN qty/price would slip past `qty <= 0.0` and
        // poison every downstream field (qty, avg_price, notional,
        // realized_pnl). +inf qty also satisfies `qty > 0.0` and would
        // produce nonsense PnL once close_qty clamps it to a finite
        // old_qty inside the reducing-fill branch.
        if (!std::isfinite(qty) || !std::isfinite(price)) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::position_logger(),
                "on_fill: non-finite qty/price rejected symbol={} side={} qty={} price={}",
                symbol, side, qty, price);
            return;
        }
        if (qty <= 0.0 || price <= 0.0) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::position_logger(), "on_fill: ignoring invalid fill symbol={} side={} qty={} price={}",
                        symbol, side, qty, price);
            return;
        }
        if (side != '1' && side != '2') [[unlikely]] {
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
            if (!std::isfinite(pos.avg_price)) [[unlikely]] {
                SPDLOG_LOGGER_WARN(detail::position_logger(), "on_fill: symbol={} avg_price became non-finite, "
                            "resetting to fill price={}", symbol, price);
                pos.avg_price = price;
            }
        } else {
            // Reducing fill -- may partially or fully close, or cross zero.
            const double close_qty = std::min(std::abs(signed_qty), std::abs(old_qty));

            // Realized PnL on the closed portion.
            if (!std::isfinite(pos.avg_price)) [[unlikely]] {
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
            // Defense-in-depth: a NaN/+-Inf market price (corrupt feed,
            // stale snapshot, deserialiser bug) would propagate NaN into
            // `total` and stay there for every subsequent symbol — the
            // observability call returns NaN and downstream PnL
            // dashboards silently lose the entire portfolio's number.
            // Skip the bad symbol with a WARN so the rest of the
            // portfolio still aggregates cleanly. (Symmetric to the
            // isfinite guard inside on_fill which prevents NaN from
            // entering pos.avg_price in the first place.)
            if (!std::isfinite(it->second)) [[unlikely]] {
                SPDLOG_LOGGER_WARN(detail::position_logger(),
                    "total_unrealized_pnl: non-finite market price={} "
                    "for symbol={}, skipping (would poison the running "
                    "total with NaN)", it->second, sym);
                continue;
            }
            // Unrealized = (market - avg) * qty  (works for both signs).
            total += (it->second - pos.avg_price) * pos.qty;
        }
        return total;
    }

    /// @brief Sum of realized PnL across all symbols.
    ///
    /// Symmetric NaN/Inf defense to `total_unrealized_pnl` above:
    /// `on_fill` guards against non-finite avg_price entering the
    /// running PnL recurrence, so under normal flow `pos.realized_pnl`
    /// is finite. But a future code path that exposes Position
    /// mutability (deserialised position restore, snapshot reload)
    /// could leak NaN into one symbol's realized_pnl, and the naïve
    /// sum would propagate that NaN into `total` and stay there for
    /// every subsequent symbol — the dashboard read returns NaN and
    /// the entire portfolio's PnL silently disappears. Skip with WARN
    /// instead so the rest of the portfolio still aggregates cleanly.
    [[nodiscard]] double total_realized_pnl() const noexcept
    {
        double total = 0.0;
        for (const auto& [sym, pos] : positions_) {
            if (!std::isfinite(pos.realized_pnl)) [[unlikely]] {
                SPDLOG_LOGGER_WARN(detail::position_logger(),
                    "total_realized_pnl: non-finite realized_pnl={} for "
                    "symbol={}, skipping (would poison the running total "
                    "with NaN)", pos.realized_pnl, sym);
                continue;
            }
            total += pos.realized_pnl;
        }
        return total;
    }

    /// @brief Gross notional exposure across all positions.
    ///
    /// Returns sum of abs(qty) * avg_price across every tracked symbol — i.e.
    /// the **gross** measure used for risk-limit checks (`risk_check.hpp`
    /// `max_total_exposure`), not signed net exposure.
    ///
    /// The legacy name is preserved for source-compat with existing call
    /// sites and tests. Despite "net" in the name, longs and shorts both
    /// add positively to this total. If you need signed net (Σ qty *
    /// avg_price), iterate `positions()` directly.
    [[nodiscard]] double net_exposure() const noexcept
    {
        double total = 0.0;
        for (const auto& [sym, pos] : positions_) {
            // Symmetric NaN/Inf defense to `total_unrealized_pnl` and
            // `total_realized_pnl`. `pos.notional` = |qty|*avg_price
            // recomputed on every on_fill from finite inputs, so under
            // normal flow it's finite. Defense-in-depth for any
            // future Position mutability path that bypasses on_fill —
            // a NaN notional would silently poison the entire
            // exposure aggregate (and risk_check.hpp path-5 reads
            // `current_exposure = positions.net_exposure()` which
            // already has its own isfinite trap downstream, so this
            // catches the bad input one layer earlier with the
            // actionable per-symbol diagnostic).
            if (!std::isfinite(pos.notional)) [[unlikely]] {
                SPDLOG_LOGGER_WARN(detail::position_logger(),
                    "net_exposure: non-finite notional={} for symbol={}, "
                    "skipping (would poison the running exposure total "
                    "with NaN)", pos.notional, sym);
                continue;
            }
            total += pos.notional;  // notional is already abs(qty) * avg_price
        }
        return total;
    }

    /// @brief Alias for `net_exposure()` with the precise name.
    ///
    /// Prefer this in new code. Returns Σ abs(qty) * avg_price.
    [[nodiscard]] double gross_exposure() const noexcept
    {
        return net_exposure();
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

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for Position
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::fix::Position> : std::formatter<std::string> {
    auto format(const eph::fix::Position& p, auto& ctx) const {
        return std::formatter<std::string>::format(p.dump(), ctx);
    }
};
