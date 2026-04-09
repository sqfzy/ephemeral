/// @file mock/lib/ws_frame.hpp
/// Server-side WebSocket frame build / parse (RFC 6455).
///
/// `build_server_frame`: emit unmasked server→client frame (FIN=1).
/// `parse_client_frame`: parse one client→server frame, unmasking the
/// payload in place (callers must own the input buffer).
///
/// Both routines support the three RFC length encodings (≤125 single-byte,
/// 126→2-byte ext, 127→8-byte ext) so the bench can sweep payloads
/// across the encoding boundary at 125/126.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>

namespace bench::mock {

// Opcodes per RFC 6455.
inline constexpr uint8_t kOpCont   = 0x00;
inline constexpr uint8_t kOpText   = 0x01;
inline constexpr uint8_t kOpBinary = 0x02;
inline constexpr uint8_t kOpClose  = 0x08;
inline constexpr uint8_t kOpPing   = 0x09;
inline constexpr uint8_t kOpPong   = 0x0A;

/// Build an unmasked server frame into `out` (must hold header + payload).
/// Returns total bytes written.
inline size_t build_server_frame(uint8_t* out, uint8_t opcode,
                                 const uint8_t* payload, size_t len) noexcept {
    out[0] = static_cast<uint8_t>(0x80 | opcode); // FIN=1
    size_t hdr;
    if (len < 126) {
        out[1] = static_cast<uint8_t>(len);
        hdr = 2;
    } else if (len < 65536) {
        out[1] = 126;
        out[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        out[3] = static_cast<uint8_t>(len & 0xFF);
        hdr = 4;
    } else {
        out[1] = 127;
        for (int i = 0; i < 8; ++i) {
            out[2 + i] = static_cast<uint8_t>((len >> (56 - 8 * i)) & 0xFF);
        }
        hdr = 10;
    }
    if (len > 0) std::memcpy(out + hdr, payload, len);
    return hdr + len;
}

/// Result of `parse_client_frame_inplace`.
struct ClientFrame {
    uint8_t  opcode = 0;
    uint64_t payload_len = 0;
    size_t   total_consumed = 0; ///< header + mask + payload bytes consumed
};

/// Parse a client→server frame from `buf[0..buf_len)`. Unmasks the
/// payload in place. Returns `nullopt` if the buffer is too short or
/// the frame would exceed `max_payload`.
inline std::optional<ClientFrame>
parse_client_frame_inplace(uint8_t* buf, size_t buf_len, size_t max_payload) noexcept {
    if (buf_len < 2) return std::nullopt;

    ClientFrame f;
    f.opcode = buf[0] & 0x0F;
    bool masked = (buf[1] & 0x80) != 0;
    uint64_t plen = buf[1] & 0x7F;
    size_t cursor = 2;

    if (plen == 126) {
        if (buf_len < cursor + 2) return std::nullopt;
        plen = (static_cast<uint64_t>(buf[cursor]) << 8) | buf[cursor + 1];
        cursor += 2;
    } else if (plen == 127) {
        if (buf_len < cursor + 8) return std::nullopt;
        plen = 0;
        for (int i = 0; i < 8; ++i) {
            plen = (plen << 8) | buf[cursor + i];
        }
        cursor += 8;
    }
    if (plen > max_payload) return std::nullopt;

    uint8_t mask[4] = {};
    if (masked) {
        if (buf_len < cursor + 4) return std::nullopt;
        std::memcpy(mask, buf + cursor, 4);
        cursor += 4;
    }
    if (buf_len < cursor + plen) return std::nullopt;

    if (masked && plen > 0) {
        for (uint64_t i = 0; i < plen; ++i) {
            buf[cursor + i] ^= mask[i & 3];
        }
    }

    f.payload_len    = plen;
    f.total_consumed = cursor + plen;
    return f;
}

} // namespace bench::mock
