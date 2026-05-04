#pragma once

/// @file signals.hpp
/// Market microstructure signal calculators for HFT order book analysis.
///
/// Pure function templates that derive common trading signals from any book
/// type exposing best_bid(), best_ask(), total_bid_qty(), total_ask_qty().
/// All functions are noexcept and allocation-free on the hot path.
///
/// Supported signals:
///   - **Order imbalance**: normalized buy/sell pressure metric
///   - **Weighted mid / microprice**: size-adjusted fair value estimates
///   - **Spread (bps)**: bid-ask spread normalized to basis points
///   - **VWAP**: volume-weighted average price across depth levels
///   - **Depth ratio**: aggregate buy vs. sell interest

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

#include "eph/book/array_book.hpp" // PriceLevel

namespace eph::book {

// ============================================================================
// Order flow imbalance
// ============================================================================

/// @brief Normalized order flow imbalance: (bid_qty - ask_qty) / (bid_qty + ask_qty).
///
/// Returns a value in [-1.0, 1.0].  Positive indicates buy pressure,
/// negative indicates sell pressure.  Returns 0.0 if both sides are empty.
///
/// @tparam Book  Any book type exposing total_bid_qty() and total_ask_qty().
/// @param  book  The order book to measure imbalance on.
/// @return Imbalance ratio in [-1.0, 1.0], or 0.0 if the book is empty.
template <typename Book>
[[nodiscard]] double order_imbalance(const Book& book) noexcept {
    const double bid_qty = book.total_bid_qty();
    const double ask_qty = book.total_ask_qty();
    const double total   = bid_qty + ask_qty;
    // `!(total > 0.0)` instead of `total <= 0.0` so NaN folds into the
    // empty-book early-return rather than slipping past the guard. The
    // ArrayBook / MapBook update_side() validators reject non-finite
    // qty at insert, so `total` is finite under normal flow — but this
    // function is templated on `Book`, so a future user-supplied book
    // type that exposes total_bid_qty() / total_ask_qty() without the
    // same insert-time guards (e.g. a side-loaded snapshot store, a
    // unit-test fixture, a prototype L3 aggregator) would otherwise let
    // NaN slip into `(bid-ask)/total = NaN` and silently poison every
    // downstream trading signal that consumes order_imbalance. Same
    // defensive pattern as `rate_limiter.hpp::sanitize_refill_` and
    // `signals.hpp::vwap` (both flag the symmetric NaN-comparison
    // hazard in their inline rationale).
    if (!(total > 0.0)) return 0.0;
    return (bid_qty - ask_qty) / total;
}

// ============================================================================
// Weighted mid price
// ============================================================================

/// @brief Weighted mid price using top-of-book quantities.
///
/// Formula: bid * (ask_qty / total) + ask * (bid_qty / total)
/// where total = bid_qty + ask_qty at the BBO.
///
/// More accurate than simple mid when top-of-book sizes are asymmetric.
///
/// @tparam Book  Any book type exposing best_bid() and best_ask() returning
///               optional<PriceLevel>.
/// @param  book  The order book to compute weighted mid for.
/// @return Weighted mid price, or nullopt if either side is empty.
template <typename Book>
[[nodiscard]] std::optional<double> weighted_mid(const Book& book) noexcept {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (!bid || !ask) return std::nullopt;

    const double total = bid->qty + ask->qty;
    // `!(total > 0.0)` folds NaN into the nullopt branch — see
    // `order_imbalance` above for the templated-book defensive rationale.
    // best_bid()/best_ask() return PriceLevel by value from the book, but a
    // user-supplied book can synthesise levels without insert-time validation.
    if (!(total > 0.0)) return std::nullopt;

    // Weight each price by the *opposite* side's quantity — a large ask qty
    // pulls the fair value toward the bid and vice versa.
    return (bid->price * ask->qty + ask->price * bid->qty) / total;
}

// ============================================================================
// Microprice
// ============================================================================

/// @brief Microprice: size-weighted fair value using only top-of-book.
///
/// microprice = (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)
///
/// Semantically distinct from weighted_mid in that microprice always uses
/// only top-of-book, whereas weighted_mid could be extended to deeper levels.
///
/// @tparam Book  Any book type exposing best_bid() and best_ask().
/// @param  book  The order book to compute microprice for.
/// @return Microprice estimate, or nullopt if either side is empty.
template <typename Book>
[[nodiscard]] std::optional<double> microprice(const Book& book) noexcept {
    // Top-of-book microprice is the same formula as weighted mid.
    return weighted_mid(book);
}

// ============================================================================
// Bid-ask spread in basis points
// ============================================================================

/// @brief Bid-ask spread in basis points: (ask - bid) / mid * 10000.
///
/// @tparam Book  Any book type exposing best_bid() and best_ask().
/// @param  book  The order book to measure spread on.
/// @return Spread in basis points, or nullopt if either side is empty or mid <= 0.
template <typename Book>
[[nodiscard]] std::optional<double> spread_bps(const Book& book) noexcept {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (!bid || !ask) return std::nullopt;

    const double mid = (bid->price + ask->price) / 2.0;
    // `!(mid > 0.0)` folds NaN into the nullopt branch — same templated-book
    // defensive rationale as `order_imbalance` / `weighted_mid` above. A
    // negative or zero mid is also undefined for a bps spread.
    if (!(mid > 0.0)) return std::nullopt;

    return (ask->price - bid->price) / mid * 10000.0;
}

// ============================================================================
// VWAP over a span of price levels
// ============================================================================

/// @brief Volume-weighted average price over a span of PriceLevels.
///
/// Works with ArrayBook::bids()/asks() spans or any contiguous PriceLevel range.
///
/// @param levels  Contiguous span of price levels to aggregate.
/// @return VWAP value, or `std::nullopt` if the span is empty or total quantity is zero.
[[nodiscard]] inline std::optional<double>
vwap(std::span<const PriceLevel> levels) noexcept {
    if (levels.empty()) return std::nullopt;

    // Defense-in-depth: callers normally pass spans from a validated
    // book (ArrayBook / MapBook reject non-finite price/qty at insert),
    // but this is a free function — a caller can build any span. A
    // single NaN/Inf level would poison `sum_pq` / `sum_q` for every
    // subsequent level, the `sum_q <= 0.0` guard would silently fail
    // (every NaN comparison is false), and the function would return
    // a NaN that propagates into trading-signal pipelines downstream.
    // Skip non-finite levels with the same fail-soft policy applied
    // by `PositionTracker::total_unrealized_pnl`.
    double sum_pq = 0.0;
    double sum_q  = 0.0;
    for (const auto& lvl : levels) {
        if (!std::isfinite(lvl.price) || !std::isfinite(lvl.qty)) [[unlikely]]
            continue;
        sum_pq += lvl.price * lvl.qty;
        sum_q  += lvl.qty;
    }
    if (sum_q <= 0.0) return std::nullopt;
    return sum_pq / sum_q;
}

// ============================================================================
// Book depth ratio
// ============================================================================

/// @brief Depth ratio: total_bid_qty / total_ask_qty.
///
/// Values > 1.0 indicate more buy interest; < 1.0 more sell interest.
///
/// Returns `std::nullopt` when the ratio is undefined — either side empty
/// (0/0 or X/0). The previous "0.0 when ask side empty" sentinel inverted
/// the signal: an empty ask is the most extreme buy-pressure regime, but
/// 0.0 reads as the most extreme sell-pressure regime to any caller doing
/// `if (depth_ratio > threshold)`. Returning `nullopt` matches the rest of
/// this header (`weighted_mid`, `microprice`, `spread_bps`, `vwap`) and
/// forces callers to make a deliberate choice about handling the
/// undefined case.
///
/// @tparam Book  Any book type exposing total_bid_qty() and total_ask_qty().
/// @param  book  The order book to compute the depth ratio for.
/// @return Ratio of aggregate bid to ask quantity, or `std::nullopt`
///         when either side is empty.
template <typename Book>
[[nodiscard]] std::optional<double> depth_ratio(const Book& book) noexcept {
    const double ask_qty = book.total_ask_qty();
    const double bid_qty = book.total_bid_qty();
    // `!(ask_qty > 0.0) || !(bid_qty > 0.0)` folds NaN into the nullopt
    // branch — same templated-book defensive rationale as `order_imbalance`
    // / `weighted_mid` / `spread_bps` above. NaN slipping past `<= 0.0`
    // would silently return NaN/inf to a caller doing `if (depth_ratio
    // > threshold)` (NaN > anything == false → buy-pressure threshold
    // never trips), exactly the failure mode the existing nullopt
    // contract was designed to surface.
    if (!(ask_qty > 0.0) || !(bid_qty > 0.0)) return std::nullopt;
    return bid_qty / ask_qty;
}

} // namespace eph::book
