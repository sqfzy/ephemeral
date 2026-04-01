#pragma once

/// @file array_book.hpp
/// Fixed-size, zero-allocation L2 order book backed by sorted arrays.
///
/// ArrayBook maintains bid and ask price levels in contiguous std::arrays,
/// sorted so that the best price always sits at index 0.  This layout is
/// cache-friendly for shallow books (5-20 levels) typical of crypto L2 feeds
/// and equity top-of-book data.
///
/// Two update styles are supported from a single interface:
///   - **Explicit** (crypto exchange style): call update_bid / update_ask with
///     a price and quantity.  qty == 0 removes the level.
///   - **Implicit** (order-level, e.g. ITCH): the caller maintains
///     order-to-level aggregation externally and feeds the result here.
///
/// Template parameter `MaxLevels` caps each side independently.  When the book
/// is full and a new price is worse than the worst tracked level, it is silently
/// dropped — this is the expected behavior for L2 depth snapshots.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

#include <spdlog/spdlog.h>

namespace eph::book {

// ============================================================================
// PriceLevel — a single (price, qty) pair on one side of the book
// ============================================================================

struct PriceLevel {
    double price = 0.0;
    double qty   = 0.0;
};

// ============================================================================
// ArrayBook — fixed-capacity sorted L2 order book
// ============================================================================

/// @tparam MaxLevels  Maximum number of price levels per side (bid / ask).
template <std::size_t MaxLevels = 20>
class ArrayBook {
    static_assert(MaxLevels > 0, "MaxLevels must be at least 1");

public:
    // -- Price-level updates (crypto exchange style) -------------------------

    /// Insert or update a bid level.  If @p qty == 0 the level is removed.
    void update_bid(double price, double qty) noexcept {
        SPDLOG_TRACE("update_bid price={} qty={}", price, qty);
        update_side(bids_, bid_count_, price, qty, /*descending=*/true);
    }

    /// Insert or update an ask level.  If @p qty == 0 the level is removed.
    void update_ask(double price, double qty) noexcept {
        SPDLOG_TRACE("update_ask price={} qty={}", price, qty);
        update_side(asks_, ask_count_, price, qty, /*descending=*/false);
    }

    // -- BBO queries ---------------------------------------------------------

    /// Best bid (highest price).  Returns nullopt if the bid side is empty.
    [[nodiscard]] std::optional<PriceLevel> best_bid() const noexcept {
        if (bid_count_ == 0) return std::nullopt;
        return bids_[0];
    }

    /// Best ask (lowest price).  Returns nullopt if the ask side is empty.
    [[nodiscard]] std::optional<PriceLevel> best_ask() const noexcept {
        if (ask_count_ == 0) return std::nullopt;
        return asks_[0];
    }

    /// Mid price = (best_bid + best_ask) / 2.
    /// Returns nullopt when either side is empty.
    [[nodiscard]] std::optional<double> mid_price() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return std::nullopt;
        return (bids_[0].price + asks_[0].price) / 2.0;
    }

    /// Spread = best_ask - best_bid.
    /// Returns nullopt when either side is empty.
    [[nodiscard]] std::optional<double> spread() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return std::nullopt;
        return asks_[0].price - bids_[0].price;
    }

    // -- Depth queries -------------------------------------------------------

    /// Return a read-only span over active bid levels (descending by price).
    [[nodiscard]] std::span<const PriceLevel> bids() const noexcept {
        return {bids_.data(), bid_count_};
    }

    /// Return a read-only span over active ask levels (ascending by price).
    [[nodiscard]] std::span<const PriceLevel> asks() const noexcept {
        return {asks_.data(), ask_count_};
    }

    /// Number of active bid levels.
    [[nodiscard]] std::size_t bid_depth() const noexcept { return bid_count_; }

    /// Number of active ask levels.
    [[nodiscard]] std::size_t ask_depth() const noexcept { return ask_count_; }

    // -- Housekeeping --------------------------------------------------------

    /// Remove all levels from both sides.
    void clear() noexcept {
        bid_count_ = 0;
        ask_count_ = 0;
        SPDLOG_DEBUG("ArrayBook cleared");
    }

    /// True if best bid strictly exceeds best ask (anomalous).
    /// Excludes the locked case (bid == ask within epsilon).
    /// Returns false when either side is empty.
    [[nodiscard]] bool is_crossed() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return false;
        return bids_[0].price > asks_[0].price + kEps;
    }

    /// True if best bid equals best ask within epsilon (locked market).
    /// A locked market is distinct from crossed: prices are equal rather
    /// than inverted.  Uses <= so the boundary case (diff == kEps) that
    /// is_crossed() excludes is caught here.  Returns false when either
    /// side is empty.
    [[nodiscard]] bool is_locked() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return false;
        return std::abs(bids_[0].price - asks_[0].price) <= kEps;
    }

    /// Sum of quantities across all bid levels.
    [[nodiscard]] double total_bid_qty() const noexcept {
        double sum = 0.0;
        for (std::size_t i = 0; i < bid_count_; ++i) sum += bids_[i].qty;
        return sum;
    }

    /// Sum of quantities across all ask levels.
    [[nodiscard]] double total_ask_qty() const noexcept {
        double sum = 0.0;
        for (std::size_t i = 0; i < ask_count_; ++i) sum += asks_[i].qty;
        return sum;
    }

    /// Total number of active levels (bid + ask).
    [[nodiscard]] std::size_t level_count() const noexcept {
        return bid_count_ + ask_count_;
    }

    /// Maximum number of levels per side (compile-time constant).
    static constexpr std::size_t max_levels = MaxLevels;

