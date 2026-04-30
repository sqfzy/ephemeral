#pragma once

/// @file itch_adapter.hpp
/// ITCH 5.0 to ArrayBook adapter — bridges order-level ITCH events into an
/// aggregated L2 price-level book.
///
/// Maintains an order map (order_ref -> {price, qty, side}) and feeds
/// price-level updates into ArrayBook whenever an order changes.
///
/// Usage requires both eph-book and eph-itch headers to be available on the
/// include path.  eph-book does NOT depend on eph-itch; the user must ensure
/// both are linked when including this header.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "eph/book/array_book.hpp"
#include "eph/itch/messages.hpp"
#include "eph/itch/parser.hpp"

namespace eph::book {

/// @cond INTERNAL
namespace detail {
/// @brief Lazily-constructed spdlog logger for ItchBookBuilder diagnostics.
/// @return Pointer to the "book.itch_adapter" logger instance.
inline spdlog::logger* itch_adapter_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("book.itch_adapter");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("book.itch_adapter");
        }
    }();
    return l.get();
}
} // namespace detail
/// @endcond

// ---------------------------------------------------------------------------
// Order — per-order state tracked by the adapter
// ---------------------------------------------------------------------------

/// @brief Per-order state tracked by ItchBookBuilder.
///
/// Each live order in the ITCH feed is stored in an internal hash map keyed
/// by its 64-bit order reference number.  When the order is executed,
/// cancelled, deleted, or replaced, this record is updated and the
/// corresponding price level in the ArrayBook is adjusted.
struct Order {
    double price;          ///< Resting price of this order.
    double remaining_qty;  ///< Remaining (unexecuted, uncancelled) quantity.
    char   side;           ///< Side indicator: `'B'` = buy, `'S'` = sell.
};

// ---------------------------------------------------------------------------
// ItchBookBuilder — ITCH-to-L2 book adapter
// ---------------------------------------------------------------------------

/// @brief Converts order-level ITCH 5.0 events into aggregated L2 price levels.
///
/// Maintains an internal order map (`order_ref -> Order`) and per-price
/// quantity accumulators.  When an ITCH message modifies an order, the
/// builder adjusts the aggregated quantity at the affected price and pushes
/// the updated level into the underlying ArrayBook.
///
/// Supported ITCH message types:
///   - `'A'` AddOrder
///   - `'F'` AddOrderMPID
///   - `'E'` OrderExecuted
///   - `'C'` OrderExecutedWithPrice
///   - `'X'` OrderCancel
///   - `'D'` OrderDelete
///   - `'U'` OrderReplace
///
/// All other message types are silently ignored.
///
/// @tparam MaxLevels  Maximum price levels per side in the underlying ArrayBook.
///                    Defaults to 20.
///
/// @note  Executed / cancelled shares are clamped to the order's remaining
///        quantity to guard against exchange over-execution race conditions.
template <std::size_t MaxLevels = 20>
class ItchBookBuilder {
public:
    /// @brief Process a single ITCH message and update the book accordingly.
    ///
    /// Dispatches to the appropriate handler based on `msg.msg_type`.
    /// Unrecognized message types are silently ignored (returns `false`).
    ///
    /// @param msg  A parsed ITCH message view (type byte + raw data pointer).
    /// @return `true` if the book was modified, `false` if the message was
    ///         ignored or the referenced order was not found.
    bool process(const eph::itch::MessageView& msg) noexcept {
        switch (msg.msg_type) {
        case eph::itch::kAddOrder:     return handle_add_order(msg.data);
        case eph::itch::kAddOrderMPID: return handle_add_order_mpid(msg.data);
        case eph::itch::kOrderExecuted:         return handle_order_executed(msg.data);
        case eph::itch::kOrderExecutedWithPrice: return handle_order_executed_with_price(msg.data);
        case eph::itch::kOrderCancel:  return handle_order_cancel(msg.data);
        case eph::itch::kOrderDelete:  return handle_order_delete(msg.data);
        case eph::itch::kOrderReplace: return handle_order_replace(msg.data);
        default:
            SPDLOG_LOGGER_TRACE(detail::itch_adapter_logger(), "ItchBookBuilder: ignoring msg_type=0x{:02x}", msg.msg_type);
            return false;
        }
    }

    /// @brief Get the current book state (const).
    /// @return Const reference to the underlying ArrayBook.
    [[nodiscard]] const ArrayBook<MaxLevels>& book() const noexcept { return book_; }

    /// @brief Get the current book state (mutable).
    /// @return Mutable reference to the underlying ArrayBook.
    [[nodiscard]] ArrayBook<MaxLevels>& book() noexcept { return book_; }

