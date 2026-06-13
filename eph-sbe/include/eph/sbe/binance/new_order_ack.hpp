#pragma once

/// @file new_order_ack.hpp
/// Zero-copy accessors for the Binance spot SBE `NewOrderAckResponse`
/// (template id 300, schema 3:2) — the ACK for `order.place` with
/// `newOrderRespType=ACK` (the low-latency default), nested inside a
/// `WebSocketResponse(50)`.
///
/// Authoritative layout (spot_3_2.xml, `<sbe:message id="300">`):
///   fixed block (blockLength = 24):
///     field id=1 orderId      orderId        (int64)  @ +0   (exchange order id)
///     field id=2 orderListId  orderId        (int64)  @ +8   optional (-1 = none)
///     field id=3 transactTime utcTimestampUs (int64)  @ +16  (µs)
///   data id=200 symbol        varString8
///   data id=201 clientOrderId varString8     (our orderLinkId / cid)

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "eph/sbe/binance/schema.hpp"
#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/parser.hpp"

namespace eph::sbe::binance {

namespace new_order_ack {

inline constexpr std::size_t kOffOrderId      = 0;   ///< field id=1 (int64)
inline constexpr std::size_t kOffTransactTime = 16;  ///< field id=3 (int64, µs)

/// @brief Exchange-assigned order id.
[[nodiscard]] inline int64_t order_id(const MessageView& view) noexcept {
    return read_le_i64(view.body() + kOffOrderId);
}

/// @brief Exchange transaction time (microseconds since epoch).
[[nodiscard]] inline int64_t transact_time_us(const MessageView& view) noexcept {
    return read_le_i64(view.body() + kOffTransactTime);
}

/// @brief Trading symbol (zero-copy varString8 at the on-wire block_length).
[[nodiscard]] inline std::string_view symbol(const MessageView& view) noexcept {
    const std::size_t off = view.block_length;
    if (off > view.body_len()) [[unlikely]] return {};
    auto s = read_var_string8(view.body() + off, view.body_len() - off);
    return s ? s->value : std::string_view{};
}

/// @brief Client order id (our cid / orderLinkId) — the varString8 after symbol.
[[nodiscard]] inline std::string_view client_order_id(const MessageView& view) noexcept {
    std::size_t off = view.block_length;
    if (off > view.body_len()) [[unlikely]] return {};
    auto sym = read_var_string8(view.body() + off, view.body_len() - off);
    if (!sym) [[unlikely]] return {};
    off += sym->advance;
    auto cid = read_var_string8(view.body() + off, view.body_len() - off);
    return cid ? cid->value : std::string_view{};
}

} // namespace new_order_ack

} // namespace eph::sbe::binance
