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
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "eph/book/array_book.hpp"
#include "eph/itch/messages.hpp"
#include "eph/itch/parser.hpp"

namespace eph::book {

// ---------------------------------------------------------------------------
// Order — per-order state tracked by the adapter
// ---------------------------------------------------------------------------

struct Order {
    double price;
    double remaining_qty;
    char   side;  // 'B' = buy, 'S' = sell
};

// ---------------------------------------------------------------------------
// ItchBookBuilder — ITCH-to-L2 book adapter
// ---------------------------------------------------------------------------

/// Converts order-level ITCH 5.0 events into aggregated price-level updates
/// for an ArrayBook.
///
/// @tparam MaxLevels  Maximum price levels per side in the underlying ArrayBook.
template <std::size_t MaxLevels = 20>
class ItchBookBuilder {
public:
    /// Process an ITCH message and update the book.
    /// Returns true if the book was modified.
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
            SPDLOG_TRACE("ItchBookBuilder: ignoring msg_type=0x{:02x}", msg.msg_type);
            return false;
        }
    }

    /// Get the current book state (const).
    [[nodiscard]] const ArrayBook<MaxLevels>& book() const noexcept { return book_; }

    /// Get the current book state (mutable).
    [[nodiscard]] ArrayBook<MaxLevels>& book() noexcept { return book_; }

    /// Number of tracked live orders.
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }

    /// Clear all orders and the book.
    void clear() noexcept {
        orders_.clear();
        book_.clear();
        SPDLOG_DEBUG("ItchBookBuilder cleared: orders={} levels={}",
                     orders_.size(), book_.level_count());
    }

