#pragma once

/// @file parser.hpp
/// Zero-copy Simple Binary Encoding (SBE) message parser — schema-independent core.
///
/// Decodes the SBE message header into a lightweight MessageView that points
/// back into the caller's receive buffer (no allocation, no copy), and provides
/// the repeating-group dimension primitive (`groupSize16Encoding`) plus a
/// template-id dispatch scaffold. Schema-specific field accessors live under
/// `eph/sbe/<schema>/` (e.g. binance/book_ticker.hpp) and operate on the views
/// produced here. Mirrors eph-itch's parser.hpp structure.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/sbe/byte_order.hpp"
#include "eph/sbe/errors.hpp"
#include "eph/sbe/message_header.hpp"

namespace eph::sbe {

/// @brief Internal detail namespace for the SBE parser module logger.
namespace detail {
/// @brief Get or create the SBE parser module logger ("sbe.parser").
/// @return Shared pointer reference to the spdlog logger instance.
inline const std::shared_ptr<spdlog::logger>& sbe_parser_logger() {
    static auto l = [] {
        auto lg = spdlog::get("sbe.parser");
        if (!lg) {
            try { lg = spdlog::stdout_color_mt("sbe.parser"); }
            catch (const spdlog::spdlog_ex&) { lg = spdlog::get("sbe.parser"); }
        }
        if (!lg) lg = spdlog::default_logger();
        return lg;
    }();
    return l;
}
} // namespace detail

// ---------------------------------------------------------------------------
// MessageView — zero-copy parsed-message handle
// ---------------------------------------------------------------------------

/// @brief Zero-copy view of an SBE message.
///
/// Holds the decoded header fields plus a pointer/length back into the caller's
/// buffer. `data` points at the messageHeader (byte 0); the root block (if any)
/// begins at `data + kHeaderSize`. The view owns nothing and is valid only as
/// long as the underlying buffer.
struct MessageView {
    uint16_t       template_id;  ///< Message type id (from the header).
    uint16_t       schema_id;    ///< Schema id (from the header).
    uint16_t       version;      ///< Schema version (from the header).
    uint16_t       block_length; ///< Root-block size in bytes (from the header).
    const uint8_t* data;         ///< Pointer to the messageHeader (byte 0).
    std::size_t    length;       ///< Total readable bytes at `data`.

    /// @brief Pointer to the first byte after the 8-byte message header.
    [[nodiscard]] const uint8_t* body() const noexcept { return data + kHeaderSize; }

