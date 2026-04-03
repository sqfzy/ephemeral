#pragma once

/// @file websocket.hpp
/// WebSocket protocol implementation (RFC 6455).
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

#include <cassert>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <expected>
#include <format>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/rand.h>

namespace eph::net::ws {

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket constants
// ─────────────────────────────────────────────────────────────────────────────

/// WebSocket frame opcodes (RFC 6455 section 5.2).
namespace opcode {
inline constexpr uint8_t kContinuation = 0x0; ///< Continuation frame (fragmented message)
inline constexpr uint8_t kText         = 0x1; ///< Text data frame (payload must be UTF-8)
inline constexpr uint8_t kBinary       = 0x2; ///< Binary data frame
inline constexpr uint8_t kClose        = 0x8; ///< Connection Close control frame
inline constexpr uint8_t kPing         = 0x9; ///< Ping control frame (keepalive probe)
inline constexpr uint8_t kPong         = 0xA; ///< Pong control frame (keepalive response)
} // namespace opcode

/// WebSocket close status codes (RFC 6455 section 7.4.1).
namespace close_code {
inline constexpr uint16_t kNormal           = 1000; ///< Normal closure (purpose fulfilled)
inline constexpr uint16_t kGoingAway        = 1001; ///< Endpoint going away (server shutdown)
inline constexpr uint16_t kProtocolError    = 1002; ///< Protocol error detected
inline constexpr uint16_t kUnsupportedData  = 1003; ///< Unsupported data type received
inline constexpr uint16_t kAbnormalClosure  = 1006; ///< Abnormal closure (no Close frame sent)
inline constexpr uint16_t kInvalidPayload   = 1007; ///< Invalid payload data (e.g., bad UTF-8 in text frame)
inline constexpr uint16_t kPolicyViolation  = 1008; ///< Policy violation
inline constexpr uint16_t kMessageTooBig    = 1009; ///< Message too big for the endpoint to process
inline constexpr uint16_t kMandatoryExtension = 1010; ///< Required extension not negotiated
inline constexpr uint16_t kInternalError    = 1011; ///< Internal server error
} // namespace close_code

/// Human-readable name for a WebSocket opcode value.
/// Returns "UNKNOWN(0xNN)" for unrecognized opcodes.
/// Known opcodes return a constexpr string_view (zero allocation).
/// Unknown opcodes fall back to a thread-local formatted string.
inline std::string_view opcode_name(uint8_t op) noexcept {
    switch (op) {
    case opcode::kContinuation: return "CONTINUATION";
    case opcode::kText:         return "TEXT";
    case opcode::kBinary:       return "BINARY";
    case opcode::kClose:        return "CLOSE";
    case opcode::kPing:         return "PING";
    case opcode::kPong:         return "PONG";
    default: [[unlikely]] {
        // Thread-local buffer avoids heap allocation on every call.
        // 16 bytes is enough for "UNKNOWN(0xNN)\0".
        thread_local char buf[16];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf) - 1, op, 16);
        // Format as "UNKNOWN(0xNN)" manually to avoid std::format overhead
        constexpr std::string_view prefix = "UNKNOWN(0x";
        thread_local char out[24];
        std::memcpy(out, prefix.data(), prefix.size());
        size_t digits = static_cast<size_t>(ptr - buf);
        // Pad to 2 hex digits
        size_t pos = prefix.size();
        if (digits < 2) out[pos++] = '0';
        std::memcpy(out + pos, buf, digits);
        pos += digits;
        out[pos++] = ')';
        return {out, pos};
    }
    }
}

