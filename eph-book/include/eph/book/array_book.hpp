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
#include <spdlog/sinks/stdout_color_sinks.h>

namespace eph::book {

/// @cond INTERNAL
namespace detail {
/// @brief Lazily-constructed spdlog logger for ArrayBook diagnostics.
/// @return Pointer to the "book.array_book" logger instance.
inline spdlog::logger* array_book_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("book.array_book");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("book.array_book");
        }
    }();
    return l.get();
}
} // namespace detail
/// @endcond

// ============================================================================
// PriceLevel — a single (price, qty) pair on one side of the book
// ============================================================================

/// @brief A single price level on one side of an order book.
///
/// Represents one row in the L2 depth view: a price and the total quantity
/// resting at that price.  Used by both ArrayBook and MapBook as the
/// canonical level representation, and accepted by signal functions in
/// signals.hpp via `std::span<const PriceLevel>`.
struct PriceLevel {
    double price = 0.0; ///< Price of this level (native exchange units).
    double qty   = 0.0; ///< Aggregate quantity at this price.
};

// ============================================================================
// ArrayBook — fixed-capacity sorted L2 order book
// ============================================================================

/// @brief Fixed-capacity, zero-allocation L2 order book backed by sorted arrays.
///
/// Maintains bid and ask price levels in contiguous `std::array` storage,
/// sorted so that the best price always sits at index 0.  The contiguous
/// layout is cache-friendly for shallow books (5-20 levels) typical of
/// crypto L2 feeds and equity top-of-book data.
///
/// Levels are updated via update_bid() / update_ask().  Passing `qty == 0`
/// removes the level.  When the book is full and a new price is worse than
/// the worst tracked level, it is silently dropped.
///
/// @tparam MaxLevels  Maximum number of price levels per side (bid / ask).
///                    Must be >= 1.  Defaults to 20.
///
/// @note  This class is trivially relocatable and safe to use in
///        pre-allocated arenas.  All operations are noexcept.
/// @warning Prices are compared with a fixed epsilon of 1e-12.  If your
///          price representation has coarser tick sizes, near-duplicate
///          levels could appear.  Consider MapBook (with price quantization)
///          for sub-tick precision feeds.
template <std::size_t MaxLevels = 20>
class ArrayBook {
    static_assert(MaxLevels > 0, "MaxLevels must be at least 1");

public:
    // -- Price-level updates (crypto exchange style) -------------------------

    /// @brief Insert or update a bid level.  If @p qty == 0 the level is removed.
    ///
    /// Maintains descending sort order so that `bids()[0]` is always the
    /// highest (best) bid.  When the book is full and @p price is worse
    /// (lower) than the worst tracked bid, the update is silently dropped.
    ///
    /// @param price  Bid price.  NaN values are rejected with a warning.
    /// @param qty    Quantity at this price.  Zero removes the level;
    ///               negative values are clamped to zero (treated as removal).
    void update_bid(double price, double qty) noexcept {
        SPDLOG_LOGGER_TRACE(detail::array_book_logger(), "update_bid price={} qty={}", price, qty);
        update_side(bids_, bid_count_, price, qty, /*descending=*/true);
    }

    /// @brief Insert or update an ask level.  If @p qty == 0 the level is removed.
    ///
    /// Maintains ascending sort order so that `asks()[0]` is always the
    /// lowest (best) ask.  When the book is full and @p price is worse
    /// (higher) than the worst tracked ask, the update is silently dropped.
    ///
    /// @param price  Ask price.  NaN values are rejected with a warning.
    /// @param qty    Quantity at this price.  Zero removes the level;
    ///               negative values are clamped to zero (treated as removal).
    void update_ask(double price, double qty) noexcept {
        SPDLOG_LOGGER_TRACE(detail::array_book_logger(), "update_ask price={} qty={}", price, qty);
        update_side(asks_, ask_count_, price, qty, /*descending=*/false);
    }

    // -- BBO queries ---------------------------------------------------------

    /// @brief Best bid (highest price).
    /// @return The top-of-book bid level, or `std::nullopt` if the bid side is empty.
    [[nodiscard]] std::optional<PriceLevel> best_bid() const noexcept {
        if (bid_count_ == 0) return std::nullopt;
        return bids_[0];
    }

    /// @brief Best ask (lowest price).
    /// @return The top-of-book ask level, or `std::nullopt` if the ask side is empty.
    [[nodiscard]] std::optional<PriceLevel> best_ask() const noexcept {
        if (ask_count_ == 0) return std::nullopt;
        return asks_[0];
    }

