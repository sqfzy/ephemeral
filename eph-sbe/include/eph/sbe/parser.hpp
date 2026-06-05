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

// ---------------------------------------------------------------------------
// Tag types + dispatch() — type-safe visitor over template ids
// ---------------------------------------------------------------------------

/// @brief Empty tag types for compile-time template-id discrimination.
///
/// New schema messages append a tag here and a case in dispatch(); accessors
/// for that message live in the schema's own header. `Unknown` is dispatched
/// for any template id without a registered tag.
namespace msg {
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
    default: return handler(msg::Unknown{}, view);
    }
}

} // namespace eph::sbe
