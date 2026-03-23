#pragma once

/// @file raw_framer.hpp
/// Pass-through framer — no framing overhead, delivers raw bytes.
///
/// Use with protocols that handle their own message boundaries (e.g., FIX
/// with SOH-delimited fields, or when the application manages framing).

#include <cstring>
#include "eph/net/framer_concept.hpp"

namespace eph::net {

/// Pass-through framer: no framing header, raw byte delivery.
///
/// encode() copies data unchanged. decode() returns the entire buffer
/// as a single frame. This framer is suitable for protocols where the
/// application or a higher-level parser handles message boundaries.
class RawFramer {
public:
    static constexpr size_t max_overhead() noexcept { return 0; }

    size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t /*msg_type*/) noexcept {
        if (len == 0 || !data) [[unlikely]] return 0;
        std::memcpy(out, data, len);
        return len;
    }

    static std::expected<DecodedFrame, FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        if (len == 0) return std::unexpected(FrameError::kIncomplete);
        return DecodedFrame{
            .payload     = data,
            .payload_len = len,
            .msg_type    = 0,
            .is_control  = false,
            .total_len   = len,
        };
    }
};

static_assert(MessageFramer<RawFramer>, "RawFramer must satisfy MessageFramer concept");

} // namespace eph::net
