#pragma once

/// @file execution_report.hpp
/// Zero-copy accessors for the Binance spot SBE `ExecutionReportEvent`
/// (template id 603, schema 3:2) — the user-data-stream order lifecycle / fill
/// event (the SBE equivalent of the JSON `executionReport`). Pushed unsolicited
/// (NOT wrapped in a `WebSocketResponse`) on a `userDataStream.subscribe`d
/// connection.
///
/// Authoritative layout (spot_3_2.xml, `<sbe:message id="603">`):
///   fixed block (blockLength = 281): 47 fields. Offsets of the consumed subset
///   (computed from the schema field order/types; price/qty are mantissa64 paired
///   with the exponent fields):
///     +0   eventTime           utcTimestampUs (int64, µs)
///     +8   transactTime        utcTimestampUs (int64, µs)
///     +16  priceExponent       exponent8 (int8)
///     +17  qtyExponent         exponent8 (int8)
///     +35  orderId             orderId (int64)
///     +94  executionType       executionType (uint8)
///     +95  orderStatus         orderStatus (uint8)
///     +104 executionId         executionId (int64)   — dedup key
///     +112 executedQty         mantissa64 (cumulative filled; "z")
///     +120 cummulativeQuoteQty mantissa64 (cumulative quote; "Z")
///     +136 lastPrice           mantissa64 ("L")
///   data id=200 symbol        varString8
///   data id=201 clientOrderId varString8   (our cid)
///   (further optionalVarString8 fields trail; not consumed here)

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "eph/sbe/binance/schema.hpp"
#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/parser.hpp"

namespace eph::sbe::binance {

namespace execution_report {

inline constexpr std::size_t kOffEventTime     = 0;
inline constexpr std::size_t kOffTransactTime  = 8;
inline constexpr std::size_t kOffPriceExponent = 16;
inline constexpr std::size_t kOffQtyExponent   = 17;
inline constexpr std::size_t kOffOrderId       = 35;
inline constexpr std::size_t kOffExecutionType = 94;
inline constexpr std::size_t kOffOrderStatus   = 95;
inline constexpr std::size_t kOffExecutionId   = 104;
inline constexpr std::size_t kOffExecutedQty   = 112;
inline constexpr std::size_t kOffCumQuoteQty   = 120;
inline constexpr std::size_t kOffLastPrice     = 136;

[[nodiscard]] inline int64_t event_time_us(const MessageView& v) noexcept {
    return read_le_i64(v.body() + kOffEventTime);
}
[[nodiscard]] inline int64_t transact_time_us(const MessageView& v) noexcept {
    return read_le_i64(v.body() + kOffTransactTime);
}
[[nodiscard]] inline int8_t price_exponent(const MessageView& v) noexcept {
    return read_le_i8(v.body() + kOffPriceExponent);
}
[[nodiscard]] inline int8_t qty_exponent(const MessageView& v) noexcept {
    return read_le_i8(v.body() + kOffQtyExponent);
}
[[nodiscard]] inline int64_t order_id(const MessageView& v) noexcept {
    return read_le_i64(v.body() + kOffOrderId);
}
/// @brief Raw executionType byte (e.g. NEW/TRADE/CANCELED/REJECTED enum).
[[nodiscard]] inline uint8_t execution_type(const MessageView& v) noexcept {
    return v.body()[kOffExecutionType];
}
/// @brief Raw orderStatus byte (see binance OrderStatus enum; 0=New … 2=Filled).
[[nodiscard]] inline uint8_t order_status(const MessageView& v) noexcept {
    return v.body()[kOffOrderStatus];
}
/// @brief Unique execution id — the natural cross-path dedup key.
[[nodiscard]] inline int64_t execution_id(const MessageView& v) noexcept {
    return read_le_i64(v.body() + kOffExecutionId);
}
/// @brief Cumulative filled quantity (decoded decimal: executedQty × 10^qtyExp).
[[nodiscard]] inline double executed_qty(const MessageView& v) noexcept {
    return decode_decimal(read_le_i64(v.body() + kOffExecutedQty), qty_exponent(v));
}
/// @brief Cumulative quote quantity (Z) — avg price = cumQuoteQty / executedQty.
[[nodiscard]] inline double cummulative_quote_qty(const MessageView& v) noexcept {
    return decode_decimal(read_le_i64(v.body() + kOffCumQuoteQty), price_exponent(v));
}
/// @brief Last fill price (L), decoded decimal.
[[nodiscard]] inline double last_price(const MessageView& v) noexcept {
    return decode_decimal(read_le_i64(v.body() + kOffLastPrice), price_exponent(v));
}
/// @brief Trading symbol (1st trailing varString8 at on-wire block_length).
[[nodiscard]] inline std::string_view symbol(const MessageView& v) noexcept {
    const std::size_t off = v.block_length;
    if (off > v.body_len()) [[unlikely]] return {};
    auto s = read_var_string8(v.body() + off, v.body_len() - off);
    return s ? s->value : std::string_view{};
}
/// @brief Client order id / cid — the 2nd trailing varString8 (after symbol).
[[nodiscard]] inline std::string_view client_order_id(const MessageView& v) noexcept {
    std::size_t off = v.block_length;
    if (off > v.body_len()) [[unlikely]] return {};
    auto sym = read_var_string8(v.body() + off, v.body_len() - off);
    if (!sym) [[unlikely]] return {};
    off += sym->advance;
    auto cid = read_var_string8(v.body() + off, v.body_len() - off);
    return cid ? cid->value : std::string_view{};
}

} // namespace execution_report

} // namespace eph::sbe::binance
