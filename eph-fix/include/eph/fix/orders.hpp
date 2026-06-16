#pragma once

/// @file orders.hpp
/// Typed builder wrappers for common HFT order entry FIX messages.
///
/// Wraps MessageBuilder with strongly-typed interfaces for NewOrderSingle (D),
/// OrderCancelRequest (F), and OrderCancelReplaceRequest (G). Each function
/// builds a complete, finalized FIX message into a caller-provided buffer.
/// No heap allocations.

#include <chrono>
#include <cmath>     // isfinite for qty validation in build_new_order
#include <cstdint>
#include <format>
#include <string_view>

#include "eph/core/log.hpp"

#include "eph/fix/builder.hpp"
#include "eph/fix/tags.hpp"

namespace eph::fix {

/// @brief Internal implementation details for the orders module.
namespace detail {
/// @brief Get or create the spdlog logger for the orders module.
/// @return Raw pointer to the "fix.orders" logger (never null after first call).
inline spdlog::logger* fix_orders_logger() noexcept {
    static spdlog::logger* l = ::eph::log::get("fix.orders");
    return l;
}

/// Get current wall-clock time as nanoseconds since Unix epoch.
inline uint64_t now_epoch_ns() noexcept {
    auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch())
            .count());
}
} // namespace detail

/// @brief FIX Side (tag 54) -- direction of the order.
enum class Side : char {
    Buy  = '1',  ///< Buy side ('1').
    Sell = '2',  ///< Sell side ('2').
};

/// Human-readable name for Side.
[[nodiscard]] constexpr std::string_view side_name(Side s) noexcept {
    switch (s) {
    case Side::Buy:  return "Buy";
    case Side::Sell: return "Sell";
    }
    return "Unknown";
}

/// @brief FIX Order type (tag 40) -- how the order is to be executed.
enum class OrdType : char {
    Market = '1',  ///< Market order -- fill immediately at best available price.
    Limit  = '2',  ///< Limit order -- fill at the specified price or better.
};

/// Human-readable name for OrdType.
[[nodiscard]] constexpr std::string_view ord_type_name(OrdType t) noexcept {
    switch (t) {
    case OrdType::Market: return "Market";
    case OrdType::Limit:  return "Limit";
    }
    return "Unknown";
}

/// @brief FIX Time in force (tag 59) -- how long the order remains active.
enum class TimeInForce : char {
    Day = '0',  ///< Day order -- expires at end of trading day.
    GTC = '1',  ///< Good Till Cancel -- remains active until explicitly canceled.
    IOC = '3',  ///< Immediate Or Cancel -- fill what you can, cancel the rest.
    FOK = '4',  ///< Fill Or Kill -- fill entirely or cancel entirely.
};

/// Human-readable name for TimeInForce.
[[nodiscard]] constexpr std::string_view time_in_force_name(TimeInForce tif) noexcept {
    switch (tif) {
    case TimeInForce::Day: return "Day";
    case TimeInForce::GTC: return "GTC";
    case TimeInForce::IOC: return "IOC";
    case TimeInForce::FOK: return "FOK";
    }
    return "Unknown";
}

