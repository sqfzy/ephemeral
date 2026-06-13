#pragma once

/// @file error_response.hpp
/// Zero-copy accessors for the Binance spot SBE `ErrorResponse` (template id 100,
/// schema 3:2) — the error payload nested inside a `WebSocketResponse(50)` when a
/// WS API method fails (rejected order, bad params, auth failure, …).
///
/// Authoritative layout (spot_3_2.xml, `<sbe:message id="100">`):
///   fixed block (blockLength = 18):
///     field id=1 code       int16          @ +0   (Binance error code, e.g. -2010)
///     field id=2 serverTime utcTimestampUs @ +2   optional
///     field id=3 retryAfter utcTimestampUs @ +10  optional
///   data  id=200 msg  varString (uint16-prefixed UTF-8)   (human-readable reason)
///   data  id=201 data optionalMessageData                 (may carry a sub-message; ignored here)

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include "eph/sbe/binance/schema.hpp"
#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/errors.hpp"
#include "eph/sbe/parser.hpp"

namespace eph::sbe::binance {

namespace error_response {

inline constexpr std::size_t kOffCode = 0;  ///< field id=1 (int16)

/// @brief Binance error code (negative, e.g. -2010 "insufficient balance").
[[nodiscard]] inline int16_t code(const MessageView& view) noexcept {
    return read_le_i16(view.body() + kOffCode);
}

/// @brief Human-readable error message (zero-copy varString, uint16-prefixed).
///
/// Located at the on-wire block_length (robust to block growth). Returns "" on
/// truncation rather than reading past the buffer.
[[nodiscard]] inline std::string_view msg(const MessageView& view) noexcept {
    const std::size_t off = view.block_length;
    if (off > view.body_len()) [[unlikely]] return {};
    auto s = read_var_string16(view.body() + off, view.body_len() - off);
    return s ? s->value : std::string_view{};
}

} // namespace error_response

} // namespace eph::sbe::binance
