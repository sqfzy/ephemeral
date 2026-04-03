#pragma once

/// @file raw_framer.hpp
/// Pass-through framer — no framing overhead, delivers raw bytes.
///
/// Use with protocols that handle their own message boundaries (e.g., FIX
/// with SOH-delimited fields, or when the application manages framing).

#include <cstring>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/core/framer_concept.hpp"

namespace eph::net {

namespace detail {
inline spdlog::logger* raw_framer_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.raw_framer");
        if (!lg) lg = spdlog::stdout_color_mt("net.raw_framer");
        return lg;
    }();
    return l.get();
}
} // namespace detail

/// Pass-through framer: no framing header, raw byte delivery.
///
/// encode() copies data unchanged. decode() returns the entire buffer
/// as a single frame. This framer is suitable for protocols where the
/// application or a higher-level parser handles message boundaries.
class RawFramer {
public:
    /// Maximum framing overhead in bytes (zero for pass-through).
    static constexpr size_t max_overhead() noexcept { return 0; }

    /// Encode raw bytes (pass-through copy).
    /// @pre Caller must ensure `out` points to a buffer with at least `len` bytes available.
    [[nodiscard]] size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t /*msg_type*/) noexcept {
        if (len == 0 || !data || !out) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::raw_framer_logger(),
                        "RawFramer::encode: invalid args len={} data={} out={} "
                        "(caller bug: null pointer or zero length)",
                        len, fmt::ptr(data), fmt::ptr(out));
            return 0;
        }
        std::memcpy(out, data, len);
        return len;
    }

    /// Decode raw bytes as a single frame (pass-through).
    ///
    /// Returns the entire available buffer as one frame. Returns
    /// FrameError::kIncomplete only when no data is available.
    ///
    /// @param data  Input buffer
    /// @param len   Available bytes in input buffer
    /// @return Decoded frame spanning all available data, or kIncomplete if len == 0
    [[nodiscard]] std::expected<DecodedFrame, FrameError>
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
