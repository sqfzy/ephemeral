#pragma once

/// @file orders.hpp
/// Typed builder wrappers for common HFT order entry FIX messages.
///
/// Wraps MessageBuilder with strongly-typed interfaces for NewOrderSingle (D),
/// OrderCancelRequest (F), and OrderCancelReplaceRequest (G). Each function
/// builds a complete, finalized FIX message into a caller-provided buffer.
/// No heap allocations.

#include <chrono>
#include <cstdint>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/fix/builder.hpp"
#include "eph/fix/tags.hpp"

namespace eph::fix {

namespace detail {
inline spdlog::logger* fix_orders_logger() noexcept {
    static auto l = [] {
        auto lg = spdlog::get("fix.orders");
        if (!lg) lg = spdlog::stdout_color_mt("fix.orders");
        return lg;
    }();
    return l.get();
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

/// FIX Side (tag 54)
enum class Side : char { Buy = '1', Sell = '2' };

/// FIX Order type (tag 40)
enum class OrdType : char { Market = '1', Limit = '2' };

/// FIX Time in force (tag 59)
enum class TimeInForce : char {
    Day = '0',
    GTC = '1',
    IOC = '3',
    FOK = '4',
};

/// Build a NewOrderSingle (MsgType=D) message.
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
    SPDLOG_LOGGER_DEBUG(detail::fix_orders_logger(),
        "build_new_order: cl_ord_id={}, symbol={}, side={}, ord_type={}, qty={}, price={}",
        cl_ord_id, symbol, static_cast<char>(side), static_cast<char>(ord_type), qty, price);

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
        b.set_double(tag::Price, price);
    }

    size_t len = b.finish();
    if (len == 0) [[unlikely]] {
        SPDLOG_LOGGER_WARN(detail::fix_orders_logger(),
            "build_new_order: failed (overflow or error), cl_ord_id={}", cl_ord_id);
    }
    return len;
}

/// Build an OrderCancelRequest (MsgType=F) message.
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
    SPDLOG_LOGGER_DEBUG(detail::fix_orders_logger(),
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
        SPDLOG_LOGGER_WARN(detail::fix_orders_logger(),
            "build_cancel_order: failed (overflow or error), cl_ord_id={}", cl_ord_id);
    }
    return len;
}

/// Build an OrderCancelReplaceRequest (MsgType=G) message.
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
    SPDLOG_LOGGER_DEBUG(detail::fix_orders_logger(),
        "build_replace_order: cl_ord_id={}, orig_cl_ord_id={}, symbol={}, side={}, qty={}, price={}",
        cl_ord_id, orig_cl_ord_id, symbol, static_cast<char>(side), qty, price);

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
        b.set_double(tag::Price, price);
    }

    size_t len = b.finish();
    if (len == 0) [[unlikely]] {
        SPDLOG_LOGGER_WARN(detail::fix_orders_logger(),
            "build_replace_order: failed (overflow or error), cl_ord_id={}", cl_ord_id);
    }
    return len;
}

} // namespace eph::fix
