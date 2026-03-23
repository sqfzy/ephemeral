#pragma once

/// @file ws_framer.hpp
/// WebSocket MessageFramer adapter — wraps ws::encode_frame / ws::decode_frame
/// into the MessageFramer concept interface for use with Transport.

#include <eph/net/framer_concept.hpp>
#include <eph/net/websocket.hpp>

namespace eph::net {

/// MessageFramer implementation for WebSocket wire format.
///
/// Delegates to ws::encode_frame (which handles masking internally via
/// the thread-local MaskKeyCache) and ws::decode_frame, mapping the
/// ws-specific types to the generic DecodedFrame / FrameError types.
class WsFramer {
public:
    /// Encode a payload into a WebSocket frame.
    ///
    /// @param out       Output buffer (must have at least len + max_overhead() bytes)
    /// @param data      Input payload data
    /// @param len       Input payload length
    /// @param msg_type  WebSocket opcode (e.g. ws::opcode::kBinary)
    /// @return Total bytes written (header + masked payload)
    size_t encode(uint8_t* out, const uint8_t* data,
                  size_t len, uint8_t msg_type) noexcept {
        return ws::encode_frame(out, msg_type, data, len);
    }

    /// Decode a WebSocket frame from the receive buffer.
    ///
    /// Maps ws::DecodedFrame fields to eph::net::DecodedFrame and
    /// ws::DecodeError codes to eph::net::FrameError codes.
    ///
    /// @param data  Input buffer (may contain partial or multiple frames)
    /// @param len   Available bytes in input buffer
    /// @return Decoded frame on success, FrameError on failure
    static std::expected<DecodedFrame, FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        auto result = ws::decode_frame(data, len);
        if (!result) {
            // Map ws::DecodeError → eph::net::FrameError
            switch (result.error()) {
            case ws::DecodeError::kIncomplete:
                return std::unexpected(FrameError::kIncomplete);
            case ws::DecodeError::kReservedBits:
            case ws::DecodeError::kFragmentedControl:
            case ws::DecodeError::kControlPayloadTooLarge:
            case ws::DecodeError::kInvalidOpcode:
                return std::unexpected(FrameError::kInvalidFormat);
            }
            // Unreachable, but satisfy the compiler
            return std::unexpected(FrameError::kInvalidFormat);
        }

        const auto& frame = *result;
        return DecodedFrame{
            .payload     = frame.payload,
            .payload_len = static_cast<size_t>(frame.payload_len),
            .msg_type    = frame.opcode,
            .is_control  = frame.is_control(),
            .total_len   = frame.total_len,
        };
    }

    /// Maximum framing overhead in bytes (14 = 2 base + 8 extended length + 4 mask).
    static constexpr size_t max_overhead() noexcept {
        return ws::kMaxFrameHeaderLen;
    }
};

static_assert(MessageFramer<WsFramer>);

} // namespace eph::net