    /// @brief Number of live orders currently tracked in the internal order map.
    /// @return Count of orders that have been added but not fully executed,
    ///         cancelled, or deleted.
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }

    /// @brief Clear all tracked orders, quantity accumulators, and the book.
    ///
    /// After this call, order_count() == 0 and book().level_count() == 0.
    void clear() noexcept {
        orders_.clear();
        bid_qty_.clear();
        ask_qty_.clear();
        book_.clear();
        SPDLOG_LOGGER_DEBUG(detail::itch_adapter_logger(), "ItchBookBuilder cleared: orders={} levels={}",
                     orders_.size(), book_.level_count());
    }

private:
    ArrayBook<MaxLevels> book_;
    std::unordered_map<uint64_t, Order> orders_;

    /// Per-price aggregated quantity for O(1) incremental updates.
    /// Key: price (quantized via quantize()), Value: total qty at that level.
    std::map<double, double> bid_qty_;
    std::map<double, double> ask_qty_;

    // -- Epsilon for floating-point price comparison --------------------------
    static constexpr double kEps = 1e-12;

    /// Quantization step for price keys (matches MapBook).
    static constexpr double kTickQuantum = 1e-9;

    [[nodiscard]] static bool price_eq(double a, double b) noexcept {
        return std::fabs(a - b) < kEps;
    }

    /// Snap price to canonical representation for use as hash key.
    [[nodiscard]] static double quantize(double p) noexcept {
        return std::round(p / kTickQuantum) * kTickQuantum;
    }

    /// Get the per-price qty map for a side.
    [[nodiscard]] auto& qty_map(char side) noexcept {
        return side == 'B' ? bid_qty_ : ask_qty_;
    }

    /// Add qty to a price level and push to book. O(1).
    ///
    /// Both the internal qty_map AND the downstream `book_.update_*` are
    /// keyed on the quantized price (`qp`). Passing the raw `price` to the
    /// book risked diverging keys: ITCH feeds always derive prices from
    /// `int / 10000.0` so today the two are bit-identical, but the qty_map
    /// would silently aggregate near-duplicates while the book installed
    /// them as distinct levels under the raw price — leaving the
    /// previous level's stale qty visible to consumers (matching the
    /// safety rationale in MapBook::quantize and the BinanceBookAdapter
    /// canonical-key bookkeeping).
    void add_qty(double price, double qty, char side) noexcept {
        double qp = quantize(price);
        auto& m = qty_map(side);
        double& total = m[qp];
        total += qty;
        SPDLOG_LOGGER_TRACE(detail::itch_adapter_logger(), "add_qty side={} price={} qp={} delta={} total={}", side, price, qp, qty, total);
        if (side == 'B') {
            book_.update_bid(qp, total);
        } else {
            book_.update_ask(qp, total);
        }
    }

    /// Subtract qty from a price level and push to book. O(1).
    /// Removes the level from the map if total reaches zero.
    /// See add_qty for the canonical-price (qp) rationale.
    void sub_qty(double price, double qty, char side) noexcept {
        double qp = quantize(price);
        auto& m = qty_map(side);
        auto it = m.find(qp);
        if (it == m.end()) {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(), "sub_qty: price={} qp={} side={} not in qty map", price, qp, side);
            if (side == 'B') book_.update_bid(qp, 0.0);
            else             book_.update_ask(qp, 0.0);
            return;
        }
        it->second -= qty;
        double total = it->second;
        if (total <= 0.0) {
            m.erase(it);
            total = 0.0;
        }
        SPDLOG_LOGGER_TRACE(detail::itch_adapter_logger(), "sub_qty side={} price={} qp={} delta={} total={}", side, price, qp, qty, total);
        if (side == 'B') {
            book_.update_bid(qp, total);
        } else {
            book_.update_ask(qp, total);
        }
    }

    // -- AddOrder ('A') -------------------------------------------------------
    bool handle_add_order(const uint8_t* msg) noexcept {
        const uint64_t ref   = eph::itch::add_order::order_ref(msg);
        const char     side  = eph::itch::add_order::side(msg);
        const uint32_t shares = eph::itch::add_order::shares(msg);
        const double   price = eph::itch::add_order::price(msg);

        SPDLOG_LOGGER_DEBUG(detail::itch_adapter_logger(), "AddOrder ref={} side={} shares={} price={}", ref, side, shares, price);

        // ITCH wire format does not validate the side byte — a corrupt /
        // adversarial feed could send any value. qty_map() treats any
        // non-'B' as ask, which would silently route bids onto the ask
        // side and corrupt every subsequent BBO read. Reject up-front.
        if (side != 'B' && side != 'S') [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(),
                "AddOrder ref={} invalid side=0x{:02x} (expected 'B' or 'S') — dropping",
                ref, static_cast<uint8_t>(side));
            return false;
        }

        // ITCH 5.0 §4 says order_ref is session-unique, but real feeds
        // occasionally retransmit AddOrder for the same ref during
        // gap-recovery / cancel-replace races. Treat re-add as
        // remove-old + add-new so the per-price aggregation map and
        // orders_ map stay consistent — the previous unconditional
        // orders_[ref] = ... left phantom qty on the old level.
        evict_existing_ref(ref, "AddOrder");

        orders_[ref] = Order{price, static_cast<double>(shares), side};
        add_qty(price, static_cast<double>(shares), side);
        return true;
    }

    // -- AddOrderMPID ('F') ---------------------------------------------------
    bool handle_add_order_mpid(const uint8_t* msg) noexcept {
        const uint64_t ref   = eph::itch::add_order_mpid::order_ref(msg);
        const char     side  = eph::itch::add_order_mpid::side(msg);
        const uint32_t shares = eph::itch::add_order_mpid::shares(msg);
        const double   price = eph::itch::add_order_mpid::price(msg);

        SPDLOG_LOGGER_DEBUG(detail::itch_adapter_logger(), "AddOrderMPID ref={} side={} shares={} price={}", ref, side, shares, price);

        // See handle_add_order for rationale.
        if (side != 'B' && side != 'S') [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(),
                "AddOrderMPID ref={} invalid side=0x{:02x} (expected 'B' or 'S') — dropping",
                ref, static_cast<uint8_t>(side));
            return false;
        }

        // Same duplicate-ref guard as handle_add_order — see comment there.
        evict_existing_ref(ref, "AddOrderMPID");

        orders_[ref] = Order{price, static_cast<double>(shares), side};
        add_qty(price, static_cast<double>(shares), side);
        return true;
    }

    // -- OrderExecuted ('E') --------------------------------------------------
    bool handle_order_executed(const uint8_t* msg) noexcept {
        const uint64_t ref    = eph::itch::order_executed::order_ref(msg);
        const uint32_t shares = eph::itch::order_executed::executed_shares(msg);

        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(), "OrderExecuted ref={} not found in order map", ref);
            return false;
        }

        auto& ord = it->second;
        const double old_qty = ord.remaining_qty;
        // Clamp executed shares to remaining qty to prevent book level under-count
        // when exchange reports over-execution (e.g., cancel/replace race).
        const double clamped = std::min(static_cast<double>(shares), old_qty);
        if (clamped < static_cast<double>(shares)) {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(), "OrderExecuted ref={} shares={} exceeds remaining={}, clamped to {}",
                        ref, shares, old_qty, clamped);
        }
        ord.remaining_qty -= clamped;
        SPDLOG_LOGGER_DEBUG(detail::itch_adapter_logger(), "OrderExecuted ref={} exec_shares={} qty {}->{}", ref, shares, old_qty, ord.remaining_qty);

        const double price = ord.price;
        const char side = ord.side;

        // Remove fully executed orders from the map before updating level.
        if (ord.remaining_qty <= 0.0) {
            orders_.erase(it);
        }

        sub_qty(price, clamped, side);
        return true;
    }

    // -- OrderExecutedWithPrice ('C') -----------------------------------------
    // Same reduction logic as OrderExecuted; the execution price field is
    // informational and does not change the order's resting price in the book.
    bool handle_order_executed_with_price(const uint8_t* msg) noexcept {
        const uint64_t ref    = eph::itch::order_executed_price::order_ref(msg);
        const uint32_t shares = eph::itch::order_executed_price::executed_shares(msg);

        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(), "OrderExecutedWithPrice ref={} not found in order map", ref);
            return false;
        }

        auto& ord = it->second;
        const double old_qty = ord.remaining_qty;
        const double clamped = std::min(static_cast<double>(shares), old_qty);
        if (clamped < static_cast<double>(shares)) {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(), "OrderExecutedWithPrice ref={} shares={} exceeds remaining={}, clamped to {}",
                        ref, shares, old_qty, clamped);
        }
        ord.remaining_qty -= clamped;
        SPDLOG_LOGGER_DEBUG(detail::itch_adapter_logger(), "OrderExecutedWithPrice ref={} exec_shares={} qty {}->{}", ref, shares, old_qty, ord.remaining_qty);

        const double price = ord.price;
        const char side = ord.side;

        if (ord.remaining_qty <= 0.0) {
            orders_.erase(it);
        }

        sub_qty(price, clamped, side);
        return true;
    }

    // -- OrderCancel ('X') ----------------------------------------------------
    bool handle_order_cancel(const uint8_t* msg) noexcept {
        const uint64_t ref    = eph::itch::order_cancel::order_ref(msg);
        const uint32_t shares = eph::itch::order_cancel::cancelled_shares(msg);

        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(), "OrderCancel ref={} not found in order map", ref);
            return false;
        }

        auto& ord = it->second;
        const double old_qty = ord.remaining_qty;
        const double clamped = std::min(static_cast<double>(shares), old_qty);
        if (clamped < static_cast<double>(shares)) {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(), "OrderCancel ref={} shares={} exceeds remaining={}, clamped to {}",
                        ref, shares, old_qty, clamped);
        }
        ord.remaining_qty -= clamped;
        SPDLOG_LOGGER_DEBUG(detail::itch_adapter_logger(), "OrderCancel ref={} cancel_shares={} qty {}->{}", ref, shares, old_qty, ord.remaining_qty);

        const double price = ord.price;
        const char side = ord.side;

        // Remove fully cancelled orders.
        if (ord.remaining_qty <= 0.0) {
            orders_.erase(it);
        }

        sub_qty(price, clamped, side);
        return true;
    }

    // -- OrderDelete ('D') ----------------------------------------------------
    bool handle_order_delete(const uint8_t* msg) noexcept {
        const uint64_t ref = eph::itch::order_delete::order_ref(msg);

        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(), "OrderDelete ref={} not found in order map", ref);
            return false;
        }

        const double price = it->second.price;
        const char side = it->second.side;
        SPDLOG_LOGGER_DEBUG(detail::itch_adapter_logger(), "OrderDelete ref={} price={} side={}", ref, price, side);

        double rem_qty = it->second.remaining_qty;
        orders_.erase(it);
        sub_qty(price, rem_qty, side);
        return true;
    }

    // -- OrderReplace ('U') ---------------------------------------------------
    // Replace = delete old order + add new order with new ref, shares, and price.
    // The new order inherits the side from the original order.
    bool handle_order_replace(const uint8_t* msg) noexcept {
        const uint64_t orig_ref = eph::itch::order_replace::original_order_ref(msg);
        const uint64_t new_ref  = eph::itch::order_replace::new_order_ref(msg);
        const uint32_t shares   = eph::itch::order_replace::shares(msg);
        const double   new_price = eph::itch::order_replace::price(msg);

        auto it = orders_.find(orig_ref);
        if (it == orders_.end()) {
            SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(), "OrderReplace orig_ref={} not found in order map", orig_ref);
            return false;
        }

        const double old_price = it->second.price;
        const char side = it->second.side;
        SPDLOG_LOGGER_DEBUG(detail::itch_adapter_logger(), "OrderReplace orig_ref={} new_ref={} old_price={} new_price={} shares={}",
                     orig_ref, new_ref, old_price, new_price, shares);

        // Remove old order qty from old level.
        double old_qty = it->second.remaining_qty;
        orders_.erase(it);
        sub_qty(old_price, old_qty, side);

        // Defensive: if new_ref aliases another live order (buggy /
        // adversarial feed), evict that order's qty from the book first
        // — same phantom-qty bug as handle_add_order otherwise.
        evict_existing_ref(new_ref, "OrderReplace.new_ref");

        // Insert the new order and add qty to new level.
        orders_[new_ref] = Order{new_price, static_cast<double>(shares), side};
        add_qty(new_price, static_cast<double>(shares), side);

        return true;
    }

    // ----------------------------------------------------------------------
    // Helper: drain phantom qty if `ref` is already a live order.
    //
    // Keeps the per-price aggregation map (`bid_qty_` / `ask_qty_`) in
    // lockstep with `orders_` even when the feed re-adds a known ref or
    // a replace's new_ref collides with an existing order. Without this
    // step the old level keeps `remaining_qty` in the aggregation map
    // forever, since no future delete/cancel touches the *old* (price,
    // side) any more — they will route through the new state.
    void evict_existing_ref(uint64_t ref, const char* origin) noexcept {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return;
        const double old_price = it->second.price;
        const double old_qty   = it->second.remaining_qty;
        const char   old_side  = it->second.side;
        SPDLOG_LOGGER_WARN(detail::itch_adapter_logger(),
            "{} ref={} already live at price={} side={} qty={} — "
            "evicting old level before applying new state to avoid "
            "phantom qty in the aggregation map",
            origin, ref, old_price, old_side, old_qty);
        orders_.erase(it);
        sub_qty(old_price, old_qty, old_side);
    }
};

} // namespace eph::book