/// @brief Build a NewOrderSingle (MsgType=D) message.
///
/// Populates: MsgType, SenderCompID, TargetCompID, ClOrdID, Symbol, Side,
/// TransactTime, OrderQty, OrdType, TimeInForce, and optionally Price
/// (for Limit orders). HandlInst is set to '1' (automated, no intervention).
///
/// @param buf              Output buffer (caller-owned)
/// @param capacity         Buffer size in bytes
/// @param sender_comp_id   Sender identification
/// @param target_comp_id   Target identification
/// @param cl_ord_id        Client order ID (unique per order)
/// @param symbol           Instrument symbol
/// @param side             Buy or Sell
/// @param ord_type         Market or Limit
/// @param qty              Order quantity
/// @param price            Limit price (ignored for Market orders)
/// @param tif              Time in force (default Day)
/// @param sending_time_ns  Epoch nanoseconds for SendingTime (0 = current time)
/// @return Bytes written to buf, or 0 on overflow / error
[[nodiscard]] inline size_t build_new_order(
    uint8_t* buf, size_t capacity,
    std::string_view sender_comp_id,
    std::string_view target_comp_id,
    std::string_view cl_ord_id,
    std::string_view symbol,
    Side side,
    OrdType ord_type,
    double qty,
    double price = 0.0,
    TimeInForce tif = TimeInForce::Day,
    uint64_t sending_time_ns = 0) noexcept
{
    EPH_LOG_DEBUG(detail::fix_orders_logger(),
        "build_new_order: cl_ord_id={}, symbol={}, side={}, ord_type={}, qty={}, price={}",
        cl_ord_id, symbol, static_cast<char>(side), static_cast<char>(ord_type), qty, price);

    // Reject pathological qty up-front. set_double() further down catches
    // non-finite (sets overflow_, finish() returns 0), but lets through
    // qty=0 and negative qty silently — which then ends up on the wire as
    // "0.00" or "-1.00", an invalid OrderQty per FIX 4.4 §F.2 that the
    // venue rejects with BusinessMessageReject. Surface as a build-time
    // failure instead so the caller gets a clear "qty must be > 0" log
    // line at submission rather than a confusing exchange reject seconds
    // later. Same isfinite-first ordering as the sibling guards in
    // PositionTracker / RiskChecker / OrderManager.
    if (!std::isfinite(qty) || qty <= 0.0) [[unlikely]] {
        EPH_LOG_WARN(detail::fix_orders_logger(),
            "build_new_order: rejected qty={} (must be finite and > 0) "
            "for cl_ord_id={} symbol={}", qty, cl_ord_id, symbol);
        return 0;
    }

    MessageBuilder b(buf, capacity);

    b.set_char(tag::MsgType, tag::msg_type::NewOrderSingle);
    b.set(tag::SenderCompID, sender_comp_id);
    b.set(tag::TargetCompID, target_comp_id);
    b.set(tag::ClOrdID, cl_ord_id);

    // HandlInst=1: automated execution, no broker intervention
    b.set_char(tag::HandlInst, '1');
    b.set(tag::Symbol, symbol);
    b.set_char(tag::Side, static_cast<char>(side));

    uint64_t ts = sending_time_ns != 0 ? sending_time_ns : detail::now_epoch_ns();
    b.set_timestamp(tag::TransactTime, ts);
    b.set_timestamp(tag::SendingTime, ts);

    b.set_double(tag::OrderQty, qty);
    b.set_char(tag::OrdType, static_cast<char>(ord_type));
    b.set_char(tag::TimeInForce, static_cast<char>(tif));

    // Price only for Limit orders — Market orders must not include tag 44
    if (ord_type == OrdType::Limit) {
        // isfinite-first ordering: every NaN comparison is false, so a
        // NaN price slips past `<= 0.0` and the warning never logs —
        // builder.set_double would still mark overflow downstream, but
        // the caller would see "Limit order failed (overflow or error)"
        // without the actionable "non-positive price" diagnostic. Surface
        // NaN/Inf in the same WARN so log readers can fix the right knob.
        if (!std::isfinite(price) || price <= 0.0) {
            EPH_LOG_WARN(detail::fix_orders_logger(),
                "build_new_order: Limit order with invalid price={} "
                "(must be finite and > 0), cl_ord_id={}",
                price, cl_ord_id);
        }
        b.set_double(tag::Price, price);
    } else if (ord_type == OrdType::Market && price != 0.0) {
        EPH_LOG_DEBUG(detail::fix_orders_logger(),
            "build_new_order: Market order ignoring price={}, cl_ord_id={}",
            price, cl_ord_id);
    }

    size_t len = b.finish();
    if (len == 0) [[unlikely]] {
        EPH_LOG_WARN(detail::fix_orders_logger(),
            "build_new_order: failed (overflow or error), cl_ord_id={}", cl_ord_id);
    }
    return len;
}

