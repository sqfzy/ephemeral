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

#include <spdlog/spdlog.h>

#include "eph/book/array_book.hpp"  // PriceLevel

namespace eph::book {

// ============================================================================
// MapBook — dynamic-depth sorted order book
// ============================================================================

/// Deep order book for L3 feeds (1000+ levels).
/// Uses std::map for guaranteed sorted order and O(log n) operations.
/// More memory-hungry than ArrayBook but scales to arbitrary depth.
class MapBook {
public:
    // -- Price-level updates (crypto exchange style) -------------------------

    /// Insert or update a bid level.  If @p qty == 0 the level is removed.
    void update_bid(double price, double qty) noexcept {
        SPDLOG_TRACE("MapBook::update_bid price={} qty={}", price, qty);
        update_side(bids_, price, qty);
    }

    /// Insert or update an ask level.  If @p qty == 0 the level is removed.
    void update_ask(double price, double qty) noexcept {
        SPDLOG_TRACE("MapBook::update_ask price={} qty={}", price, qty);
        update_side(asks_, price, qty);
    }

    // -- BBO queries ---------------------------------------------------------

    /// Best bid (highest price).  Returns nullopt if the bid side is empty.
    [[nodiscard]] std::optional<PriceLevel> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        auto it = bids_.begin();
        return PriceLevel{it->first, it->second};
    }

    /// Best ask (lowest price).  Returns nullopt if the ask side is empty.
    [[nodiscard]] std::optional<PriceLevel> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        auto it = asks_.begin();
        return PriceLevel{it->first, it->second};
    }

    /// Mid price = (best_bid + best_ask) / 2.
    /// Returns nullopt when either side is empty.
    [[nodiscard]] std::optional<double> mid_price() const noexcept {
        if (bids_.empty() || asks_.empty()) return std::nullopt;
        return (bids_.begin()->first + asks_.begin()->first) / 2.0;
    }

    /// Spread = best_ask - best_bid.
    /// Returns nullopt when either side is empty.
    [[nodiscard]] std::optional<double> spread() const noexcept {
        if (bids_.empty() || asks_.empty()) return std::nullopt;
        return asks_.begin()->first - bids_.begin()->first;
    }

    // -- Depth queries -------------------------------------------------------

    /// Number of active bid levels.
    [[nodiscard]] std::size_t bid_depth() const noexcept { return bids_.size(); }

    /// Number of active ask levels.
    [[nodiscard]] std::size_t ask_depth() const noexcept { return asks_.size(); }

    /// Total number of active levels (bid + ask).
    [[nodiscard]] std::size_t level_count() const noexcept {
        return bids_.size() + asks_.size();
    }

    // -- Housekeeping --------------------------------------------------------

    /// True if the book is crossed (best_bid >= best_ask).
    /// This indicates a market anomaly or a stale book.  Returns false when
    /// either side is empty.
    [[nodiscard]] bool is_crossed() const noexcept {
        if (bids_.empty() || asks_.empty()) return false;
        return bids_.begin()->first >= asks_.begin()->first - kEps;
    }

    /// Sum of quantities across all bid levels.
    [[nodiscard]] double total_bid_qty() const noexcept {
        double sum = 0.0;
        for (const auto& [price, qty] : bids_) sum += qty;
        return sum;
    }

    /// Sum of quantities across all ask levels.
    [[nodiscard]] double total_ask_qty() const noexcept {
        double sum = 0.0;
        for (const auto& [price, qty] : asks_) sum += qty;
        return sum;
    }

    /// Remove all levels from both sides.
    void clear() noexcept {
        bids_.clear();
        asks_.clear();
        SPDLOG_DEBUG("MapBook cleared");
    }

    // -- Top-N extraction (for display/logging) ------------------------------

    /// Get top N bid levels (descending by price).
    /// Returns min(n, bid_depth()) levels.
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

    /// Get top N ask levels (ascending by price).
    /// Returns min(n, ask_depth()) levels.
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

    /// Epsilon for floating-point price comparisons.
    static constexpr double kEps = 1e-12;

    /// Epsilon-tolerant equality for floating-point prices.
    /// Matches ArrayBook::price_eq semantics.
    [[nodiscard]] static bool price_eq(double a, double b) noexcept {
        return std::fabs(a - b) < kEps;
    }

    /// Find a key within epsilon tolerance using lower_bound.
    /// std::map::find uses exact comparison; this scans nearby keys
    /// to match ArrayBook's epsilon-tolerant behavior.
    template <typename Map>
    static auto find_approx(Map& side, double price) noexcept {
        // lower_bound finds first key >= price (ascending) or first key
        // where !(key > price) (descending). Check the found key and its
        // immediate neighbor for epsilon match.
        auto it = side.lower_bound(price);
        if (it != side.end() && price_eq(it->first, price)) return it;
        if (it != side.begin()) {
            auto prev = std::prev(it);
            if (price_eq(prev->first, price)) return prev;
        }
        return side.end();
    }

    /// Core update logic for either side.
    /// If qty > 0, insert or update the level.
    /// If qty <= 0, remove the level.
    /// NaN prices are rejected.
    ///
    /// Uses epsilon-tolerant price matching consistent with ArrayBook.
    template <typename Map>
    static void update_side(Map& side, double price, double qty) noexcept {
        // Reject NaN prices — they would corrupt map ordering.
        if (std::isnan(price)) {
            SPDLOG_WARN("MapBook::update_side ignoring NaN price");
            return;
        }

        if (qty <= 0.0) {
            // Remove the level if it exists (epsilon-tolerant lookup).
            auto it = find_approx(side, price);
            if (it != side.end()) {
                SPDLOG_TRACE("MapBook remove level price={}", price);
                side.erase(it);
            } else {
                SPDLOG_TRACE("MapBook remove non-existent price={} — no-op",
                             price);
            }
            return;
        }

        // Check for existing level within epsilon — update in place
        // rather than inserting a near-duplicate key.
        auto it = find_approx(side, price);
        if (it != side.end()) {
            SPDLOG_TRACE("MapBook updated price={} qty={}", price, qty);
            it->second = qty;
        } else {
            side.emplace(price, qty);
            SPDLOG_DEBUG("MapBook inserted price={} qty={} (depth={})",
                         price, qty, side.size());
        }
    }
};

} // namespace eph::book
