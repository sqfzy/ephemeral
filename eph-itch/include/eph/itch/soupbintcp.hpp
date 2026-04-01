#pragma once

/// @file soupbintcp.hpp
/// SoupBinTCP framer — Nasdaq's TCP transport protocol for ITCH data feeds.
///
/// Wire format: [uint16_t length (big-endian)] [uint8_t type] [payload...]
/// The length field includes the type byte but NOT itself.
/// Sequenced Data ('S') packets carry ITCH messages as their payload.

#include <cstdint>
#include <cstring>

#include <spdlog/spdlog.h>

#include "eph/core/framer_concept.hpp"

namespace eph::itch {

// -------------------------------------------------------------------------
// SoupBinTCP packet type constants
// -------------------------------------------------------------------------

namespace soupbin {

inline constexpr uint8_t kDebug           = '+';  // 0x2B — server -> client
inline constexpr uint8_t kSequencedData   = 'S';  // 0x53 — server -> client (ITCH messages)
inline constexpr uint8_t kServerHeartbeat = 'H';  // 0x48 — server -> client
inline constexpr uint8_t kLoginAccepted   = 'A';  // 0x41 — server -> client
inline constexpr uint8_t kLoginRejected   = 'J';  // 0x4A — server -> client
inline constexpr uint8_t kLoginRequest    = 'L';  // 0x4C — client -> server
inline constexpr uint8_t kUnsequencedData = 'U';  // 0x55 — client -> server
inline constexpr uint8_t kClientHeartbeat = 'R';  // 0x52 — client -> server
inline constexpr uint8_t kLogoutRequest   = 'O';  // 0x4F — client -> server

/// True if this is a heartbeat packet (no payload expected).
constexpr bool is_heartbeat(uint8_t type) noexcept {
    return type == kServerHeartbeat || type == kClientHeartbeat;
}

}  // namespace soupbin

// -------------------------------------------------------------------------
// SoupBinTcpFramer
// -------------------------------------------------------------------------

/// Framer for the SoupBinTCP protocol.
///
/// Satisfies the MessageFramer concept. Unlike the bare LengthPrefixFramer,
/// SoupBinTCP has an explicit type byte between the length header and the
/// payload, so the overhead is 3 bytes (2 length + 1 type).
class SoupBinTcpFramer {
public:
    /// Maximum framing overhead: 2 (length) + 1 (type).
    static constexpr size_t max_overhead() noexcept { return 3; }

    /// Maximum payload length: uint16_t max minus the type byte.
    static constexpr size_t kMaxPayloadLen = 65534;

    /// Encode a payload into SoupBinTCP wire format.
    ///
    /// @param out       Output buffer (must have at least len + 3 bytes)
    /// @param data      Input payload data (may be nullptr if len == 0)
    /// @param len       Input payload length in bytes
    /// @param msg_type  SoupBinTCP packet type byte (e.g. soupbin::kSequencedData)
    /// @return Total bytes written to out (3 + len), or 0 on failure
    [[nodiscard]] size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t msg_type) noexcept {
        if (!out) [[unlikely]] {
            SPDLOG_DEBUG("SoupBinTcpFramer::encode: null output buffer");
            return 0;
        }
        if (len > kMaxPayloadLen) [[unlikely]] {
            SPDLOG_DEBUG("SoupBinTcpFramer::encode: payload too large len={} max={}",
                         len, kMaxPayloadLen);
            return 0;
        }
        if (len > 0 && !data) [[unlikely]] {
            SPDLOG_DEBUG("SoupBinTcpFramer::encode: null data with non-zero len={}", len);
            return 0;
        }

        // Length field = payload size + 1 (for type byte)
        const auto wire_len = static_cast<uint16_t>(len + 1);
        out[0] = static_cast<uint8_t>((wire_len >> 8) & 0xFF);
        out[1] = static_cast<uint8_t>(wire_len & 0xFF);
        out[2] = msg_type;

        if (len > 0) {
            std::memcpy(out + 3, data, len);
        }

        SPDLOG_TRACE("SoupBinTcpFramer::encode: type=0x{:02X} payload_len={} "
                     "wire_len={}", msg_type, len, 3 + len);
        return 3 + len;
    }

    /// Decode a SoupBinTCP packet from the receive buffer.
    ///
    /// @param data  Input buffer (may contain partial or multiple frames)
    /// @param len   Available bytes in input buffer
    /// @return Decoded frame on success, FrameError on failure
    [[nodiscard]] std::expected<eph::net::DecodedFrame, eph::net::FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        using eph::net::DecodedFrame;
        using eph::net::FrameError;

        // Need at least the 2-byte length header
        if (len < 2) {
            SPDLOG_DEBUG("SoupBinTcpFramer::decode: incomplete header, "
                         "need 2 bytes but have {}", len);
            return std::unexpected(FrameError::kIncomplete);
        }

        // Read big-endian length (includes type byte, excludes itself)
        const uint16_t msg_len = static_cast<uint16_t>(
            (static_cast<uint16_t>(data[0]) << 8) | data[1]);

        // Length must be >= 1 (at minimum the type byte)
        if (msg_len < 1) {
            SPDLOG_WARN("SoupBinTcpFramer::decode: zero-length field "
                        "(must include type byte)");
            return std::unexpected(FrameError::kInvalidFormat);
        }

        // Check if the full packet is available
        const size_t total = 2u + msg_len;
        if (len < total) {
            SPDLOG_DEBUG("SoupBinTcpFramer::decode: incomplete packet, "
                         "need {} bytes but have {}", total, len);
            return std::unexpected(FrameError::kIncomplete);
        }

        // Extract type byte and payload
        const uint8_t type = data[2];
        const size_t payload_len = msg_len - 1;  // subtract type byte

        SPDLOG_TRACE("SoupBinTcpFramer::decode: type=0x{:02X} payload_len={} "
                     "total_len={}", type, payload_len, total);

        return DecodedFrame{
            .payload     = payload_len > 0 ? data + 3 : nullptr,
            .payload_len = payload_len,
            .msg_type    = type,
            .is_control  = soupbin::is_heartbeat(type),
            .total_len   = total,
        };
    }
};

static_assert(eph::net::MessageFramer<SoupBinTcpFramer>,
    "SoupBinTcpFramer must satisfy MessageFramer concept");

}  // namespace eph::itch