/// @brief Build an OrderCancelRequest (MsgType=F) message.
///
/// Populates: MsgType, SenderCompID, TargetCompID, ClOrdID, OrigClOrdID,
/// Symbol, Side, TransactTime.
///
/// @param buf              Output buffer (caller-owned)
/// @param capacity         Buffer size in bytes
/// @param sender_comp_id   Sender identification
/// @param target_comp_id   Target identification
/// @param cl_ord_id        New client order ID for this cancel request
/// @param orig_cl_ord_id   ClOrdID of the order to cancel
/// @param symbol           Instrument symbol
/// @param side             Side of the original order
/// @return Bytes written to buf, or 0 on overflow / error
[[nodiscard]] inline size_t build_cancel_order(
    uint8_t* buf, size_t capacity,
    std::string_view sender_comp_id,
    std::string_view target_comp_id,
    std::string_view cl_ord_id,
    std::string_view orig_cl_ord_id,
    std::string_view symbol,
    Side side) noexcept
{
    EPH_LOG_DEBUG(detail::fix_orders_logger(),
        "build_cancel_order: cl_ord_id={}, orig_cl_ord_id={}, symbol={}, side={}",
        cl_ord_id, orig_cl_ord_id, symbol, static_cast<char>(side));

    MessageBuilder b(buf, capacity);

    b.set_char(tag::MsgType, tag::msg_type::OrderCancelRequest);
    b.set(tag::SenderCompID, sender_comp_id);
    b.set(tag::TargetCompID, target_comp_id);
    b.set(tag::ClOrdID, cl_ord_id);
    b.set(tag::OrigClOrdID, orig_cl_ord_id);
    b.set(tag::Symbol, symbol);
    b.set_char(tag::Side, static_cast<char>(side));

    uint64_t ts = detail::now_epoch_ns();
    b.set_timestamp(tag::TransactTime, ts);
    b.set_timestamp(tag::SendingTime, ts);

    size_t len = b.finish();
    if (len == 0) [[unlikely]] {
        EPH_LOG_WARN(detail::fix_orders_logger(),
            "build_cancel_order: failed (overflow or error), cl_ord_id={}", cl_ord_id);
    }
    return len;
}

