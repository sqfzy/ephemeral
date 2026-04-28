#pragma once

/// @file framer.hpp
/// JSON framer satisfying the eph::net::MessageFramer concept.
///
/// WebSocket already provides message framing, so JSON payloads arrive
/// as complete messages. This framer is a semantic pass-through: it
/// delivers the entire payload as a single frame, matching the
/// length-prefix / raw-stream framers' behavior but with a distinct
/// type for use with the legacy `MessageFramer`-based stack
/// (parser modules still consume this; the v3.3 net layer uses the
/// `eph::core::StreamCodec` concept directly via `eph-codec/ws_codec`).

#include <cstring>

#include "eph/core/framer_concept.hpp"

namespace eph::json {

/// @brief Pass-through framer for JSON-over-WebSocket.
///
/// Since WebSocket handles message boundaries, each recv callback
/// delivers exactly one JSON message. This framer simply wraps the
/// raw bytes as a `DecodedFrame` for downstream consumers that operate
/// on the `MessageFramer` contract.
///
/// Satisfies the eph::net::MessageFramer concept with zero framing
/// overhead -- encode is a plain memcpy, decode wraps the buffer
/// in a DecodedFrame without copying or transforming any bytes.
///
/// @note This framer assumes the transport layer (WebSocket) has
///       already delineated message boundaries. Using it over a raw
///       TCP stream will treat the entire recv buffer as one message.
class JsonFramer {
public:
    /// @brief Returns the maximum framing overhead in bytes.
    ///
    /// Always zero -- JSON-over-WebSocket adds no extra framing bytes
    /// beyond the WebSocket frame header (handled by the WS layer).
    ///
    /// @return Always 0.
    static constexpr size_t max_overhead() noexcept { return 0; }

    /// @brief Encode a JSON message for transmission (identity copy).
    ///
    /// Copies @p len bytes from @p data into @p out without modification.
    /// The caller must ensure @p out has at least @p len bytes of capacity.
    ///
    /// @param out       Destination buffer (must have capacity >= @p len)
    /// @param data      Source JSON payload bytes
    /// @param len       Number of bytes to encode
    /// @param msg_type  Ignored (WebSocket message type is set elsewhere)
    /// @return Number of bytes written to @p out, or 0 if any pointer is null
    ///         or @p len is zero.
    [[nodiscard]] size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t /*msg_type*/) noexcept {
        if (len == 0 || !data || !out) [[unlikely]] return 0;
        std::memcpy(out, data, len);
        return len;
    }

    /// @brief Decode a received buffer into a DecodedFrame (identity wrap).
    ///
    /// Wraps the entire buffer as a single frame with msg_type 0 and
    /// is_control false. No data is copied or transformed.
    ///
    /// @param data  Received payload bytes
    /// @param len   Number of received bytes
    /// @return DecodedFrame spanning the full buffer on success,
    ///         or FrameError::kIncomplete if @p len is zero.
    [[nodiscard]] std::expected<eph::net::DecodedFrame, eph::net::FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        if (len == 0) return std::unexpected(eph::net::FrameError::kIncomplete);
        return eph::net::DecodedFrame{
            .payload     = data,
            .payload_len = len,
            .msg_type    = 0,
            .is_control  = false,
            .total_len   = len,
        };
    }
};

static_assert(eph::net::MessageFramer<JsonFramer>,
    "JsonFramer must satisfy MessageFramer concept");

} // namespace eph::json
