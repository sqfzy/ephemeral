#pragma once

/// @file framer.hpp
/// JSON framer satisfying the eph::net::MessageFramer concept.
///
/// WebSocket already provides message framing, so JSON payloads arrive
/// as complete messages. This framer is a semantic pass-through: it
/// delivers the entire payload as a single frame, matching RawFramer
/// behavior but with a distinct type for Transport type aliases.

#include <cstring>

#include "eph/core/framer_concept.hpp"

namespace eph::json {

/// Pass-through framer for JSON-over-WebSocket.
///
/// Since WebSocket handles message boundaries, each recv callback
/// delivers exactly one JSON message. This framer simply wraps the
/// raw bytes as a DecodedFrame for the Transport pipeline.
class JsonFramer {
public:
    static constexpr size_t max_overhead() noexcept { return 0; }

    [[nodiscard]] size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t /*msg_type*/) noexcept {
        if (len == 0 || !data || !out) [[unlikely]] return 0;
        std::memcpy(out, data, len);
        return len;
    }

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