/// @brief Build an OrderCancelReplaceRequest (MsgType=G) message.
///
/// Populates: MsgType, SenderCompID, TargetCompID, ClOrdID, OrigClOrdID,
/// Symbol, Side, TransactTime, OrderQty, OrdType, TimeInForce, and
/// optionally Price (for Limit orders). HandlInst=1.
///
/// @param buf              Output buffer (caller-owned)
/// @param capacity         Buffer size in bytes
/// @param sender_comp_id   Sender identification
/// @param target_comp_id   Target identification
/// @param cl_ord_id        New client order ID for this replace
/// @param orig_cl_ord_id   ClOrdID of the order to replace
/// @param symbol           Instrument symbol
/// @param side             Buy or Sell
/// @param ord_type         Market or Limit
/// @param qty              New order quantity
/// @param price            New limit price (ignored for Market orders)
/// @param tif              Time in force (default Day)
/// @return Bytes written to buf, or 0 on overflow / error
[[nodiscard]] inline size_t build_replace_order(
    uint8_t* buf, size_t capacity,
    std::string_view sender_comp_id,
    std::string_view target_comp_id,
    std::string_view cl_ord_id,
    std::string_view orig_cl_ord_id,
    std::string_view symbol,
    Side side,
    OrdType ord_type,
    double qty,
    double price = 0.0,
    TimeInForce tif = TimeInForce::Day) noexcept
{
    EPH_LOG_DEBUG(detail::fix_orders_logger(),
        "build_replace_order: cl_ord_id={}, orig_cl_ord_id={}, symbol={}, side={}, qty={}, price={}",
        cl_ord_id, orig_cl_ord_id, symbol, static_cast<char>(side), qty, price);

    // Mirror build_new_order's qty guard. A replace request with NaN /
    // negative / zero qty has the same downstream consequences: set_double
    // would emit "0.00" or "-1.00" and the venue rejects with
    // BusinessMessageReject seconds later, after the original order is
    // already in flight. Surface as a build-time failure so the caller
    // knows immediately that the replace was never sent. The omission
    // here vs. build_new_order was a copy-paste gap in 2893608f.
    if (!std::isfinite(qty) || qty <= 0.0) [[unlikely]] {
        EPH_LOG_WARN(detail::fix_orders_logger(),
            "build_replace_order: rejected qty={} (must be finite and > 0) "
            "for cl_ord_id={} orig_cl_ord_id={} symbol={}",
            qty, cl_ord_id, orig_cl_ord_id, symbol);
        return 0;
    }

    MessageBuilder b(buf, capacity);

    b.set_char(tag::MsgType, tag::msg_type::OrderCancelReplace);
    b.set(tag::SenderCompID, sender_comp_id);
    b.set(tag::TargetCompID, target_comp_id);
    b.set(tag::ClOrdID, cl_ord_id);
    b.set(tag::OrigClOrdID, orig_cl_ord_id);

    b.set_char(tag::HandlInst, '1');
    b.set(tag::Symbol, symbol);
    b.set_char(tag::Side, static_cast<char>(side));

    uint64_t ts = detail::now_epoch_ns();
    b.set_timestamp(tag::TransactTime, ts);
    b.set_timestamp(tag::SendingTime, ts);

    b.set_double(tag::OrderQty, qty);
    b.set_char(tag::OrdType, static_cast<char>(ord_type));
    b.set_char(tag::TimeInForce, static_cast<char>(tif));

    if (ord_type == OrdType::Limit) {
        // Same isfinite-first ordering as build_new_order: NaN slips
        // past `<= 0.0` and would hide the actionable diagnostic
        // behind the generic "(overflow or error)" line. See the
        // matching guard in build_new_order above for the rationale.
        if (!std::isfinite(price) || price <= 0.0) {
            EPH_LOG_WARN(detail::fix_orders_logger(),
                "build_replace_order: Limit order with invalid price={} "
                "(must be finite and > 0), cl_ord_id={}",
                price, cl_ord_id);
        }
        b.set_double(tag::Price, price);
    } else if (ord_type == OrdType::Market && price != 0.0) {
        EPH_LOG_DEBUG(detail::fix_orders_logger(),
            "build_replace_order: Market order ignoring price={}, cl_ord_id={}",
            price, cl_ord_id);
    }

    size_t len = b.finish();
    if (len == 0) [[unlikely]] {
        EPH_LOG_WARN(detail::fix_orders_logger(),
            "build_replace_order: failed (overflow or error), cl_ord_id={}", cl_ord_id);
    }
    return len;
}

} // namespace eph::fix

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations for FIX order enums
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::fix::Side> : std::formatter<std::string_view> {
    auto format(eph::fix::Side s, auto& ctx) const {
        return std::formatter<std::string_view>::format(eph::fix::side_name(s), ctx);
    }
};

template <>
struct std::formatter<eph::fix::OrdType> : std::formatter<std::string_view> {
    auto format(eph::fix::OrdType t, auto& ctx) const {
        return std::formatter<std::string_view>::format(eph::fix::ord_type_name(t), ctx);
    }
};

template <>
struct std::formatter<eph::fix::TimeInForce> : std::formatter<std::string_view> {
    auto format(eph::fix::TimeInForce tif, auto& ctx) const {
        return std::formatter<std::string_view>::format(eph::fix::time_in_force_name(tif), ctx);
    }
};