private:
    ArrayBook<MaxLevels> book_;
    std::unordered_map<uint64_t, Order> orders_;

    // -- Epsilon for floating-point price comparison --------------------------
    static constexpr double kEps = 1e-12;

    [[nodiscard]] static bool price_eq(double a, double b) noexcept {
        return std::fabs(a - b) < kEps;
    }

    // -- Recalculate total qty at a price level by scanning all orders --------
    // O(n) over the order map, but correct.  For L2 books with ~20 levels and
    // moderate order counts this is acceptable.
    void recalculate_level(double price, char side) noexcept {
        double total_qty = 0.0;
        for (const auto& [ref, ord] : orders_) {
            if (ord.side == side && price_eq(ord.price, price)) {
                total_qty += ord.remaining_qty;
            }
        }
        SPDLOG_TRACE("recalculate_level side={} price={} total_qty={}", side, price, total_qty);
        if (side == 'B') {
            book_.update_bid(price, total_qty);
        } else {
            book_.update_ask(price, total_qty);
        }
    }

    // -- AddOrder ('A') -------------------------------------------------------
    bool handle_add_order(const uint8_t* msg) noexcept {
        const uint64_t ref   = eph::itch::add_order::order_ref(msg);
        const char     side  = eph::itch::add_order::side(msg);
        const uint32_t shares = eph::itch::add_order::shares(msg);
        const double   price = eph::itch::add_order::price(msg);

        SPDLOG_DEBUG("AddOrder ref={} side={} shares={} price={}", ref, side, shares, price);

        orders_[ref] = Order{price, static_cast<double>(shares), side};
        recalculate_level(price, side);
        return true;
    }

    // -- AddOrderMPID ('F') ---------------------------------------------------
    bool handle_add_order_mpid(const uint8_t* msg) noexcept {
        const uint64_t ref   = eph::itch::add_order_mpid::order_ref(msg);
        const char     side  = eph::itch::add_order_mpid::side(msg);
        const uint32_t shares = eph::itch::add_order_mpid::shares(msg);
        const double   price = eph::itch::add_order_mpid::price(msg);

        SPDLOG_DEBUG("AddOrderMPID ref={} side={} shares={} price={}", ref, side, shares, price);

        orders_[ref] = Order{price, static_cast<double>(shares), side};
        recalculate_level(price, side);
        return true;
    }

    // -- OrderExecuted ('E') --------------------------------------------------
    bool handle_order_executed(const uint8_t* msg) noexcept {
        const uint64_t ref    = eph::itch::order_executed::order_ref(msg);
        const uint32_t shares = eph::itch::order_executed::executed_shares(msg);

        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            SPDLOG_WARN("OrderExecuted ref={} not found in order map", ref);
            return false;
        }

        auto& ord = it->second;
        [[maybe_unused]] const double old_qty = ord.remaining_qty;
        ord.remaining_qty -= static_cast<double>(shares);
        SPDLOG_DEBUG("OrderExecuted ref={} exec_shares={} qty {}->{}", ref, shares, old_qty, ord.remaining_qty);

        const double price = ord.price;
        const char side = ord.side;

        // Remove fully executed orders from the map before recalculating.
        if (ord.remaining_qty <= 0.0) {
            orders_.erase(it);
        }

        recalculate_level(price, side);
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
            SPDLOG_WARN("OrderExecutedWithPrice ref={} not found in order map", ref);
            return false;
        }

        auto& ord = it->second;
        [[maybe_unused]] const double old_qty = ord.remaining_qty;
        ord.remaining_qty -= static_cast<double>(shares);
        SPDLOG_DEBUG("OrderExecutedWithPrice ref={} exec_shares={} qty {}->{}", ref, shares, old_qty, ord.remaining_qty);

        const double price = ord.price;
        const char side = ord.side;

        if (ord.remaining_qty <= 0.0) {
            orders_.erase(it);
        }

        recalculate_level(price, side);
        return true;
    }

    // -- OrderCancel ('X') ----------------------------------------------------
    bool handle_order_cancel(const uint8_t* msg) noexcept {
        const uint64_t ref    = eph::itch::order_cancel::order_ref(msg);
        const uint32_t shares = eph::itch::order_cancel::cancelled_shares(msg);

        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            SPDLOG_WARN("OrderCancel ref={} not found in order map", ref);
            return false;
        }

        auto& ord = it->second;
        [[maybe_unused]] const double old_qty = ord.remaining_qty;
        ord.remaining_qty -= static_cast<double>(shares);
        SPDLOG_DEBUG("OrderCancel ref={} cancel_shares={} qty {}->{}", ref, shares, old_qty, ord.remaining_qty);

        const double price = ord.price;
        const char side = ord.side;

        // Remove fully cancelled orders.
        if (ord.remaining_qty <= 0.0) {
            orders_.erase(it);
        }

        recalculate_level(price, side);
        return true;
    }

    // -- OrderDelete ('D') ----------------------------------------------------
    bool handle_order_delete(const uint8_t* msg) noexcept {
        const uint64_t ref = eph::itch::order_delete::order_ref(msg);

        auto it = orders_.find(ref);
        if (it == orders_.end()) {
            SPDLOG_WARN("OrderDelete ref={} not found in order map", ref);
            return false;
        }

        const double price = it->second.price;
        const char side = it->second.side;
        SPDLOG_DEBUG("OrderDelete ref={} price={} side={}", ref, price, side);

        orders_.erase(it);
        recalculate_level(price, side);
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
            SPDLOG_WARN("OrderReplace orig_ref={} not found in order map", orig_ref);
            return false;
        }

        const double old_price = it->second.price;
        const char side = it->second.side;
        SPDLOG_DEBUG("OrderReplace orig_ref={} new_ref={} old_price={} new_price={} shares={}",
                     orig_ref, new_ref, old_price, new_price, shares);

        // Remove the old order.
        orders_.erase(it);

        // Insert the new order.
        orders_[new_ref] = Order{new_price, static_cast<double>(shares), side};

        // Recalculate both the old and new price levels.
        recalculate_level(old_price, side);
        if (!price_eq(old_price, new_price)) {
            recalculate_level(new_price, side);
        }

        return true;
    }
};

} // namespace eph::book
