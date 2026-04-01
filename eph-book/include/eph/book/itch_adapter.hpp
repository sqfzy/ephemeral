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
        bid_qty_.clear();
        ask_qty_.clear();
        book_.clear();
        SPDLOG_DEBUG("ItchBookBuilder cleared: orders={} levels={}",
                     orders_.size(), book_.level_count());
    }

private:
    ArrayBook<MaxLevels> book_;
    std::unordered_map<uint64_t, Order> orders_;

    /// Per-price aggregated quantity for O(1) incremental updates.
    /// Key: price (quantized via quantize()), Value: total qty at that level.
    std::unordered_map<double, double> bid_qty_;
    std::unordered_map<double, double> ask_qty_;

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
    void add_qty(double price, double qty, char side) noexcept {
        double qp = quantize(price);
        auto& m = qty_map(side);
        double& total = m[qp];
        total += qty;
        SPDLOG_TRACE("add_qty side={} price={} delta={} total={}", side, price, qty, total);
        if (side == 'B') {
            book_.update_bid(price, total);
        } else {
            book_.update_ask(price, total);
        }
    }

    /// Subtract qty from a price level and push to book. O(1).
    /// Removes the level from the map if total reaches zero.
    void sub_qty(double price, double qty, char side) noexcept {
        double qp = quantize(price);
        auto& m = qty_map(side);
        auto it = m.find(qp);
        if (it == m.end()) {
            SPDLOG_WARN("sub_qty: price={} side={} not in qty map", price, side);
            if (side == 'B') book_.update_bid(price, 0.0);
            else             book_.update_ask(price, 0.0);
            return;
        }
        it->second -= qty;
        double total = it->second;
        if (total <= 0.0) {
            m.erase(it);
            total = 0.0;
        }
        SPDLOG_TRACE("sub_qty side={} price={} delta={} total={}", side, price, qty, total);
        if (side == 'B') {
            book_.update_bid(price, total);
        } else {
            book_.update_ask(price, total);
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
        add_qty(price, static_cast<double>(shares), side);
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
        add_qty(price, static_cast<double>(shares), side);
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

        // Remove fully executed orders from the map before updating level.
        if (ord.remaining_qty <= 0.0) {
            orders_.erase(it);
        }

        sub_qty(price, static_cast<double>(shares), side);
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

        sub_qty(price, static_cast<double>(shares), side);
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

        sub_qty(price, static_cast<double>(shares), side);
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
            SPDLOG_WARN("OrderReplace orig_ref={} not found in order map", orig_ref);
            return false;
        }

        const double old_price = it->second.price;
        const char side = it->second.side;
        SPDLOG_DEBUG("OrderReplace orig_ref={} new_ref={} old_price={} new_price={} shares={}",
                     orig_ref, new_ref, old_price, new_price, shares);

        // Remove old order qty from old level.
        double old_qty = it->second.remaining_qty;
        orders_.erase(it);
        sub_qty(old_price, old_qty, side);

        // Insert the new order and add qty to new level.
        orders_[new_ref] = Order{new_price, static_cast<double>(shares), side};
        add_qty(new_price, static_cast<double>(shares), side);

        return true;
    }
};

} // namespace eph::book
