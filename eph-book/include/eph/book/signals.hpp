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
    if (total <= 0.0) return 0.0;
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
    if (total <= 0.0) return std::nullopt;

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
    if (mid <= 0.0) return std::nullopt;

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

    double sum_pq = 0.0;
    double sum_q  = 0.0;
    for (const auto& lvl : levels) {
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
/// @tparam Book  Any book type exposing total_bid_qty() and total_ask_qty().
/// @param  book  The order book to compute the depth ratio for.
/// @return Ratio of aggregate bid to ask quantity, or 0.0 if the ask side
///         is empty (avoids division by zero).
template <typename Book>
[[nodiscard]] double depth_ratio(const Book& book) noexcept {
    const double ask_qty = book.total_ask_qty();
    if (ask_qty <= 0.0) return 0.0;
    return book.total_bid_qty() / ask_qty;
}

} // namespace eph::book