    /// @brief Mid price: arithmetic mean of the best bid and best ask prices.
    /// @return `(best_bid + best_ask) / 2`, or `std::nullopt` when either side is empty.
    [[nodiscard]] std::optional<double> mid_price() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return std::nullopt;
        return (bids_[0].price + asks_[0].price) / 2.0;
    }

    /// @brief Bid-ask spread in native price units.
    /// @return `best_ask - best_bid`, or `std::nullopt` when either side is empty.
    /// @note  A negative spread indicates a crossed book.  Use is_crossed() to
    ///        test for that condition explicitly.
    [[nodiscard]] std::optional<double> spread() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return std::nullopt;
        return asks_[0].price - bids_[0].price;
    }

    // -- Depth queries -------------------------------------------------------

    /// @brief Return a read-only span over active bid levels, sorted descending by price.
    /// @return Non-owning span of `[0, bid_depth())` elements.  The span is
    ///         invalidated by any subsequent mutating call (update_bid, clear).
    [[nodiscard]] std::span<const PriceLevel> bids() const noexcept {
        return {bids_.data(), bid_count_};
    }

    /// @brief Return a read-only span over active ask levels, sorted ascending by price.
    /// @return Non-owning span of `[0, ask_depth())` elements.  The span is
    ///         invalidated by any subsequent mutating call (update_ask, clear).
    [[nodiscard]] std::span<const PriceLevel> asks() const noexcept {
        return {asks_.data(), ask_count_};
    }

    /// @brief Number of active bid levels.
    /// @return Count in `[0, MaxLevels]`.
    [[nodiscard]] std::size_t bid_depth() const noexcept { return bid_count_; }

    /// @brief Number of active ask levels.
    /// @return Count in `[0, MaxLevels]`.
    [[nodiscard]] std::size_t ask_depth() const noexcept { return ask_count_; }

    // -- Housekeeping --------------------------------------------------------

    /// @brief Remove all levels from both sides, resetting the book to empty.
    void clear() noexcept {
        bid_count_ = 0;
        ask_count_ = 0;
        SPDLOG_LOGGER_DEBUG(detail::array_book_logger(), "ArrayBook cleared");
    }

    /// @brief Check whether the book is crossed (best bid > best ask).
    ///
    /// A crossed book is an anomalous state where the best bid price strictly
    /// exceeds the best ask, typically indicating a feed error or race condition.
    /// The locked case (bid == ask within epsilon) is excluded.
    ///
    /// @return `true` if crossed, `false` when either side is empty or the book
    ///         is normal/locked.
    [[nodiscard]] bool is_crossed() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return false;
        return bids_[0].price > asks_[0].price + kEps;
    }

    /// @brief Check whether the book is locked (best bid == best ask within epsilon).
    ///
    /// A locked market means the best bid and ask are effectively equal,
    /// distinct from a crossed book where bid exceeds ask.  Uses `<= kEps`
    /// so the boundary case that is_crossed() excludes is caught here.
    ///
    /// @return `true` if locked, `false` when either side is empty or prices differ.
    [[nodiscard]] bool is_locked() const noexcept {
        if (bid_count_ == 0 || ask_count_ == 0) return false;
        return std::abs(bids_[0].price - asks_[0].price) <= kEps;
    }

    /// @brief Sum of quantities across all active bid levels.
    /// @return Aggregate bid quantity (>= 0.0).  Zero when the bid side is empty.
    [[nodiscard]] double total_bid_qty() const noexcept {
        double sum = 0.0;
        for (std::size_t i = 0; i < bid_count_; ++i) sum += bids_[i].qty;
        return sum;
    }

    /// @brief Sum of quantities across all active ask levels.
    /// @return Aggregate ask quantity (>= 0.0).  Zero when the ask side is empty.
    [[nodiscard]] double total_ask_qty() const noexcept {
        double sum = 0.0;
        for (std::size_t i = 0; i < ask_count_; ++i) sum += asks_[i].qty;
        return sum;
    }

    /// @brief Total number of active levels (bid + ask).
    /// @return Combined count in `[0, 2 * MaxLevels]`.
    [[nodiscard]] std::size_t level_count() const noexcept {
        return bid_count_ + ask_count_;
    }

    /// @brief Maximum number of levels per side (compile-time constant).
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
            SPDLOG_LOGGER_WARN(detail::array_book_logger(), "update_side ignoring NaN price");
            return;
        }

        // -- Reject NaN qty — every NaN comparison is false, so NaN slips
        //    past both the negative-clamp guard and the `qty <= 0.0` removal
        //    branch and writes NaN straight into levels[i].qty. Downstream
        //    consequences: total_*_qty() returns NaN and every signal that
        //    reads it (vwap, depth_ratio, order_imbalance, mid_price,
        //    spread) cascades NaN forever; is_crossed() / is_locked() get
        //    NaN comparisons that hide real crossed-book conditions.
        //    Treat NaN qty the same fail-soft way as NaN price.
        if (std::isnan(qty)) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::array_book_logger(),
                "update_side ignoring NaN qty at price={}", price);
            return;
        }

        // -- Warn on negative qty (caller likely has a bug) ------------------
        // Intentional: clamp to 0 so downstream removal logic doesn't need
        // to distinguish negative from zero — both mean "remove this level".
        if (qty < 0.0) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::array_book_logger(), "update_side: negative qty={} at price={} clamped to 0 "
                         "(treated as removal) — caller should use qty=0",
                         qty, price);
            qty = 0.0;
        }

        // -- Search for an existing level at this price ----------------------
        for (std::size_t i = 0; i < count; ++i) {
            if (price_eq(levels[i].price, price)) {
                if (qty <= 0.0) {
                    // Remove: shift remaining levels down by one.
                    SPDLOG_LOGGER_TRACE(detail::array_book_logger(), "remove level price={} idx={}", price, i);
                    if (i + 1 < count) {
                        std::memmove(&levels[i], &levels[i + 1],
                                     (count - i - 1) * sizeof(PriceLevel));
                    }
                    --count;
                } else {
                    // Update in-place.
                    SPDLOG_LOGGER_TRACE(detail::array_book_logger(), "update level price={} old_qty={} new_qty={}",
                                 price, levels[i].qty, qty);
                    levels[i].qty = qty;
                }
                return;
            }
        }

        // -- Price not found: nothing to remove ------------------------------
        if (qty <= 0.0) {
            SPDLOG_LOGGER_TRACE(detail::array_book_logger(), "remove non-existent price={} — no-op", price);
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
                SPDLOG_LOGGER_TRACE(detail::array_book_logger(), "book full, dropping worse price={}", price);
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
            SPDLOG_LOGGER_DEBUG(detail::array_book_logger(), "inserted price={} qty={} at idx={} (evicted worst)",
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
        SPDLOG_LOGGER_DEBUG(detail::array_book_logger(), "inserted price={} qty={} at idx={} (depth={})",
                     price, qty, pos, count);
    }
};

} // namespace eph::book
