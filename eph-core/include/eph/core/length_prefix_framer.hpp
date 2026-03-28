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

#include "eph/core/framer_concept.hpp"

namespace eph::net {

/// Length-prefix framer: 2-byte big-endian length header.
///
/// Encodes: [len_hi][len_lo][payload...]
/// Decodes: reads 2-byte length, waits for full payload, returns DecodedFrame.
/// msg_type is set to the first payload byte (protocol message type).
class LengthPrefixFramer {
public:
    static constexpr size_t max_overhead() noexcept { return 2; }

    /// Maximum payload length representable in a 2-byte big-endian header.
    static constexpr size_t kMaxPayloadLen = 65535;

    size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t /*msg_type*/) noexcept {
        // Guard: payload must fit in a uint16_t length field.
        // Silently truncating would corrupt the wire format, so return 0
        // to signal failure (caller already checks return value > 0).
        if (len > kMaxPayloadLen || len == 0 || !data || !out) [[unlikely]] {
            SPDLOG_DEBUG("LengthPrefixFramer::encode: invalid args len={} "
                         "data={} out={} (max={}, min=1)",
                         len, fmt::ptr(data), fmt::ptr(out), kMaxPayloadLen);
            return 0;
        }

        // 2-byte big-endian length prefix
        out[0] = static_cast<uint8_t>((len >> 8) & 0xFF);
        out[1] = static_cast<uint8_t>(len & 0xFF);
        std::memcpy(out + 2, data, len);
        return 2 + len;
    }

    [[nodiscard]] std::expected<DecodedFrame, FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        if (len < 2) {
            SPDLOG_DEBUG("LengthPrefixFramer::decode: incomplete header, "
                         "need 2 bytes but have {}", len);
            return std::unexpected(FrameError::kIncomplete);
        }

        uint16_t msg_len = static_cast<uint16_t>(
            (static_cast<uint16_t>(data[0]) << 8) | data[1]);

        if (msg_len == 0) {
            SPDLOG_WARN("LengthPrefixFramer::decode: zero-length payload "
                        "(invalid format)");
            return std::unexpected(FrameError::kInvalidFormat);
        }
        if (len < 2u + msg_len) {
            SPDLOG_DEBUG("LengthPrefixFramer::decode: incomplete payload, "
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
