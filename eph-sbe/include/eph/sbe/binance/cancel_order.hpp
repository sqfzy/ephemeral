#pragma once

/// @file cancel_order.hpp
/// Zero-copy accessors for the Binance spot SBE `CancelOrderResponse`
/// (template id 305, schema 3:2) — the ACK for `order.cancel`, nested inside a
/// `WebSocketResponse(50)`.
///
/// Authoritative layout (spot_3_2.xml, `<sbe:message id="305">`):
///   fixed block (blockLength = 137): 29 fields. Only the few consumed here are
///   named; offsets are computed from the schema field order/types:
///     field id=10 status  orderStatus (uint8) @ +58
///   data id=200 symbol            varString8
///   data id=201 origClientOrderId varString8
///   data id=202 clientOrderId     varString8   (our cid — the 3rd trailing string)
///
/// Only `status` + `clientOrderId` are exposed (what the gateway maps back to the
/// strategy); the rest of the rich block is intentionally not surfaced.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "eph/sbe/binance/schema.hpp"
#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/parser.hpp"

namespace eph::sbe::binance {

namespace cancel_order {

inline constexpr std::size_t kOffStatus = 58;  ///< field id=10 orderStatus (uint8)

/// @brief Raw orderStatus byte (see binance::OrderStatus enum values; 3 = Canceled).
[[nodiscard]] inline uint8_t status(const MessageView& view) noexcept {
    return view.body()[kOffStatus];
}

/// @brief Trading symbol (1st trailing varString8).
[[nodiscard]] inline std::string_view symbol(const MessageView& view) noexcept {
    const std::size_t off = view.block_length;
    if (off > view.body_len()) [[unlikely]] return {};
    auto s = read_var_string8(view.body() + off, view.body_len() - off);
    return s ? s->value : std::string_view{};
}

/// @brief Client order id / cid — the 3rd trailing varString8
///        (symbol, origClientOrderId, clientOrderId).
[[nodiscard]] inline std::string_view client_order_id(const MessageView& view) noexcept {
    std::size_t off = view.block_length;
    const std::size_t avail = view.body_len();
    for (int i = 0; i < 2; ++i) {                 // skip symbol + origClientOrderId
        if (off > avail) [[unlikely]] return {};
        auto s = read_var_string8(view.body() + off, avail - off);
        if (!s) [[unlikely]] return {};
        off += s->advance;
    }
    if (off > avail) [[unlikely]] return {};
    auto cid = read_var_string8(view.body() + off, avail - off);
    return cid ? cid->value : std::string_view{};
}

} // namespace cancel_order

} // namespace eph::sbe::binance
