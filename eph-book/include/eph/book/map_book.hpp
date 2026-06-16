#pragma once

/// @file map_book.hpp
/// Deep L3 order book backed by std::map for sorted-order maintenance.
///
/// MapBook supports 1000+ price levels with O(log n) insert/delete/lookup.
/// Unlike ArrayBook (fixed-size arrays, optimal for shallow 5-20 level L2
/// feeds), MapBook uses std::map to handle arbitrary depth without capacity
/// limits.  BBO access is O(1) via begin()/rbegin() on the sorted maps.
///
/// Price levels are stored as (price → qty) pairs.  Bids are sorted
/// descending (std::greater) so begin() yields the best bid; asks are
/// sorted ascending (default) so begin() yields the best ask.
///
/// Two update styles are supported from a single interface:
///   - **Explicit** (crypto exchange style): call update_bid / update_ask
///     with a price and quantity.  qty == 0 removes the level.
///   - **Implicit** (order-level, e.g. ITCH): the caller maintains
///     order-to-level aggregation externally and feeds the result here.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "eph/core/log.hpp"

#include "eph/book/array_book.hpp"  // PriceLevel

namespace eph::book {

/// @cond INTERNAL
namespace detail {
/// @brief Lazily-constructed spdlog logger for MapBook diagnostics.
/// @return Pointer to the "book.map_book" logger instance.
inline spdlog::logger* map_book_logger() {
    static spdlog::logger* l = ::eph::log::get("book.map_book");
    return l;
}
} // namespace detail
/// @endcond

// ============================================================================
// MapBook — dynamic-depth sorted order book
// ============================================================================

/// @brief Dynamic-depth order book backed by `std::map` for L3 / deep-book feeds.
///
/// Supports 1000+ price levels with O(log n) insert/delete/lookup.  Unlike
/// ArrayBook (fixed-size arrays, optimal for shallow 5-20 level L2 feeds),
/// MapBook uses `std::map` to handle arbitrary depth without capacity limits.
/// BBO access is O(1) via `begin()` / `rbegin()` on the sorted maps.
///
/// Prices are quantized to multiples of 1e-9 (sub-nano precision) on
/// insertion, preventing near-duplicate keys from different floating-point
/// rounding paths.  This exceeds any exchange tick size.
///
/// @note  MapBook allocates on insert (heap nodes from `std::map`).  For
///        latency-critical shallow-book paths, prefer ArrayBook.
/// @warning The bids() and asks() accessors return `std::vector` copies
///          (not spans) because `std::map` is node-based.  Avoid calling
///          them on the hot path.
class MapBook {
public:
    // -- Price-level updates (crypto exchange style) -------------------------

    /// @brief Insert or update a bid level.  If @p qty == 0 the level is removed.
    ///
    /// The price is quantized before insertion to prevent near-duplicate keys.
    ///
    /// @param price  Bid price.  NaN values are rejected with a warning.
    /// @param qty    Quantity at this price.  Zero or negative removes the level.
    void update_bid(double price, double qty) noexcept {
        EPH_LOG_TRACE(detail::map_book_logger(), "MapBook::update_bid price={} qty={}", price, qty);
        update_side(bids_, price, qty);
    }

    /// @brief Insert or update an ask level.  If @p qty == 0 the level is removed.
    ///
    /// The price is quantized before insertion to prevent near-duplicate keys.
    ///
    /// @param price  Ask price.  NaN values are rejected with a warning.
    /// @param qty    Quantity at this price.  Zero or negative removes the level.
    void update_ask(double price, double qty) noexcept {
        EPH_LOG_TRACE(detail::map_book_logger(), "MapBook::update_ask price={} qty={}", price, qty);
        update_side(asks_, price, qty);
    }

    // -- BBO queries ---------------------------------------------------------

    /// @brief Best bid (highest price).
    /// @return The top-of-book bid level, or `std::nullopt` if the bid side is empty.
    [[nodiscard]] std::optional<PriceLevel> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        auto it = bids_.begin();
        return PriceLevel{it->first, it->second};
    }

