#pragma once

/// @file session_logon.hpp
/// Zero-copy accessors for the Binance spot SBE `WebSocketSessionLogonResponse`
/// (template id 51, schema 3:2) — the success payload for `session.logon`,
/// nested inside a `WebSocketResponse(50)`. Its mere presence (with envelope
/// status 200) means the Ed25519 session is authenticated.
///
/// Authoritative layout (spot_3_2.xml, `<sbe:message id="51">`):
///   fixed block (blockLength = 26):
///     field id=1 authorizedSince  utcTimestampUs @ +0
///     field id=2 connectedSince   utcTimestampUs @ +8
///     field id=3 returnRateLimits boolEnum       @ +16
///     field id=4 serverTime       utcTimestampUs @ +17
///     field id=5 userDataStream   boolEnum       @ +25  optional
///   data id=200 loggedOnApiKey varString (uint16-prefixed)

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "eph/sbe/binance/schema.hpp"
#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/parser.hpp"

namespace eph::sbe::binance {

namespace session_logon {

inline constexpr std::size_t kOffAuthorizedSince = 0;   ///< field id=1 (int64, µs)
inline constexpr std::size_t kOffServerTime      = 17;  ///< field id=4 (int64, µs)

/// @brief When the API key became authorized (µs since epoch).
[[nodiscard]] inline int64_t authorized_since_us(const MessageView& view) noexcept {
    return read_le_i64(view.body() + kOffAuthorizedSince);
}

/// @brief Server time at logon (µs since epoch).
[[nodiscard]] inline int64_t server_time_us(const MessageView& view) noexcept {
    return read_le_i64(view.body() + kOffServerTime);
}

/// @brief Logged-on API key echo (zero-copy varString at on-wire block_length).
[[nodiscard]] inline std::string_view logged_on_api_key(const MessageView& view) noexcept {
    const std::size_t off = view.block_length;
    if (off > view.body_len()) [[unlikely]] return {};
    auto s = read_var_string16(view.body() + off, view.body_len() - off);
    return s ? s->value : std::string_view{};
}

} // namespace session_logon

} // namespace eph::sbe::binance