private:
    std::array<PriceLevel, MaxLevels> bids_{};
    std::array<PriceLevel, MaxLevels> asks_{};
    std::size_t bid_count_ = 0;
    std::size_t ask_count_ = 0;

    /// Epsilon for floating-point price comparisons.
    static constexpr double kEps = 1e-12;

    /// Returns true when two prices are considered equal.
    [[nodiscard]] static bool price_eq(double a, double b) noexcept {
        return std::fabs(a - b) < kEps;
    }

    // ========================================================================
    // Core update logic — works for both bid (descending) and ask (ascending).
    //
    // Invariants maintained:
    //   - levels[0..count) are sorted (descending if `descending`, else ascending)
    //   - count <= MaxLevels
    //
    // Three cases:
    //   1. Price already exists → update qty in-place (or remove if qty == 0).
    //   2. qty == 0 and price not found → no-op.
    //   3. New price with qty > 0 → insert at sorted position, shifting tail.
    //      If full and price is worse than the worst tracked, ignore.
    // ========================================================================
    void update_side(std::array<PriceLevel, MaxLevels>& levels,
                     std::size_t& count,
                     double price, double qty,
                     bool descending) noexcept {

        // -- Reject NaN prices — they would corrupt sort order ---------------
        if (std::isnan(price)) {
            SPDLOG_WARN("update_side ignoring NaN price");
            return;
        }

        // -- Warn on negative qty (caller likely has a bug) ------------------
        if (qty < 0.0) [[unlikely]] {
            SPDLOG_WARN("update_side: negative qty={} at price={} treated as "
                         "removal — caller should use qty=0 for explicit removal",
                         qty, price);
        }

        // -- Search for an existing level at this price ----------------------
        for (std::size_t i = 0; i < count; ++i) {
            if (price_eq(levels[i].price, price)) {
                if (qty <= 0.0) {
                    // Remove: shift remaining levels down by one.
                    SPDLOG_TRACE("remove level price={} idx={}", price, i);
                    if (i + 1 < count) {
                        std::memmove(&levels[i], &levels[i + 1],
                                     (count - i - 1) * sizeof(PriceLevel));
                    }
                    --count;
                } else {
                    // Update in-place.
                    SPDLOG_TRACE("update level price={} old_qty={} new_qty={}",
                                 price, levels[i].qty, qty);
                    levels[i].qty = qty;
                }
                return;
            }
        }

        // -- Price not found: nothing to remove ------------------------------
        if (qty <= 0.0) {
            SPDLOG_TRACE("remove non-existent price={} — no-op", price);
            return;
        }

        // -- Find insertion point to maintain sort order ----------------------
        // descending: new price should go before the first level whose price
        //             is less than it (bids — higher is better).
        // ascending:  new price should go before the first level whose price
        //             is greater than it (asks — lower is better).
        std::size_t pos = count; // default: append at end
        for (std::size_t i = 0; i < count; ++i) {
            bool should_insert = descending
                ? (price > levels[i].price + kEps)
                : (price < levels[i].price - kEps);
            if (should_insert) {
                pos = i;
                break;
            }
        }

        // -- If book is full, check whether the new level is worse than worst -
        if (count >= MaxLevels) {
            if (pos >= MaxLevels) {
                // New price is worse than everything we track — drop it.
                SPDLOG_TRACE("book full, dropping worse price={}", price);
                return;
            }
            // We will insert at `pos` and the last level falls off.
            // Shift [pos, MaxLevels-2] → [pos+1, MaxLevels-1].
            if (pos < MaxLevels - 1) {
                std::memmove(&levels[pos + 1], &levels[pos],
                             (MaxLevels - 1 - pos) * sizeof(PriceLevel));
            }
            levels[pos] = {price, qty};
            // count stays at MaxLevels (last level evicted).
            SPDLOG_DEBUG("inserted price={} qty={} at idx={} (evicted worst)",
                         price, qty, pos);
            return;
        }

        // -- Normal insert (room available) ----------------------------------
        if (pos < count) {
            std::memmove(&levels[pos + 1], &levels[pos],
                         (count - pos) * sizeof(PriceLevel));
        }
        levels[pos] = {price, qty};
        ++count;
        SPDLOG_DEBUG("inserted price={} qty={} at idx={} (depth={})",
                     price, qty, pos, count);
    }
};

} // namespace eph::book