/// Human-readable name for a WebSocket close status code (RFC 6455 §7.4).
/// Returns "UNKNOWN(NNNN)" for unrecognized codes.
/// Known codes return a constexpr string_view (zero allocation).
/// Unknown/registered/private codes use a thread-local formatted buffer.
inline std::string_view close_code_name(uint16_t code) noexcept {
    switch (code) {
    case close_code::kNormal:             return "NORMAL_CLOSURE";
    case close_code::kGoingAway:          return "GOING_AWAY";
    case close_code::kProtocolError:      return "PROTOCOL_ERROR";
    case close_code::kUnsupportedData:    return "UNSUPPORTED_DATA";
    case close_code::kAbnormalClosure:    return "ABNORMAL_CLOSURE";
    case close_code::kInvalidPayload:     return "INVALID_PAYLOAD";
    case close_code::kPolicyViolation:    return "POLICY_VIOLATION";
    case close_code::kMessageTooBig:      return "MESSAGE_TOO_BIG";
    case close_code::kMandatoryExtension: return "MANDATORY_EXTENSION";
    case close_code::kInternalError:      return "INTERNAL_ERROR";
    default: [[unlikely]] {
        // Thread-local buffer avoids heap allocation. Max: "REGISTERED(NNNNN)\0" = 18 chars.
        thread_local char buf[24];
        const char* prefix;
        size_t prefix_len;
        if (code >= 3000 && code <= 3999) {
            prefix = "REGISTERED("; prefix_len = 11;
        } else if (code >= 4000 && code <= 4999) {
            prefix = "PRIVATE("; prefix_len = 8;
        } else {
            prefix = "UNKNOWN("; prefix_len = 8;
        }
        std::memcpy(buf, prefix, prefix_len);
        auto [ptr, ec] = std::to_chars(buf + prefix_len, buf + sizeof(buf) - 1, code);
        *ptr++ = ')';
        return {buf, static_cast<size_t>(ptr - buf)};
    }
    }
}

/// Check if a close status code is valid for sending per RFC 6455 §7.4.
/// Valid ranges: 1000-1003, 1007-1011, 3000-4999.
/// Codes 1004-1006 and 1015 are reserved and MUST NOT be sent in a Close frame.
constexpr bool is_valid_close_code(uint16_t code) noexcept {
    // Standard codes that may be sent
    if (code >= 1000 && code <= 1003) return true;
    if (code >= 1007 && code <= 1011) return true;
    // Registered (IANA) and private-use ranges
    if (code >= 3000 && code <= 4999) return true;
    return false;
}

/// Error codes returned by decode_frame() for programmatic handling.
enum class DecodeError : uint8_t {
    kIncomplete,            ///< Need more data (partial frame)
    kReservedBits,          ///< Non-zero RSV bits without extension
    kFragmentedControl,     ///< Control frame with FIN=0
    kControlPayloadTooLarge,///< Control frame payload > 125 bytes
    kInvalidOpcode,         ///< Reserved opcode (0x3-0x7, 0xB-0xF per RFC 6455 §5.2)
};

/// Human-readable name for a DecodeError value.
constexpr std::string_view decode_error_name(DecodeError e) noexcept {
    switch (e) {
    case DecodeError::kIncomplete:             return "incomplete";
    case DecodeError::kReservedBits:           return "non-zero RSV bits without negotiated extension";
    case DecodeError::kFragmentedControl:      return "fragmented control frame";
    case DecodeError::kControlPayloadTooLarge: return "control frame payload exceeds 125 bytes";
    case DecodeError::kInvalidOpcode:          return "reserved opcode (RFC 6455 §5.2)";
    }
    return "unknown";
}

inline constexpr uint8_t kFinBit  = 0x80; ///< FIN bit mask in frame byte 0
inline constexpr uint8_t kMaskBit = 0x80; ///< MASK bit mask in frame byte 1

inline constexpr size_t kMaxFrameHeaderLen = 14;  ///< Max header: 2 (base) + 8 (extended length) + 4 (mask)
inline constexpr size_t kMinFrameHeaderLen = 6;   ///< Min header: 2 (base) + 4 (mask key)
inline constexpr uint64_t kMaxPayloadLen = (uint64_t{1} << 63) - 1; ///< Max payload (MSB must be 0, RFC 6455 section 5.2)
inline constexpr size_t kMaxControlPayloadLen = 125; ///< Max control frame payload (RFC 6455 section 5.5)

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @return Pointer to the "net.websocket" spdlog logger.
inline spdlog::logger* ws_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.websocket");
        if (!lg) lg = spdlog::stdout_color_mt("net.websocket");
        return lg;
    }();
    return l.get();
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Masking (RFC 6455 Section 5.3)
// ─────────────────────────────────────────────────────────────────────────────

