#pragma once

/// @file websocket.hpp
/// WebSocket protocol implementation (RFC 6455) for DPDK transport.
///
/// Supports:
///   - Binary and text frames
///   - Client masking (required by RFC 6455)
///   - Ping/pong automatic response
///   - Close frame graceful shutdown
///   - Frame header template precomputation for hot path
///
/// Frame header sizes by payload length:
///   - [0, 125]:     2 bytes + 4 bytes mask = 6 bytes
///   - [126, 65535]:  2 + 2 bytes + 4 bytes mask = 8 bytes
///   - [65536+]:      2 + 8 bytes + 4 bytes mask = 14 bytes

#include <cstdint>
#include <cstring>
#include <expected>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/rand.h>

namespace eph::dpdk::ws {

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket constants
// ─────────────────────────────────────────────────────────────────────────────

namespace opcode {
inline constexpr uint8_t kContinuation = 0x0;
inline constexpr uint8_t kText         = 0x1;
inline constexpr uint8_t kBinary       = 0x2;
inline constexpr uint8_t kClose        = 0x8;
inline constexpr uint8_t kPing         = 0x9;
inline constexpr uint8_t kPong         = 0xA;
} // namespace opcode

// Close status codes
namespace close_code {
inline constexpr uint16_t kNormal           = 1000;
inline constexpr uint16_t kGoingAway        = 1001;
inline constexpr uint16_t kProtocolError    = 1002;
inline constexpr uint16_t kUnsupportedData  = 1003;
inline constexpr uint16_t kAbnormalClosure  = 1006;
inline constexpr uint16_t kInvalidPayload   = 1007;
inline constexpr uint16_t kPolicyViolation  = 1008;
inline constexpr uint16_t kMessageTooBig    = 1009;
} // namespace close_code

inline constexpr uint8_t kFinBit  = 0x80;
inline constexpr uint8_t kMaskBit = 0x80;

// Maximum header size: 2 (base) + 8 (extended length) + 4 (mask) = 14
inline constexpr size_t kMaxFrameHeaderLen = 14;
// Minimum header size: 2 (base) + 4 (mask) = 6
inline constexpr size_t kMinFrameHeaderLen = 6;

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::shared_ptr<spdlog::logger> ws_logger() {
    static auto l = [] {
        auto lg = spdlog::stdout_color_mt("dpdk.websocket");
        lg->set_level(spdlog::level::trace);
        return lg;
    }();
    return l;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Masking (RFC 6455 Section 5.3)
// ─────────────────────────────────────────────────────────────────────────────

/// Apply XOR masking to payload (in-place). Client frames MUST be masked.
inline void apply_mask(uint8_t* data, size_t len,
                       const uint8_t mask[4]) noexcept {
    // Process 4 bytes at a time for performance
    size_t i = 0;
    uint32_t mask32;
    std::memcpy(&mask32, mask, 4);

    for (; i + 4 <= len; i += 4) {
        uint32_t block;
        std::memcpy(&block, data + i, 4);
        block ^= mask32;
        std::memcpy(data + i, &block, 4);
    }
    // Handle remaining bytes
    for (; i < len; ++i) {
        data[i] ^= mask[i & 3];
    }
}

/// Generate a random 4-byte masking key.
inline void generate_mask_key(uint8_t mask[4]) noexcept {
    RAND_bytes(mask, 4);
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame encoder
// ─────────────────────────────────────────────────────────────────────────────

/// Encode a WebSocket frame header.
///
/// @param out          Output buffer (must have at least kMaxFrameHeaderLen bytes)
/// @param opcode_val   Frame opcode (kBinary, kText, kPing, etc.)
/// @param payload_len  Payload length
/// @param fin          FIN bit (true for complete messages)
/// @param mask_key     4-byte mask key (client MUST mask)
/// @return Number of header bytes written
inline size_t encode_frame_header(uint8_t* out, uint8_t opcode_val,
                                   uint64_t payload_len, bool fin,
                                   const uint8_t mask_key[4]) noexcept {
    size_t pos = 0;

    // Byte 0: FIN + opcode
    out[pos++] = (fin ? kFinBit : 0) | (opcode_val & 0x0F);

    // Byte 1: MASK bit + payload length
    if (payload_len < 126) {
        out[pos++] = kMaskBit | static_cast<uint8_t>(payload_len);
    } else if (payload_len <= 65535) {
        out[pos++] = kMaskBit | 126;
        out[pos++] = static_cast<uint8_t>(payload_len >> 8);
        out[pos++] = static_cast<uint8_t>(payload_len & 0xFF);
    } else {
        out[pos++] = kMaskBit | 127;
        for (int i = 7; i >= 0; --i) {
            out[pos++] = static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF);
        }
    }

    // Masking key (4 bytes)
    std::memcpy(out + pos, mask_key, 4);
    pos += 4;

    return pos;
}

/// Compute the frame header size for a given payload length (client, masked).
constexpr size_t frame_header_size(uint64_t payload_len) noexcept {
    if (payload_len < 126)  return 2 + 4; // 6 bytes
    if (payload_len <= 65535) return 2 + 2 + 4; // 8 bytes
    return 2 + 8 + 4; // 14 bytes
}

/// Compute total frame size (header + payload).
constexpr size_t total_frame_size(uint64_t payload_len) noexcept {
    return frame_header_size(payload_len) + payload_len;
}

/// Encode a complete WebSocket frame (header + masked payload) into a buffer.
///
/// @param out          Output buffer (must be large enough for header + payload)
/// @param opcode_val   Frame opcode
/// @param payload      Payload data
/// @param payload_len  Payload length
/// @param fin          FIN bit
/// @return Total bytes written (header + masked payload)
inline size_t encode_frame(uint8_t* out, uint8_t opcode_val,
                            const uint8_t* payload, uint64_t payload_len,
                            bool fin = true) noexcept {
    uint8_t mask_key[4];
    generate_mask_key(mask_key);

    size_t header_len = encode_frame_header(out, opcode_val, payload_len,
                                             fin, mask_key);

    // Copy payload and apply mask
    if (payload && payload_len > 0) {
        std::memcpy(out + header_len, payload, payload_len);
        apply_mask(out + header_len, payload_len, mask_key);
    }

    return header_len + payload_len;
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame decoder
// ─────────────────────────────────────────────────────────────────────────────

/// Decoded WebSocket frame (zero-copy view into the original buffer).
struct DecodedFrame {
    uint8_t        opcode       = 0;
    bool           fin          = false;
    bool           masked       = false;
    uint64_t       payload_len  = 0;
    const uint8_t* payload      = nullptr;  // Points into source buffer
    uint8_t        mask_key[4]  = {};
    size_t         total_len    = 0;  // Total frame bytes consumed

    [[nodiscard]] bool is_control() const noexcept {
        return (opcode & 0x08) != 0;
    }

    [[nodiscard]] bool is_data() const noexcept {
        return opcode == opcode::kText || opcode == opcode::kBinary ||
               opcode == opcode::kContinuation;
    }

    [[nodiscard]] bool is_ping() const noexcept {
        return opcode == opcode::kPing;
    }

    [[nodiscard]] bool is_pong() const noexcept {
        return opcode == opcode::kPong;
    }

    [[nodiscard]] bool is_close() const noexcept {
        return opcode == opcode::kClose;
    }

    /// For close frames: extract the 2-byte status code.
    [[nodiscard]] uint16_t close_status_code() const noexcept {
        if (opcode != opcode::kClose || payload_len < 2 || !payload) {
            return 0;
        }
        return static_cast<uint16_t>((payload[0] << 8) | payload[1]);
    }
};

/// Decode a WebSocket frame from a buffer.
///
/// @param data     Input buffer
/// @param len      Available bytes
/// @return Decoded frame, or error if incomplete/malformed.
///         "incomplete" error means more data is needed.
inline std::expected<DecodedFrame, std::string>
decode_frame(const uint8_t* data, size_t len) {
    if (len < 2) {
        return std::unexpected("incomplete");
    }

    DecodedFrame frame;
    size_t pos = 0;

    // Byte 0: FIN + opcode
    frame.fin = (data[pos] & kFinBit) != 0;
    frame.opcode = data[pos] & 0x0F;
    pos++;

    // Byte 1: MASK + payload length
    frame.masked = (data[pos] & kMaskBit) != 0;
    uint8_t len_byte = data[pos] & 0x7F;
    pos++;

    // Extended payload length
    if (len_byte < 126) {
        frame.payload_len = len_byte;
    } else if (len_byte == 126) {
        if (len < pos + 2) return std::unexpected("incomplete");
        frame.payload_len = static_cast<uint64_t>(data[pos]) << 8 |
                            static_cast<uint64_t>(data[pos + 1]);
        pos += 2;
    } else { // 127
        if (len < pos + 8) return std::unexpected("incomplete");
        frame.payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            frame.payload_len = (frame.payload_len << 8) | data[pos + i];
        }
        pos += 8;
    }

    // Masking key (if present — server frames are typically unmasked)
    if (frame.masked) {
        if (len < pos + 4) return std::unexpected("incomplete");
        std::memcpy(frame.mask_key, data + pos, 4);
        pos += 4;
    }

    // Payload
    if (len < pos + frame.payload_len) {
        return std::unexpected("incomplete");
    }

    frame.payload = data + pos;
    frame.total_len = pos + frame.payload_len;

    return frame;
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket connection (protocol-level operations)
// ─────────────────────────────────────────────────────────────────────────────

/// Build a Close frame with status code.
/// @param out          Output buffer (at least 8 bytes for small close frame)
/// @param status_code  Close reason code (e.g. close_code::kNormal)
/// @param reason       Optional close reason string (max 123 bytes)
/// @return Total frame bytes written
inline size_t build_close_frame(uint8_t* out, uint16_t status_code,
                                 std::string_view reason = {}) noexcept {
    // Close payload: 2-byte status code + optional reason
    size_t close_payload_len = 2 + reason.size();
    uint8_t close_payload[125]; // Max control frame payload
    close_payload[0] = static_cast<uint8_t>(status_code >> 8);
    close_payload[1] = static_cast<uint8_t>(status_code & 0xFF);
    if (!reason.empty()) {
        std::memcpy(close_payload + 2, reason.data(),
                    std::min(reason.size(), size_t{123}));
    }

    return encode_frame(out, opcode::kClose,
                        close_payload, close_payload_len);
}

/// Build a Pong response frame (echo back the ping payload).
inline size_t build_pong_frame(uint8_t* out, const uint8_t* ping_payload,
                                uint64_t payload_len) noexcept {
    return encode_frame(out, opcode::kPong, ping_payload, payload_len);
}

/// Build a Ping frame.
inline size_t build_ping_frame(uint8_t* out,
                                const uint8_t* payload = nullptr,
                                uint64_t payload_len = 0) noexcept {
    return encode_frame(out, opcode::kPing, payload, payload_len);
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame template for hot path (precomputed headers)
// ─────────────────────────────────────────────────────────────────────────────

/// Precomputed frame template for a fixed opcode and payload size range.
/// Used on the hot path to avoid recomputing frame headers.
///
/// Usage:
///   auto tmpl = FrameTemplate::for_binary(max_payload);
///   // On hot path:
///   size_t n = tmpl.encode(out, payload_data, actual_len);
struct FrameTemplate {
    uint8_t opcode_val = opcode::kBinary;
    bool    fin        = true;

    /// Encode a frame using this template's opcode and FIN settings.
    /// @param out          Output buffer
    /// @param payload      Payload data
    /// @param payload_len  Actual payload length
    /// @return Total bytes written
    size_t encode(uint8_t* out, const uint8_t* payload,
                  uint64_t payload_len) const noexcept {
        return encode_frame(out, opcode_val, payload, payload_len, fin);
    }

    /// Create a template for binary frames.
    static constexpr FrameTemplate for_binary() noexcept {
        return FrameTemplate{.opcode_val = opcode::kBinary, .fin = true};
    }

    /// Create a template for text frames.
    static constexpr FrameTemplate for_text() noexcept {
        return FrameTemplate{.opcode_val = opcode::kText, .fin = true};
    }
};

} // namespace eph::dpdk::ws
