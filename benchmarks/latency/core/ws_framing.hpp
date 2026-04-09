/// @file core/ws_framing.hpp
/// Minimal client-side WebSocket frame helpers shared by every bench that
/// needs to talk to the mock WS server (kernel and DPDK alike).
///
/// The mock is always unmasked server → client; the bench client is always
/// masked client → server (RFC 6455). These two helpers are all any
/// scenario needs — framing anything more complex would double the
/// surface area for no measurement benefit.
#pragma once

#include <cstdint>
#include <cstring>
#include <utility>

namespace bench::ws_framing {

/// Build a masked client→server text frame into `out` (capacity ≥ len+14).
/// `seed` is the 4-byte mask key — any non-zero rotation is fine, the bench
/// is not adversarial. Returns total bytes written.
inline size_t build_masked_text_frame(uint8_t* out, const void* payload,
                                      size_t len, uint32_t seed) noexcept {
    out[0] = 0x81; // FIN=1, opcode=text
    size_t cursor;
    if (len < 126) {
        out[1] = static_cast<uint8_t>(0x80 | len);
        cursor = 2;
    } else if (len < 65536) {
        out[1] = static_cast<uint8_t>(0x80 | 126);
        out[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
        out[3] = static_cast<uint8_t>(len & 0xFF);
        cursor = 4;
    } else {
        out[1] = static_cast<uint8_t>(0x80 | 127);
        for (int i = 0; i < 8; ++i)
            out[2 + i] = static_cast<uint8_t>((len >> (56 - 8 * i)) & 0xFF);
        cursor = 10;
    }

    const uint8_t mask[4] = {
        static_cast<uint8_t>(seed),
        static_cast<uint8_t>(seed >> 8),
        static_cast<uint8_t>(seed >> 16),
        static_cast<uint8_t>(seed >> 24),
    };
    std::memcpy(out + cursor, mask, 4);
    cursor += 4;

    const auto* src = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < len; ++i) {
        out[cursor + i] = src[i] ^ mask[i & 3];
    }
    return cursor + len;
}

/// Parse a complete unmasked server→client frame out of a linear buffer.
/// Returns `{header_size, payload_len}` on success, or `{0, 0}` if the
/// buffer is too short. The caller advances its read position by
/// `header_size + payload_len` bytes.
inline std::pair<size_t, size_t>
parse_server_frame(const uint8_t* buf, size_t buf_len) noexcept {
    if (buf_len < 2) return {0, 0};
    if ((buf[1] & 0x80) != 0) return {0, 0}; // servers MUST NOT mask
    uint64_t plen = buf[1] & 0x7F;
    size_t hdr = 2;
    if (plen == 126) {
        if (buf_len < 4) return {0, 0};
        plen = (uint64_t(buf[2]) << 8) | buf[3];
        hdr = 4;
    } else if (plen == 127) {
        if (buf_len < 10) return {0, 0};
        plen = 0;
        for (int i = 0; i < 8; ++i) plen = (plen << 8) | buf[2 + i];
        hdr = 10;
    }
    if (buf_len < hdr + plen) return {0, 0};
    return {hdr, static_cast<size_t>(plen)};
}

} // namespace bench::ws_framing