    /// @brief Readable bytes available after the message header.
    [[nodiscard]] std::size_t body_len() const noexcept {
        return length >= kHeaderSize ? length - kHeaderSize : 0;
    }
};

/// @brief Parse the SBE message header into a zero-copy MessageView.
///
/// Schema-independent: validates only that the 8-byte header is present.
/// Per-message body validation is the schema accessor's responsibility.
///
/// @param data Pointer to the start of an SBE message (the messageHeader).
/// @param len  Number of readable bytes at @p data.
/// @return A MessageView over [data, data+len), or ParseError::kIncomplete.
[[nodiscard]] inline std::expected<MessageView, ParseError>
parse(const uint8_t* data, std::size_t len) noexcept {
    auto hdr = parse_header(data, len);
    if (!hdr) [[unlikely]] {
        SPDLOG_LOGGER_TRACE(detail::sbe_parser_logger(),
            "sbe::parse: incomplete header, len={} (need {})", len, kHeaderSize);
        return std::unexpected(hdr.error());
    }
    return MessageView{
        .template_id  = hdr->template_id,
        .schema_id    = hdr->schema_id,
        .version      = hdr->version,
        .block_length = hdr->block_length,
        .data         = data,
        .length       = len,
    };
}

// ---------------------------------------------------------------------------
// Repeating-group dimension primitive (groupSize16Encoding)
// ---------------------------------------------------------------------------

/// @brief Decoded SBE repeating-group dimension header (`groupSize16Encoding`).
struct GroupHeader {
    uint16_t block_length;  ///< Size in bytes of each group entry's fixed block.
    uint16_t num_in_group;  ///< Number of entries in the group.
};

/// @brief Byte size of the `groupSize16Encoding` composite (two uint16 fields).
inline constexpr std::size_t kGroupHeaderSize = 4;

/// @brief Decode a `groupSize16Encoding` repeating-group header from raw bytes.
/// @param p Pointer to at least `kGroupHeaderSize` readable bytes.
/// @return The decoded GroupHeader (blockLength, numInGroup).
[[nodiscard]] inline GroupHeader read_group_header(const uint8_t* p) noexcept {
    return GroupHeader{
        .block_length = read_le16(p),
        .num_in_group = read_le16(p + 2),
    };
}

/// @brief Total wire size of a `groupSize16Encoding` repeating group.
///
/// = the 4-byte dimension header + numInGroup × per-entry blockLength. Useful to
/// *skip* a group whose entries the caller does not consume (e.g. the WS API
/// envelope's `rateLimits` group, which precedes the fields the caller wants).
/// @param p Pointer to the group dimension header.
/// @return Bytes the whole group occupies (header + all entries).
[[nodiscard]] inline std::size_t group_total_size(const uint8_t* p) noexcept {
    const GroupHeader gh = read_group_header(p);
    return kGroupHeaderSize +
           static_cast<std::size_t>(gh.num_in_group) * gh.block_length;
}

// ---------------------------------------------------------------------------
// Nested message (messageData / optionalMessageData composite)
// ---------------------------------------------------------------------------

/// @brief A decoded nested SBE message plus the bytes its composite occupied.
struct NestedMessage {
    MessageView view;    ///< Zero-copy view over the embedded message.
    std::size_t advance; ///< Total bytes consumed (4-byte length + embedded message).
};

/// @brief Decode an SBE `messageData` composite — a uint32 length prefix followed
///        by a fully self-describing embedded SBE message (its own 8-byte header).
///
/// Binance's WS API wraps every method result in a `WebSocketResponse(50)`
/// envelope whose `result` field is exactly this composite; the embedded
/// message's own `templateId` (e.g. NewOrderAckResponse 300, ErrorResponse 100)
/// tells the caller what it is. Bounds-checked against `remaining` before any
/// deref. For `optionalMessageData`, a declared length of 0 means null — callers
/// should treat `length == 0` as "absent" rather than calling this.
///
/// @param p         Pointer to the 4-byte length prefix.
/// @param remaining Readable bytes available at @p p.
/// @return {embedded MessageView, bytes consumed}, or ParseError::kTruncated if
///         the prefix/payload overruns (or kIncomplete if the inner header is
///         shorter than 8 bytes).
[[nodiscard]] inline std::expected<NestedMessage, ParseError>
read_message_data(const uint8_t* p, std::size_t remaining) noexcept {
    if (remaining < 4) [[unlikely]]
        return std::unexpected(ParseError::kTruncated);
    const std::size_t len = read_le32(p);
    if (remaining < 4 + len) [[unlikely]]
        return std::unexpected(ParseError::kTruncated);
    auto sub = parse(p + 4, len);   // embedded message carries its own header
    if (!sub) [[unlikely]]
        return std::unexpected(sub.error());
    return NestedMessage{ .view = *sub, .advance = 4 + len };
}

// ---------------------------------------------------------------------------
// Tag types + dispatch() — type-safe visitor over template ids
// ---------------------------------------------------------------------------

/// @brief Empty tag types for compile-time template-id discrimination.
///
/// New schema messages append a tag here and a case in dispatch(); accessors
/// for that message live in the schema's own header. `Unknown` is dispatched
/// for any template id without a registered tag.
namespace msg {
struct BookTicker {};        ///< Binance spot BookTickerResponse (template id 212).
struct WebSocketResponse {}; ///< WS API result envelope (template id 50).
struct SessionLogon {};      ///< WebSocketSessionLogonResponse (template id 51).
struct ErrorResponse {};     ///< WS API ErrorResponse (template id 100).
struct NewOrderAck {};       ///< NewOrderAckResponse (template id 300).
struct CancelOrder {};       ///< CancelOrderResponse (template id 305).
struct ExecutionReport {};   ///< ExecutionReportEvent — user-data fill/status (template id 603).
struct BestBidAsk {};        ///< spot_stream BestBidAskStreamEvent (template id 10001).
struct Unknown {};
} // namespace msg

/// @brief Dispatch a parsed SBE message to a handler by template id.
///
/// The handler is invoked as `handler(Tag{}, view)` where Tag is one of the
/// msg:: structs. The handler's return value is forwarded back to the caller.
/// With no schema messages registered yet, every message dispatches to
/// msg::Unknown — schema layers extend the switch.
///
/// @tparam Handler Callable with overloads `(msg::TagType, const MessageView&)`.
/// @param view    Parsed message view from parse().
/// @param handler Handler object, lambda, or overload set.
/// @return The handler's return value, forwarded as-is.
template <typename Handler>
decltype(auto) dispatch(const MessageView& view, Handler&& handler) {
    switch (view.template_id) {
    // Template ids kept as literals to avoid a cyclic include of the schema
    // layers (binance/schema.hpp, binance/stream/schema.hpp), which depend on
    // this header. They mirror those headers' tid:: constants.
    case 50:    return handler(msg::WebSocketResponse{}, view);
    case 51:    return handler(msg::SessionLogon{}, view);
    case 100:   return handler(msg::ErrorResponse{}, view);
    case 212:   return handler(msg::BookTicker{}, view);
    case 300:   return handler(msg::NewOrderAck{}, view);
    case 305:   return handler(msg::CancelOrder{}, view);
    case 603:   return handler(msg::ExecutionReport{}, view);
    case 10001: return handler(msg::BestBidAsk{}, view);
    default:    return handler(msg::Unknown{}, view);
    }
}

} // namespace eph::sbe
