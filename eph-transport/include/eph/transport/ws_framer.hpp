#pragma once

/// @file ws_framer.hpp
/// WebSocket MessageFramer adapter — wraps ws::encode_frame / ws::decode_frame
/// into the MessageFramer concept interface for use with Transport.

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <eph/core/framer_concept.hpp>
#include <eph/transport/websocket.hpp>

namespace eph::net {

namespace detail {
inline spdlog::logger* ws_framer_logger() {
    static auto l = [] {
        try {
            return spdlog::stdout_color_mt("net.ws_framer");
        } catch (const spdlog::spdlog_ex&) {
            return spdlog::get("net.ws_framer");
        }
    }();
    return l.get();
}
} // namespace detail

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
    [[nodiscard]] size_t encode(uint8_t* out, const uint8_t* data,
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
    [[nodiscard]] std::expected<DecodedFrame, FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        auto result = ws::decode_frame(data, len);
        if (!result) {
            // Map ws::DecodeError → eph::net::FrameError
            switch (result.error()) {
            case ws::DecodeError::kIncomplete:
                SPDLOG_LOGGER_DEBUG(detail::ws_framer_logger(), "WsFramer::decode: incomplete frame, "
                             "available={} bytes", len);
                return std::unexpected(FrameError::kIncomplete);
            case ws::DecodeError::kReservedBits:
                SPDLOG_LOGGER_WARN(detail::ws_framer_logger(), "WsFramer::decode: reserved bits set in "
                            "frame header (protocol violation)");
                return std::unexpected(FrameError::kInvalidFormat);
            case ws::DecodeError::kFragmentedControl:
                SPDLOG_LOGGER_WARN(detail::ws_framer_logger(), "WsFramer::decode: fragmented control "
                            "frame (RFC 6455 §5.5 violation)");
                return std::unexpected(FrameError::kInvalidFormat);
            case ws::DecodeError::kControlPayloadTooLarge:
                SPDLOG_LOGGER_WARN(detail::ws_framer_logger(), "WsFramer::decode: control frame payload "
                            "exceeds 125 bytes (RFC 6455 §5.5 violation)");
                return std::unexpected(FrameError::kInvalidFormat);
            case ws::DecodeError::kInvalidOpcode:
                SPDLOG_LOGGER_WARN(detail::ws_framer_logger(), "WsFramer::decode: invalid opcode {:#04x} "
                            "in frame header", len > 0 ? (data[0] & 0x0F) : 0);
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
