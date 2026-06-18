#pragma once

/// @file web_socket_response.hpp
/// Zero-copy accessors for the Binance spot SBE `WebSocketResponse` envelope
/// (template id 50, schema 3:2) — the wrapper around every WS API *method*
/// result (order.place, order.cancel, session.logon, …; even `ErrorResponse`
/// is wrapped).
///
/// Authoritative layout (spot_3_2.xml, `<sbe:message id="50">`):
///   fixed block (blockLength = 3):
///     field id=1 sbeSchemaIdVersionDeprecated boolEnum (uint8) @ +0
///     field id=2 status                       uint16          @ +1   (HTTP-like; 200 = ok)
///   group id=100 rateLimits  (groupSize16Encoding header, 4 bytes; trails block)
///   data  id=200 id          varString8   (the request's echoed `id`; correlation key)
///   data  id=201 result      messageData  (nested SBE message; its templateId says what)
///
/// The variable section (id + result) sits *after* the rateLimits group, so it is
/// located using the on-wire block_length + the on-wire group size — robust to a
/// venue that lengthens the block or adds rate-limit entries.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include "eph/sbe/binance/schema.hpp"
#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/errors.hpp"
#include "eph/sbe/parser.hpp"

namespace eph::sbe::binance {

namespace web_socket_response {

/// @brief Offset of the `status` field within the fixed block (spot 3:2).
inline constexpr std::size_t kOffStatus = 1;  ///< field id=2 (uint16)

/// @brief HTTP-like response status (200 = success). Read at a fixed offset, so
///        valid without walking the variable section.
[[nodiscard]] inline uint16_t status(const MessageView& view) noexcept {
    return read_le16(view.body() + kOffStatus);
}

/// @brief Fully decoded envelope: status + echoed request id + nested result.
struct Decoded {
    uint16_t    status;  ///< HTTP-like status (200 = ok).
    std::string_view id; ///< Echoed request `id` (correlation key; "" if none).
    MessageView result;  ///< The nested result message (its `template_id` discriminates).
};

/// @brief Walk the envelope to its `id` and nested `result`.
///
/// Bounds-checked end to end: the rateLimits group size, the `id` varString8, and
/// the `result` messageData are each validated against the buffer before any
/// deref. Refuses a non-3:2 schema (`is_supported`) rather than mis-locating
/// fields.
///
/// @param view Parsed top-level message view (template id should be 50).
/// @return {status, id, result MessageView}, or ParseError on truncation /
///         wrong schema.
[[nodiscard]] inline std::expected<Decoded, ParseError>
decode(const MessageView& view) noexcept {
    if (!is_supported(view)) [[unlikely]]
        return std::unexpected(ParseError::kMalformedGroup);
    const uint8_t* const body = view.body();
    const std::size_t avail = view.body_len();
    // fixed block + rateLimits group dimension header must be present.
    if (avail < static_cast<std::size_t>(view.block_length) + kGroupHeaderSize) [[unlikely]]
        return std::unexpected(ParseError::kTruncated);

    std::size_t off = view.block_length;
    const std::size_t grp = group_total_size(body + off);   // 4-byte header + entries
    if (off + grp > avail) [[unlikely]]
        return std::unexpected(ParseError::kTruncated);
    off += grp;

    auto id = read_var_string8(body + off, avail - off);
    if (!id) [[unlikely]] return std::unexpected(id.error());
    const std::string_view id_sv = id->value;
    off += id->advance;

    auto res = read_message_data(body + off, avail - off);
    if (!res) [[unlikely]] return std::unexpected(res.error());

    return Decoded{ .status = read_le16(body + kOffStatus),
                    .id     = id_sv,
                    .result = res->view };
}

/// @brief One decoded `rateLimits` group entry (spot 3:2, group id=100; 19-byte block).
///        Layout: rateLimitType@+0, interval@+1, intervalNum@+2, rateLimit(i64)@+3,
///        current(i64)@+11. Reports binance's per-IP usage as of this response.
struct RateLimit {
    uint8_t type;          ///< rateLimitType: 0=RAW_REQUESTS 1=CONNECTIONS 2=REQUEST_WEIGHT 3=ORDERS
    uint8_t interval;      ///< rateLimitInterval: 0=SECOND 1=MINUTE 2=HOUR 3=DAY
    uint8_t interval_num;  ///< multiplier on `interval` (interval=MINUTE,num=1 → per-minute)
    int64_t limit;         ///< the cap (schema field `rateLimit`)
    int64_t current;       ///< current usage in this window (schema field `current`)
};

inline constexpr std::size_t kRateLimitEntryLen = 19;  ///< 1+1+1+8+8

/// @brief Walk the envelope's rateLimits group (id=100) — it trails the fixed block,
/// BEFORE `id`/`result` (so this reads the same region decode() skips over). Reports
/// per-IP REQUEST_WEIGHT / ORDERS / CONNECTIONS {limit, current} as binance sees it
/// (cross-connection, cross-process truth). Bounds- and schema-checked; strides by the
/// on-wire entry blockLength so appended schema fields don't break iteration.
///
/// @tparam Fn  Callable invoked as `fn(const RateLimit&)` per entry.
/// @param view Parsed envelope view (template id should be 50).
/// @return number of entries delivered, or ParseError on truncation / wrong schema.
template <typename Fn>
[[nodiscard]] inline std::expected<std::size_t, ParseError>
for_each_rate_limit(const MessageView& view, Fn&& fn) noexcept {
    if (!is_supported(view)) [[unlikely]] return std::unexpected(ParseError::kMalformedGroup);
    const uint8_t* const body = view.body();
    const std::size_t avail = view.body_len();
    std::size_t off = view.block_length;
    if (off + kGroupHeaderSize > avail) [[unlikely]] return std::unexpected(ParseError::kTruncated);
    const GroupHeader gh = read_group_header(body + off);
    // A fixed block smaller than our known layout means the reads would overrun — reject.
    if (gh.block_length < kRateLimitEntryLen) [[unlikely]] return std::unexpected(ParseError::kMalformedGroup);
    off += kGroupHeaderSize;
    for (uint16_t i = 0; i < gh.num_in_group; ++i) {
        if (off + gh.block_length > avail) [[unlikely]] return std::unexpected(ParseError::kTruncated);
        const uint8_t* const e = body + off;
        fn(RateLimit{ e[0], e[1], e[2], read_le_i64(e + 3), read_le_i64(e + 11) });
        off += gh.block_length;
    }
    return static_cast<std::size_t>(gh.num_in_group);
}

} // namespace web_socket_response

} // namespace eph::sbe::binance
