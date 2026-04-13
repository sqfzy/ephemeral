#pragma once

/// @file length_prefix_framer.hpp
/// 2-byte big-endian length-prefix framer for binary protocols (ITCH, etc.).
///
/// Wire format: [uint16_t length (big-endian)] [payload bytes]
/// The length field contains the payload length (not including itself).
/// If the payload is non-empty, the first byte is exposed as msg_type
/// in DecodedFrame (useful for ITCH where byte 0 is the message type).

#include <cstdint>
#include <cstring>

#include <spdlog/spdlog.h>
#include "eph/core/detail/logger.hpp"

#include "eph/core/framer_concept.hpp"

namespace eph::net {

namespace detail {
/// @brief Lazily-initialized logger for the LengthPrefixFramer.
///
/// Creates a colored stdout logger named "core.framer" on first call.
/// If the logger already exists (e.g., registered by a previous shared library
/// load), returns the existing instance.
///
/// @return Pointer to the spdlog logger instance. Never null after initialization.
inline spdlog::logger* framer_logger() {
    static auto* l = ::eph::core::detail::make_logger("core.framer");
    return l;
}
} // namespace detail

/// @brief Length-prefix framer: 2-byte big-endian length header.
///
/// Encodes: [len_hi][len_lo][payload...]
/// Decodes: reads 2-byte length, waits for full payload, returns DecodedFrame.
/// msg_type is set to the first payload byte (protocol message type).
///
/// Satisfies the MessageFramer concept. Stateless -- the compiler eliminates
/// the unused `this` pointer at -O2.
///
/// @warning Zero-length payloads are rejected by both encode() and decode()
///          because msg_type is derived from payload[0]. Protocols needing
///          heartbeats should use a 1-byte sentinel message instead.
class LengthPrefixFramer {
public:
    /// @brief Maximum framing overhead in bytes (2-byte length prefix).
    /// @return Always returns 2.
    static constexpr size_t max_overhead() noexcept { return 2; }

    /// @brief Maximum payload length representable in a 2-byte big-endian header (65535).
    static constexpr size_t kMaxPayloadLen = 65535;

    /// @brief Encode a payload into length-prefixed wire format.
    ///
    /// Writes a 2-byte big-endian length prefix followed by the payload bytes.
    ///
    /// @param out       Output buffer. Must have capacity for at least len + 2 bytes.
    /// @param data      Pointer to the payload data to encode.
    /// @param len       Payload length in bytes. Must be in [1, kMaxPayloadLen].
    /// @param msg_type  Unused by this framer (ignored).
    /// @return Total bytes written (2 + len), or 0 on invalid arguments.
    ///
    /// @warning Returns 0 (not an error type) on failure. Callers must check
    ///          the return value before advancing write pointers.
    [[nodiscard]] size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t /*msg_type*/) noexcept {
        // Guard: payload must fit in a uint16_t length field.
        // Silently truncating would corrupt the wire format, so return 0
        // to signal failure (caller already checks return value > 0).
        // Zero-length payloads are rejected in both encode and decode because
        // the framer uses the first payload byte as msg_type — a zero-length
        // frame would require reading past the payload. Protocols needing
        // heartbeats should use a 1-byte sentinel message instead.
        if (len > kMaxPayloadLen || len == 0 || !data || !out) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::framer_logger(),"LengthPrefixFramer::encode: invalid args len={} "
                        "data={} out={} (max={}, min=1) — caller bug",
                        len, fmt::ptr(data), fmt::ptr(out), kMaxPayloadLen);
            return 0;
        }

        // 2-byte big-endian length prefix
        out[0] = static_cast<uint8_t>((len >> 8) & 0xFF);
        out[1] = static_cast<uint8_t>(len & 0xFF);
        std::memcpy(out + 2, data, len);
        return 2 + len;
    }

    /// @brief Decode a length-prefixed frame from the receive buffer.
    ///
    /// Reads the 2-byte big-endian length prefix, waits for the full payload,
    /// and returns a DecodedFrame with msg_type set to the first payload byte.
    ///
    /// @param data  Input buffer (may contain partial or multiple frames).
    /// @param len   Available bytes in the input buffer.
    /// @return DecodedFrame on success, FrameError::kIncomplete if more data
    ///         is needed, or FrameError::kInvalidFormat for zero-length payloads.
    [[nodiscard]] std::expected<DecodedFrame, FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        if (len < 2) {
            SPDLOG_LOGGER_DEBUG(detail::framer_logger(),"LengthPrefixFramer::decode: incomplete header, "
                         "need 2 bytes but have {}", len);
            return std::unexpected(FrameError::kIncomplete);
        }

        uint16_t msg_len = static_cast<uint16_t>(
            (static_cast<uint16_t>(data[0]) << 8) | data[1]);

        // Zero-length payloads are rejected: msg_type is derived from payload[0],
        // so a zero-length frame has no valid msg_type. See encode() comment.
        if (msg_len == 0) {
            SPDLOG_LOGGER_WARN(detail::framer_logger(),"LengthPrefixFramer::decode: zero-length payload "
                        "(invalid format)");
            return std::unexpected(FrameError::kInvalidFormat);
        }
        if (len < 2u + msg_len) {
            SPDLOG_LOGGER_DEBUG(detail::framer_logger(),"LengthPrefixFramer::decode: incomplete payload, "
                         "need {} bytes but have {}", 2u + msg_len, len);
            return std::unexpected(FrameError::kIncomplete);
        }

        return DecodedFrame{
            .payload     = data + 2,
            .payload_len = msg_len,
            .msg_type    = data[2],  // First payload byte = protocol message type
            .is_control  = false,
            .total_len   = 2u + msg_len,
        };
    }
};

static_assert(MessageFramer<LengthPrefixFramer>,
    "LengthPrefixFramer must satisfy MessageFramer concept");

} // namespace eph::net