    /// @brief Best ask (lowest price).
    /// @return The top-of-book ask level, or `std::nullopt` if the ask side is empty.
    [[nodiscard]] std::optional<PriceLevel> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        auto it = asks_.begin();
        return PriceLevel{it->first, it->second};
    }

    /// @brief Mid price: arithmetic mean of the best bid and best ask prices.
    /// @return `(best_bid + best_ask) / 2`, or `std::nullopt` when either side is empty.
    [[nodiscard]] std::optional<double> mid_price() const noexcept {
        if (bids_.empty() || asks_.empty()) return std::nullopt;
        return (bids_.begin()->first + asks_.begin()->first) / 2.0;
    }

    /// @brief Bid-ask spread in native price units.
    /// @return `best_ask - best_bid`, or `std::nullopt` when either side is empty.
    /// @note  A negative spread indicates a crossed book.
    [[nodiscard]] std::optional<double> spread() const noexcept {
        if (bids_.empty() || asks_.empty()) return std::nullopt;
        return asks_.begin()->first - bids_.begin()->first;
    }

    // -- Depth queries -------------------------------------------------------

    /// @brief Return a vector of all active bid levels, sorted descending by price.
    ///
    /// Unlike ArrayBook::bids() which returns a zero-copy span, MapBook must
    /// copy into a vector since `std::map` is node-based.  The returned vector
    /// is suitable for use with signals::vwap() and similar span-accepting APIs.
    ///
    /// @return Owning vector of PriceLevel; empty if the bid side is empty.
    /// @warning Allocates on every call.  Prefer top_bids() on the hot path.
    [[nodiscard]] std::vector<PriceLevel> bids() const {
        std::vector<PriceLevel> result;
        result.reserve(bids_.size());
        for (const auto& [price, qty] : bids_) {
            result.push_back({price, qty});
        }
        return result;
    }

    /// @brief Return a vector of all active ask levels, sorted ascending by price.
    /// @return Owning vector of PriceLevel; empty if the ask side is empty.
    /// @warning Allocates on every call.  Prefer top_asks() on the hot path.
    /// @see bids() for rationale on returning a vector instead of a span.
    [[nodiscard]] std::vector<PriceLevel> asks() const {
        std::vector<PriceLevel> result;
        result.reserve(asks_.size());
        for (const auto& [price, qty] : asks_) {
            result.push_back({price, qty});
        }
        return result;
    }

    /// @brief Number of active bid levels.
    /// @return Count of distinct bid prices currently in the book.
    [[nodiscard]] std::size_t bid_depth() const noexcept { return bids_.size(); }

    /// @brief Number of active ask levels.
    /// @return Count of distinct ask prices currently in the book.
    [[nodiscard]] std::size_t ask_depth() const noexcept { return asks_.size(); }

    /// @brief Total number of active levels (bid + ask).
    /// @return Combined count of all price levels.
    [[nodiscard]] std::size_t level_count() const noexcept {
        return bids_.size() + asks_.size();
    }

    // -- Housekeeping --------------------------------------------------------

    /// @brief Check whether the book is crossed (best bid > best ask).
    ///
    /// A crossed book is anomalous, typically indicating a feed error.
    /// The locked case (bid == ask within epsilon) is excluded.
    ///
    /// @return `true` if crossed, `false` when either side is empty or normal/locked.
    [[nodiscard]] bool is_crossed() const noexcept {
        if (bids_.empty() || asks_.empty()) return false;
        return bids_.begin()->first > asks_.begin()->first + kEps;
    }

    /// @brief Check whether the book is locked (best bid == best ask within epsilon).
    ///
    /// A locked market is distinct from a crossed book: prices are equal
    /// rather than inverted.
    ///
    /// @return `true` if locked, `false` when either side is empty or prices differ.
    [[nodiscard]] bool is_locked() const noexcept {
        if (bids_.empty() || asks_.empty()) return false;
        return std::abs(bids_.begin()->first - asks_.begin()->first) <= kEps;
    }

    /// @brief Sum of quantities across all active bid levels.
    /// @return Aggregate bid quantity (>= 0.0).  Zero when the bid side is empty.
    [[nodiscard]] double total_bid_qty() const noexcept {
        double sum = 0.0;
        for (const auto& [price, qty] : bids_) sum += qty;
        return sum;
    }

    /// @brief Sum of quantities across all active ask levels.
    /// @return Aggregate ask quantity (>= 0.0).  Zero when the ask side is empty.
    [[nodiscard]] double total_ask_qty() const noexcept {
        double sum = 0.0;
        for (const auto& [price, qty] : asks_) sum += qty;
        return sum;
    }

    /// @brief Remove all levels from both sides, resetting the book to empty.
    void clear() noexcept {
        bids_.clear();
        asks_.clear();
        EPH_LOG_DEBUG(detail::map_book_logger(), "MapBook cleared");
    }

    // -- Top-N extraction (for display/logging) ------------------------------

    /// @brief Get the top N bid levels, sorted descending by price.
    ///
    /// Useful for display, logging, or feeding into signal functions that
    /// accept a span of PriceLevels when full-depth is not needed.
    ///
    /// @param n  Maximum number of levels to return.
    /// @return Vector of `min(n, bid_depth())` levels.
    [[nodiscard]] std::vector<PriceLevel> top_bids(std::size_t n) const {
        std::vector<PriceLevel> result;
        result.reserve(std::min(n, bids_.size()));
        std::size_t count = 0;
        for (const auto& [price, qty] : bids_) {
            if (count >= n) break;
            result.push_back({price, qty});
            ++count;
        }
        return result;
    }

    /// @brief Get the top N ask levels, sorted ascending by price.
    ///
    /// Useful for display, logging, or feeding into signal functions that
    /// accept a span of PriceLevels when full-depth is not needed.
    ///
    /// @param n  Maximum number of levels to return.
    /// @return Vector of `min(n, ask_depth())` levels.
    [[nodiscard]] std::vector<PriceLevel> top_asks(std::size_t n) const {
        std::vector<PriceLevel> result;
        result.reserve(std::min(n, asks_.size()));
        std::size_t count = 0;
        for (const auto& [price, qty] : asks_) {
            if (count >= n) break;
            result.push_back({price, qty});
            ++count;
        }
        return result;
    }

