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

#include "eph/net/framer_concept.hpp"

namespace eph::net {

/// Length-prefix framer: 2-byte big-endian length header.
///
/// Encodes: [len_hi][len_lo][payload...]
/// Decodes: reads 2-byte length, waits for full payload, returns DecodedFrame.
/// msg_type is set to the first payload byte (protocol message type).
class LengthPrefixFramer {
public:
    static constexpr size_t max_overhead() noexcept { return 2; }

    size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t /*msg_type*/) noexcept {
        // 2-byte big-endian length prefix
        out[0] = static_cast<uint8_t>((len >> 8) & 0xFF);
        out[1] = static_cast<uint8_t>(len & 0xFF);
        std::memcpy(out + 2, data, len);
        return 2 + len;
    }

    static std::expected<DecodedFrame, FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        if (len < 2) return std::unexpected(FrameError::kIncomplete);

        uint16_t msg_len = static_cast<uint16_t>(
            (static_cast<uint16_t>(data[0]) << 8) | data[1]);

        if (msg_len == 0) {
            return std::unexpected(FrameError::kInvalidFormat);
        }
        if (len < 2u + msg_len) {
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