/// Apply XOR masking to payload (in-place). Client frames MUST be masked.
inline void apply_mask(uint8_t* data, size_t len,
                       const uint8_t mask[4]) noexcept {
    size_t i = 0;
    uint32_t mask32;
    std::memcpy(&mask32, mask, 4);

    for (; i + 4 <= len; i += 4) {
        uint32_t block;
        std::memcpy(&block, data + i, 4);
        block ^= mask32;
        std::memcpy(data + i, &block, 4);
    }
    for (; i < len; ++i) {
        data[i] ^= mask[i & 3];
    }
}

/// Fused memcpy + XOR masking in a single pass.
/// Copies src to dst while applying the 4-byte mask. Uses 64-bit blocks
/// for the main loop to halve the iteration count vs 32-bit.
inline void masked_copy(uint8_t* dst, const uint8_t* src,
                         size_t len, const uint8_t mask[4]) noexcept {
    uint32_t mask32;
    std::memcpy(&mask32, mask, 4);

    // 64-bit main loop: process 8 bytes per iteration
    uint64_t mask64 = (static_cast<uint64_t>(mask32) << 32) | mask32;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t block;
        std::memcpy(&block, src + i, 8);
        block ^= mask64;
        std::memcpy(dst + i, &block, 8);
    }

    // 32-bit tail
    if (i + 4 <= len) {
        uint32_t block;
        std::memcpy(&block, src + i, 4);
        block ^= mask32;
        std::memcpy(dst + i, &block, 4);
        i += 4;
    }

    // Byte tail (0-3 bytes)
    for (; i < len; ++i) {
        dst[i] = src[i] ^ mask[i & 3];
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// [P0] Batch-pregenerated mask key cache
// ─────────────────────────────────────────────────────────────────────────────

/// CSPRNG-backed mask key pool. Batch-generates 1024 keys via RAND_bytes
/// and serves them sequentially on the hot path (~2ns per key vs ~1500ns
/// for per-frame RAND_bytes). Refills automatically when exhausted.
class MaskKeyCache {
public:
    static constexpr size_t kPoolSize = 1024;

    MaskKeyCache() noexcept { refill(); }

    /// Get the next 4-byte mask key (hot path: single index increment).
    void next_key(uint8_t out[4]) noexcept {
        if (pos_ >= kPoolSize) [[unlikely]] {
            refill();
        }
        std::memcpy(out, &pool_[pos_ * 4], 4);
        pos_++;
    }

private:
    void refill() noexcept {
        if (RAND_bytes(pool_, sizeof(pool_)) != 1) {
            SPDLOG_LOGGER_ERROR(detail::ws_logger(),
                "RAND_bytes failed for mask key cache, using fallback");
            // Fallback: use a seeded LCG with TSC entropy (not crypto-secure,
            // but masking is only an anti-cache-poisoning measure per RFC 6455 §5.3)
#if defined(__x86_64__) || defined(_M_X64)
            uint64_t seed = __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
            uint64_t seed; asm volatile("mrs %0, cntvct_el0" : "=r"(seed));
#else
            uint64_t seed = static_cast<uint64_t>(time(nullptr));
#endif
            // SplitMix64 — well-distributed, passes BigCrush statistical tests
            for (size_t i = 0; i < sizeof(pool_); i += 8) {
                seed += 0x9e3779b97f4a7c15ULL;
                uint64_t z = seed;
                z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
                z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
                z = z ^ (z >> 31);
                size_t n = std::min(sizeof(pool_) - i, size_t{8});
                std::memcpy(pool_ + i, &z, n);
            }
        }
        pos_ = 0;
    }

    uint8_t pool_[kPoolSize * 4]{};
    size_t  pos_ = 0;
};

/// Thread-local mask key cache (hot path allocation-free).
inline MaskKeyCache& mask_key_cache() noexcept {
    thread_local MaskKeyCache cache;
    return cache;
}

/// Generate a random 4-byte masking key (uses batch cache on hot path).
inline void generate_mask_key(uint8_t mask[4]) noexcept {
    mask_key_cache().next_key(mask);
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame encoder
// ─────────────────────────────────────────────────────────────────────────────

/// Encode a WebSocket frame header.
///
/// @param out          Output buffer (must have at least kMaxFrameHeaderLen bytes)
/// @param opcode_val   Frame opcode (kBinary, kText, kPing, etc.)
/// @param payload_len  Payload length (must not exceed kMaxPayloadLen;
///                     control frames limited to kMaxControlPayloadLen)
/// @param fin          FIN bit (true for complete messages)
/// @param mask_key     4-byte mask key (client MUST mask)
/// @return Number of header bytes written, or 0 if payload_len is invalid
inline size_t encode_frame_header(uint8_t* out, uint8_t opcode_val,
                                   uint64_t payload_len, bool fin,
                                   const uint8_t mask_key[4]) noexcept {
    // RFC 6455 §5.2: MSB of 64-bit payload length must be 0
    if (payload_len > kMaxPayloadLen) [[unlikely]] {
        SPDLOG_LOGGER_ERROR(detail::ws_logger(),
            "payload_len {} exceeds maximum {} (MSB must be 0)",
            payload_len, kMaxPayloadLen);
        return 0;
    }
    // RFC 6455 §5.5: control frame payload must not exceed 125 bytes
    if ((opcode_val & 0x08) && payload_len > kMaxControlPayloadLen) [[unlikely]] {
        SPDLOG_LOGGER_ERROR(detail::ws_logger(),
            "control frame (opcode=0x{:02X}) payload_len {} exceeds limit {}",
            opcode_val, payload_len, kMaxControlPayloadLen);
        return 0;
    }

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

/// Check whether a payload length is valid for the given opcode.
constexpr bool is_valid_payload_len(uint8_t opcode_val,
                                     uint64_t payload_len) noexcept {
    if (payload_len > kMaxPayloadLen) return false;
    if ((opcode_val & 0x08) && payload_len > kMaxControlPayloadLen) return false;
    return true;
}

/// Compute the frame header size for a given payload length (client, masked).
constexpr size_t frame_header_size(uint64_t payload_len) noexcept {
    if (payload_len < 126)  return 2 + 4; // 6 bytes
    if (payload_len <= 65535) return 2 + 2 + 4; // 8 bytes
    return 2 + 8 + 4; // 14 bytes
}

/// Compute total frame size (header + payload).
/// Returns SIZE_MAX on overflow (payload_len too large to represent).
constexpr size_t total_frame_size(uint64_t payload_len) noexcept {
    auto header = frame_header_size(payload_len);
    if (payload_len > SIZE_MAX - header) return SIZE_MAX;
    return header + static_cast<size_t>(payload_len);
}

/// Encode a complete WebSocket frame (header + masked payload) into a buffer.
///
/// @param out          Output buffer (must be large enough for header + payload)
/// @param opcode_val   Frame opcode
/// @param payload      Payload data
/// @param payload_len  Payload length
/// @param fin          FIN bit
/// @return Total bytes written (header + masked payload)
[[nodiscard]] inline size_t encode_frame(uint8_t* out, uint8_t opcode_val,
                            const uint8_t* payload, uint64_t payload_len,
                            bool fin = true) noexcept {
    uint8_t mask_key[4];
    generate_mask_key(mask_key);

    size_t header_len = encode_frame_header(out, opcode_val, payload_len,
                                             fin, mask_key);
    if (header_len == 0) [[unlikely]] {
        return 0; // Validation failed (logged by encode_frame_header)
    }

    // [P1] Single-pass copy + mask (fused memcpy + XOR)
    if (payload && payload_len > 0) {
        masked_copy(out + header_len, payload,
                    static_cast<size_t>(payload_len), mask_key);
    }

    return header_len + static_cast<size_t>(payload_len);
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame decoder
// ─────────────────────────────────────────────────────────────────────────────

/// Decoded WebSocket frame (zero-copy view into the original buffer).
///
/// All pointers reference the original input buffer passed to decode_frame().
/// The caller must not free or modify that buffer while accessing this struct.
struct DecodedFrame {
    uint8_t        opcode       = 0;       ///< Frame opcode (text, binary, ping, pong, close, continuation)
    bool           fin          = false;    ///< FIN bit: true if this is the final fragment
    bool           masked       = false;    ///< True if payload is masked (client-to-server frames)
    uint64_t       payload_len  = 0;       ///< Payload length in bytes
    const uint8_t* payload      = nullptr; ///< Pointer into source buffer (zero-copy)
    uint8_t        mask_key[4]  = {};      ///< 4-byte masking key (valid only if masked==true)
    size_t         total_len    = 0;       ///< Total frame bytes consumed from the input buffer

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

    /// For close frames: extract the reason string (after the 2-byte status code).
    /// Returns empty string_view if not a close frame or no reason present.
    /// @note For masked (client-sent) frames, the returned view contains masked
    ///       bytes. Unmask the payload first with apply_mask() if needed.
    [[nodiscard]] std::string_view close_reason() const noexcept {
        if (opcode != opcode::kClose || payload_len <= 2 || !payload) {
            return {};
        }
        return {reinterpret_cast<const char*>(payload + 2),
                static_cast<size_t>(payload_len - 2)};
    }
};

/// Decode a WebSocket frame from a buffer.
///
/// @param data     Input buffer
/// @param len      Available bytes
/// @return Decoded frame, or error if incomplete/malformed.
///         "incomplete" error means more data is needed.
[[nodiscard]] inline std::expected<DecodedFrame, DecodeError>
decode_frame(const uint8_t* data, size_t len) {
    if (len < 2) {
        return std::unexpected(DecodeError::kIncomplete);
    }

    DecodedFrame frame;
    size_t pos = 0;

    // Byte 0: FIN + opcode
    frame.fin = (data[pos] & kFinBit) != 0;
    frame.opcode = data[pos] & 0x0F;

    // RFC 6455 §5.2: RSV1-3 must be 0 unless an extension is negotiated.
    // Check here — before any length parsing — so malformed frames are
    // rejected early without consuming variable-length fields.
    if (data[pos] & 0x70) {
        SPDLOG_LOGGER_WARN(detail::ws_logger(),
            "decode_frame: non-zero RSV bits 0x{:02X} in byte0=0x{:02X} "
            "(no extensions negotiated)",
            data[pos] & 0x70, data[pos]);
        return std::unexpected(DecodeError::kReservedBits);
    }
    pos++;

    // Byte 1: MASK + payload length
    frame.masked = (data[pos] & kMaskBit) != 0;
    uint8_t len_byte = data[pos] & 0x7F;
    pos++;

    // Extended payload length
    if (len_byte < 126) {
        frame.payload_len = len_byte;
    } else if (len_byte == 126) {
        if (len < pos + 2) return std::unexpected(DecodeError::kIncomplete);
        frame.payload_len = static_cast<uint64_t>(data[pos]) << 8 |
                            static_cast<uint64_t>(data[pos + 1]);
        pos += 2;
    } else { // 127
        if (len < pos + 8) return std::unexpected(DecodeError::kIncomplete);
        frame.payload_len = 0;
        for (size_t i = 0; i < 8; ++i) {
            frame.payload_len = (frame.payload_len << 8) | data[pos + i];
        }
        pos += 8;
    }

    // Masking key (if present — server frames are typically unmasked)
    if (frame.masked) {
        if (len < pos + 4) return std::unexpected(DecodeError::kIncomplete);
        std::memcpy(frame.mask_key, data + pos, 4);
        pos += 4;
    }

    // Payload — check pos first, then use subtraction which is safe when pos <= len
    if (pos > len || frame.payload_len > len - pos) {
        return std::unexpected(DecodeError::kIncomplete);
    }

    // RFC 6455 §5.2: opcodes 0x3-0x7 (data) and 0xB-0xF (control) are reserved
    if ((frame.opcode >= 0x3 && frame.opcode <= 0x7) ||
        (frame.opcode >= 0xB && frame.opcode <= 0xF)) {
        SPDLOG_LOGGER_WARN(detail::ws_logger(),
            "decode_frame: reserved opcode 0x{:02X} (RFC 6455 §5.2), "
            "payload_len={}",
            frame.opcode, frame.payload_len);
        return std::unexpected(DecodeError::kInvalidOpcode);
    }

    // RFC 6455 §5.5: control frames MUST have FIN=1 and payload <= 125
    if (frame.opcode & 0x08) {
        if (!frame.fin) {
            SPDLOG_LOGGER_WARN(detail::ws_logger(),
                "decode_frame: fragmented control frame opcode=0x{:02X} "
                "(FIN=0, RFC 6455 §5.5)",
                frame.opcode);
            return std::unexpected(DecodeError::kFragmentedControl);
        }
        if (frame.payload_len > 125) {
            SPDLOG_LOGGER_WARN(detail::ws_logger(),
                "decode_frame: control frame opcode=0x{:02X} payload_len={} "
                "exceeds 125-byte limit (RFC 6455 §5.5)",
                frame.opcode, frame.payload_len);
            return std::unexpected(DecodeError::kControlPayloadTooLarge);
        }
    }

    frame.payload = data + pos;
    frame.total_len = pos + static_cast<size_t>(frame.payload_len);

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
    // Close payload: 2-byte status code + optional reason (max 123 chars)
    // Control frames MUST have payload <= 125 bytes (RFC 6455 §5.5)
    size_t reason_len = std::min(reason.size(), size_t{123});
    if (reason.size() > 123) [[unlikely]] {
        SPDLOG_LOGGER_WARN(detail::ws_logger(),
            "close frame reason truncated from {} to 123 bytes "
            "(RFC 6455 §5.5 control frame payload limit)",
            reason.size());
    }
    size_t close_payload_len = 2 + reason_len;
    uint8_t close_payload[2 + 123]; // 2-byte status + max reason
    static_assert(sizeof(close_payload) == kMaxControlPayloadLen,
                  "close_payload must fit max control frame payload");
    close_payload[0] = static_cast<uint8_t>(status_code >> 8);
    close_payload[1] = static_cast<uint8_t>(status_code & 0xFF);
    if (reason_len > 0) {
        std::memcpy(close_payload + 2, reason.data(), reason_len);
    }

    return encode_frame(out, opcode::kClose,
                        close_payload, close_payload_len);
}

/// Build a Pong response frame (echo back the ping payload).
inline size_t build_pong_frame(uint8_t* out, const uint8_t* ping_payload,
                                uint64_t payload_len) noexcept {
    // Defensively clamp length to 0 when pointer is null to prevent UB.
    // In practice the WebSocket decoder always provides a valid pointer for
    // control frames, but callers may pass nullptr + non-zero length by mistake.
    if (!ping_payload) [[unlikely]] {
        payload_len = 0;
    }
    return encode_frame(out, opcode::kPong, ping_payload, payload_len);
}

/// Build a Ping frame.
inline size_t build_ping_frame(uint8_t* out,
                                const uint8_t* payload = nullptr,
                                uint64_t payload_len = 0) noexcept {
    // Defensively clamp length to 0 when pointer is null to prevent UB.
    if (!payload) [[unlikely]] {
        payload_len = 0;
    }
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
    [[nodiscard]] size_t encode(uint8_t* out, const uint8_t* payload,
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

// ─────────────────────────────────────────────────────────────────────────────
// UTF-8 validation (RFC 6455 §5.6: text frames MUST contain valid UTF-8)
// ─────────────────────────────────────────────────────────────────────────────

/// Validate that a byte sequence is well-formed UTF-8.
///
/// Uses the Bjoern Hoehrmann DFA-based algorithm for single-pass validation
/// without branching on byte values. Suitable for hot-path use.
///
/// RFC 6455 §5.6 requires text frame payloads to be valid UTF-8.
/// This function is NOT called automatically by decode_frame() to avoid
/// adding overhead in binary-only scenarios — call it explicitly when
/// processing text frames.
///
/// @param data  Byte sequence to validate
/// @param len   Length in bytes
/// @return true if the sequence is valid UTF-8, false otherwise
inline bool is_valid_utf8(const uint8_t* data, size_t len) noexcept {
    // DFA states: 0 = accept, 12 = reject, others = intermediate
    // Lookup tables from Bjoern Hoehrmann's UTF-8 decoder/validator
    // http://bjoern.hoehrmann.de/utf-8/decoder/dfa/
    // SPDX-License-Identifier: MIT
    static constexpr uint8_t utf8d[] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 00..1f
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 20..3f
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 40..5f
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 60..7f
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, // 80..9f
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, // a0..bf
        8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, // c0..df
        0xa,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3, // e0..ef
        0xb,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8, // f0..ff
        0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1, // s0..s0
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1, // s1..s2
        1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1, // s3..s4
        1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1, // s5..s6
        1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // s7..s8
    };

    uint32_t state = 0; // 0 = accept
    for (size_t i = 0; i < len; ++i) {
        uint32_t type = utf8d[data[i]];
        state = utf8d[256 + state * 16 + type];
        if (state == 1) return false; // reject state
    }
    return state == 0; // must end in accept state
}

/// Convenience overload for span.
inline bool is_valid_utf8(std::span<const uint8_t> data) noexcept {
    return is_valid_utf8(data.data(), data.size());
}

/// Convenience overload for string_view.
inline bool is_valid_utf8(std::string_view sv) noexcept {
    return is_valid_utf8(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

} // namespace eph::net::ws

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for DecodeError
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::net::ws::DecodeError> : std::formatter<std::string_view> {
    auto format(eph::net::ws::DecodeError e, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::net::ws::decode_error_name(e), ctx);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Opcode formatter — enables std::format("{}", opcode) for logging
// ─────────────────────────────────────────────────────────────────────────────

/// Formatter wrapper for WebSocket opcodes.
/// WebSocket opcodes are plain uint8_t, not a scoped enum, so we use a
/// lightweight wrapper to opt into std::format without hijacking all uint8_t.
///
/// Usage:
///   uint8_t op = ws::opcode::kPing;
///   std::format("opcode: {}", ws::Opcode{op});  // "opcode: PING"
namespace eph::net::ws {

/// @brief Lightweight wrapper around a raw uint8_t opcode for std::format support.
struct Opcode {
    uint8_t value; ///< Raw opcode value
};

/// Formatter wrapper for WebSocket close status codes.
/// Close codes are plain uint16_t, so we use a lightweight wrapper
/// to opt into std::format without hijacking all uint16_t.
///
/// Usage:
///   uint16_t code = ws::close_code::kNormal;
///   std::format("close: {}", ws::CloseCode{code});  // "close: NORMAL_CLOSURE"
/// @brief Lightweight wrapper around a raw uint16_t close code for std::format support.
struct CloseCode {
    uint16_t value; ///< Raw close status code
};

} // namespace eph::net::ws

template <>
struct std::formatter<eph::net::ws::Opcode> : std::formatter<std::string_view> {
    auto format(eph::net::ws::Opcode op, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::net::ws::opcode_name(op.value), ctx);
    }
};

template <>
struct std::formatter<eph::net::ws::CloseCode> : std::formatter<std::string_view> {
    auto format(eph::net::ws::CloseCode cc, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::net::ws::close_code_name(cc.value), ctx);
    }
};
