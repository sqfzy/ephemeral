#pragma once

/// @file best_bid_ask.hpp
/// Zero-copy accessors for the Binance spot SBE `BestBidAskStreamEvent`
/// (template id 10001, schema spot_stream 1:0) — the real-time best bid/ask
/// (the SBE equivalent of the JSON `<symbol>@bookTicker` stream, pushed on every
/// book change, with `eventTime` and `bookUpdateId`).
///
/// Authoritative layout (stream_1_0.xml, `<sbe:message id="10001">`):
///   fixed block (blockLength = 50):
///     +0  eventTime     utcTimestampUs (int64, µs)
///     +8  bookUpdateId  updateId       (int64)   — per-symbol monotonic; cross-path dedup
///     +16 priceExponent exponent8 (int8)
///     +17 qtyExponent   exponent8 (int8)
///     +18 bidPrice      mantissa64 (× 10^priceExponent)
///     +26 bidQty        mantissa64 (× 10^qtyExponent)
///     +34 askPrice      mantissa64 (× 10^priceExponent)
///     +42 askQty        mantissa64 (× 10^qtyExponent)
///   data id=200 symbol  varString8
///
/// All four price/qty fields are mandatory (no null sentinel), so the accessors
/// return plain `double`. A single event carries exactly one symbol.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include "eph/sbe/binance/stream/schema.hpp"
#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/errors.hpp"
#include "eph/sbe/parser.hpp"

namespace eph::sbe::binance::stream {

namespace best_bid_ask {

inline constexpr std::size_t kBlockLength      = 50;
inline constexpr std::size_t kOffEventTime     = 0;
inline constexpr std::size_t kOffBookUpdateId  = 8;
inline constexpr std::size_t kOffPriceExponent = 16;
inline constexpr std::size_t kOffQtyExponent   = 17;
inline constexpr std::size_t kOffBidPrice      = 18;
inline constexpr std::size_t kOffBidQty        = 26;
inline constexpr std::size_t kOffAskPrice      = 34;
inline constexpr std::size_t kOffAskQty        = 42;

[[nodiscard]] inline int64_t event_time_us(const MessageView& v) noexcept {
    return read_le_i64(v.body() + kOffEventTime);
}
/// @brief Per-symbol monotonic book update id — the cross-path dedup key.
[[nodiscard]] inline int64_t book_update_id(const MessageView& v) noexcept {
    return read_le_i64(v.body() + kOffBookUpdateId);
}
[[nodiscard]] inline int8_t price_exponent(const MessageView& v) noexcept {
    return read_le_i8(v.body() + kOffPriceExponent);
}
[[nodiscard]] inline int8_t qty_exponent(const MessageView& v) noexcept {
    return read_le_i8(v.body() + kOffQtyExponent);
}
/// @brief Raw best-bid price mantissa (use with price_exponent for exact arith).
[[nodiscard]] inline int64_t bid_price_mantissa(const MessageView& v) noexcept {
    return read_le_i64(v.body() + kOffBidPrice);
}
[[nodiscard]] inline double bid_price(const MessageView& v) noexcept {
    return decode_decimal(read_le_i64(v.body() + kOffBidPrice), price_exponent(v));
}
[[nodiscard]] inline double bid_qty(const MessageView& v) noexcept {
    return decode_decimal(read_le_i64(v.body() + kOffBidQty), qty_exponent(v));
}
[[nodiscard]] inline int64_t ask_price_mantissa(const MessageView& v) noexcept {
    return read_le_i64(v.body() + kOffAskPrice);
}
[[nodiscard]] inline double ask_price(const MessageView& v) noexcept {
    return decode_decimal(read_le_i64(v.body() + kOffAskPrice), price_exponent(v));
}
[[nodiscard]] inline double ask_qty(const MessageView& v) noexcept {
    return decode_decimal(read_le_i64(v.body() + kOffAskQty), qty_exponent(v));
}
/// @brief Trading symbol (zero-copy varString8 at the on-wire block_length).
[[nodiscard]] inline std::string_view symbol(const MessageView& v) noexcept {
    const std::size_t off = v.block_length;
    if (off > v.body_len()) [[unlikely]] return {};
    auto s = read_var_string8(v.body() + off, v.body_len() - off);
    return s ? s->value : std::string_view{};
}

} // namespace best_bid_ask

} // namespace eph::sbe::binance::stream
