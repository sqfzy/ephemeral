#pragma once

/// @file moldudp64.hpp
/// MoldUDP64 packet parser — Nasdaq's multicast transport protocol for ITCH
/// data distribution.
///
/// Wire format (UDP payload):
///   [10-byte session ID] [uint64_t sequence_number (BE)] [uint16_t message_count (BE)]
///   followed by message_count messages, each:
///   [uint16_t message_length (BE)] [message_data...]
///
/// MoldUDP64 is a batch container (not a stream framer), so this module
/// provides header parsing and message iteration rather than implementing the
/// MessageFramer concept.
///
/// Reference:
///   https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/moldudp64.pdf

#include <cstdint>
#include <expected>
#include <string_view>
#include <type_traits>

#include <spdlog/spdlog.h>

#include "eph/itch/messages.hpp"  // read_be16, read_be64
#include "eph/itch/parser.hpp"    // ParseError

namespace eph::itch {

// ---------------------------------------------------------------------------
// MoldUDP64 constants
// ---------------------------------------------------------------------------

namespace moldudp64 {

inline constexpr size_t   kSessionLen    = 10;
inline constexpr size_t   kHeaderLen     = 20;  // 10 session + 8 seq + 2 count
inline constexpr uint16_t kEndOfSession  = 0xFFFF;

}  // namespace moldudp64

// ---------------------------------------------------------------------------
// Logger
// ---------------------------------------------------------------------------

namespace detail {
inline const std::shared_ptr<spdlog::logger>& moldudp64_logger() {
    static auto l = [] {
        auto lg = spdlog::get("itch.moldudp64");
        if (!lg) lg = spdlog::stdout_color_mt("itch.moldudp64");
        return lg;
    }();
    return l;
}
}  // namespace detail

// ---------------------------------------------------------------------------
// MoldUDP64Header
// ---------------------------------------------------------------------------

/// Parsed MoldUDP64 packet header (20 bytes on the wire).
struct MoldUDP64Header {
    std::string_view session;        ///< 10-byte session identifier
    uint64_t         sequence_number; ///< First sequence number in this packet
    uint16_t         message_count;   ///< Number of messages (0xFFFF = end of session)
};

// ---------------------------------------------------------------------------
// parse_moldudp64_header
// ---------------------------------------------------------------------------

/// Parse a MoldUDP64 packet header from raw bytes.
///
/// @param data  Pointer to the start of the UDP payload
/// @param len   Number of available bytes
/// @return Parsed header on success, ParseError on failure
[[nodiscard]] inline std::expected<MoldUDP64Header, ParseError>
parse_moldudp64_header(const uint8_t* data, size_t len) noexcept {
    if (len < moldudp64::kHeaderLen) {
        SPDLOG_LOGGER_DEBUG(detail::moldudp64_logger(),
            "MoldUDP64: truncated header, have {} bytes, need {}",
            len, moldudp64::kHeaderLen);
        return std::unexpected(ParseError::kTruncated);
    }

    // Session: first 10 bytes, interpreted as ASCII text
    auto session = std::string_view{
        reinterpret_cast<const char*>(data), moldudp64::kSessionLen};

    // Sequence number: bytes 10..17, big-endian uint64_t
    uint64_t seq = read_be64(data + moldudp64::kSessionLen);

    // Message count: bytes 18..19, big-endian uint16_t
    uint16_t count = read_be16(data + moldudp64::kSessionLen + 8);

    SPDLOG_LOGGER_TRACE(detail::moldudp64_logger(),
        "MoldUDP64: header parsed session='{}' seq={} count={}",
        session, seq, count);

    return MoldUDP64Header{
        .session         = session,
        .sequence_number = seq,
        .message_count   = count,
    };
}

// ---------------------------------------------------------------------------
// parse_moldudp64
// ---------------------------------------------------------------------------

/// Iterate over messages in a MoldUDP64 packet.
///
/// Parses the header, then iterates over message_count length-prefixed
/// messages. For each message, the callback is invoked with the message data
/// pointer, message length, and computed sequence number (header.seq + i).
///
/// If message_count is kEndOfSession (0xFFFF), the callback is never invoked
/// and the function returns 0 — the caller should treat this as an end-of-
/// session signal.
///
/// @param data      Pointer to the start of the UDP payload
/// @param len       Number of available bytes
/// @param callback  Called as callback(const uint8_t* msg_data, uint16_t msg_len, uint64_t seq_num)
/// @return Number of messages successfully delivered to the callback
template <typename Fn>
    requires std::invocable<Fn, const uint8_t*, uint16_t, uint64_t>
size_t parse_moldudp64(const uint8_t* data, size_t len, Fn&& callback) noexcept {
    auto hdr_result = parse_moldudp64_header(data, len);
    if (!hdr_result) {
        SPDLOG_LOGGER_DEBUG(detail::moldudp64_logger(),
            "MoldUDP64: failed to parse header");
        return 0;
    }

    const auto& hdr = *hdr_result;

    // End-of-session marker: no messages to deliver
    if (hdr.message_count == moldudp64::kEndOfSession) {
        SPDLOG_LOGGER_INFO(detail::moldudp64_logger(),
            "MoldUDP64: end-of-session signal for session='{}'", hdr.session);
        return 0;
    }

    // Guard against sequence number wrap-around: hdr.sequence_number + i must
    // not overflow uint64_t.  With message_count <= 65534, the worst case
    // addition is UINT64_MAX - 65533, so this check is almost never triggered
    // in practice but prevents silent wraparound on malformed packets.
    if (hdr.sequence_number > UINT64_MAX - hdr.message_count) {
        SPDLOG_LOGGER_WARN(detail::moldudp64_logger(),
            "MoldUDP64: sequence number near overflow ({} + {}), dropping packet",
            hdr.sequence_number, hdr.message_count);
        return 0;
    }

    // Early reject: each message needs at least a 2-byte length prefix,
    // so the buffer must hold the header plus 2 * message_count bytes minimum.
    if (len < moldudp64::kHeaderLen + hdr.message_count * size_t{2}) {
        SPDLOG_LOGGER_WARN(detail::moldudp64_logger(),
            "MoldUDP64: buffer too small for {} messages (need >= {} bytes, have {})",
            hdr.message_count, moldudp64::kHeaderLen + hdr.message_count * size_t{2}, len);
        return 0;
    }

    size_t offset = moldudp64::kHeaderLen;
    size_t delivered = 0;

    for (uint16_t i = 0; i < hdr.message_count; ++i) {
        // Need at least 2 bytes for the message length prefix
        if (offset + 2 > len) {
            SPDLOG_LOGGER_WARN(detail::moldudp64_logger(),
                "MoldUDP64: truncated message length prefix at message {}/{}, "
                "offset={} available={}",
                i, hdr.message_count, offset, len);
            break;
        }

        uint16_t msg_len = read_be16(data + offset);
        offset += 2;

        // Verify the full message body is available
        if (offset + msg_len > len) {
            SPDLOG_LOGGER_WARN(detail::moldudp64_logger(),
                "MoldUDP64: truncated message body at message {}/{}, "
                "need {} bytes but only {} available",
                i, hdr.message_count, msg_len, len - offset);
            break;
        }

        uint64_t seq_num = hdr.sequence_number + i;

        SPDLOG_LOGGER_TRACE(detail::moldudp64_logger(),
            "MoldUDP64: delivering message {}/{} seq={} len={}",
            i + 1, hdr.message_count, seq_num, msg_len);

        callback(data + offset, msg_len, seq_num);
        ++delivered;
        offset += msg_len;
    }

    SPDLOG_LOGGER_DEBUG(detail::moldudp64_logger(),
        "MoldUDP64: delivered {}/{} messages from session='{}' starting seq={}",
        delivered, hdr.message_count, hdr.session, hdr.sequence_number);

    return delivered;
}

}  // namespace eph::itch