private:
    /// Bids sorted descending (highest price = best bid at begin()).
    std::map<double, double, std::greater<>> bids_;

    /// Asks sorted ascending (lowest price = best ask at begin()).
    std::map<double, double> asks_;

    /// Quantization step for price canonicalization.
    /// 1e-9 (sub-nano) exceeds any exchange tick size precision.
    /// Prices are snapped to multiples of this value before use as map keys,
    /// eliminating near-duplicate keys from different rounding paths.
    static constexpr double kTickQuantum = 1e-9;

    /// Epsilon for floating-point price comparisons (matches ArrayBook).
    static constexpr double kEps = 1e-12;

    /// Snap a price to the nearest multiple of kTickQuantum.
    /// This ensures prices from different rounding paths map to the same key.
    [[nodiscard]] static double quantize(double p) noexcept {
        return std::round(p / kTickQuantum) * kTickQuantum;
    }

    /// Epsilon-tolerant equality for floating-point prices.
    /// Matches ArrayBook::price_eq semantics.
    [[nodiscard]] static bool price_eq(double a, double b) noexcept {
        return std::fabs(a - b) < kEps;
    }

    /// Core update logic for either side.
    /// If qty > 0, insert or update the level.
    /// If qty <= 0, remove the level.
    /// NaN prices are rejected.
    ///
    /// Prices are quantized before insertion to prevent near-duplicate keys
    /// from different floating-point rounding paths.
    template <typename Map>
    static void update_side(Map& side, double price, double qty) noexcept {
        // Reject non-finite prices — NaN corrupts map ordering, +Inf would
        // pin best_bid (greater<>) at +Inf indefinitely under ascending /
        // descending sorts and the level couldn't be replaced via tick-size
        // updates from the feed.
        if (!std::isfinite(price)) [[unlikely]] {
            EPH_LOG_WARN(detail::map_book_logger(),
                "MapBook::update_side ignoring non-finite price={}", price);
            return;
        }

        // Reject non-finite qty — every NaN comparison is false, so a NaN
        // qty would skip the `qty <= 0.0` removal branch and call
        // insert_or_assign with NaN, poisoning total_*_qty() and every
        // signal downstream. +Inf qty would similarly poison total_*_qty
        // and any vwap-style aggregation.
        if (!std::isfinite(qty)) [[unlikely]] {
            EPH_LOG_WARN(detail::map_book_logger(),
                "MapBook::update_side ignoring non-finite qty={} at price={}", qty, price);
            return;
        }

        // Canonicalize price to eliminate rounding-induced duplicates.
        price = quantize(price);

        if (qty <= 0.0) {
            auto it = side.find(price);
            if (it != side.end()) {
                EPH_LOG_TRACE(detail::map_book_logger(), "MapBook remove level price={}", price);
                side.erase(it);
            } else {
                EPH_LOG_TRACE(detail::map_book_logger(), "MapBook remove non-existent price={} — no-op",
                             price);
            }
            return;
        }

        // After quantization, exact map::find is safe — no near-duplicates.
        auto [it, inserted] = side.insert_or_assign(price, qty);
        if (inserted) {
            EPH_LOG_DEBUG(detail::map_book_logger(), "MapBook inserted price={} qty={} (depth={})",
                         price, qty, side.size());
        } else {
            EPH_LOG_TRACE(detail::map_book_logger(), "MapBook updated price={} qty={}", price, qty);
        }
    }
};

} // namespace eph::book
