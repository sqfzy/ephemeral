/// @file eph-net-dpdk/tests/integration/ws_framing.hpp
/// Minimal WebSocket frame helpers for the DPDK E2E kernel-side echo mock.
///
/// Previously lived at `benchmarks/latency/core/ws_framing.hpp` — moved
/// here in Phase 10 when the bench latency suite was rewritten to rely
/// on Python mocks instead of C++ in-process mocks. `test_dpdk_e2e.cpp`
/// still needs a tiny in-process WS server (no framework dependency)
/// to validate the DpdkTcpStream WS upgrade path, so the helper moves
/// next to its only remaining caller.
///
/// Scope: server-side echo only. Client side (`build_masked_text_frame`
/// / `parse_server_frame`) is retained for symmetry and because it is
/// trivially small.
///
///   client side (bench → mock):
///     - `build_masked_text_frame`  — client → server
///     - `parse_server_frame`       — parse unmasked server frame
///
///   server side (mock → bench):
///     - `build_server_frame`       — server → client (unmasked)
///     - `parse_client_frame_inplace` — parse masked client frame,
///                                      unmask payload in place
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

namespace bench::ws_framing {

// ── Opcodes (RFC 6455 §5.2) ──────────────────────────────────────────
inline constexpr uint8_t kOpCont   = 0x00;
inline constexpr uint8_t kOpText   = 0x01;
inline constexpr uint8_t kOpBinary = 0x02;
inline constexpr uint8_t kOpClose  = 0x08;
inline constexpr uint8_t kOpPing   = 0x09;
inline constexpr uint8_t kOpPong   = 0x0A;

// ── Client side: bench binary → mock ─────────────────────────────────

/// Build a masked client→server text frame into `out` (capacity ≥ len+14).
/// `seed` is the 4-byte mask key — any non-zero rotation is fine, the
/// bench is not adversarial. Returns total bytes written.
inline size_t build_masked_text_frame(uint8_t* out, const void* payload,
                                      size_t len, uint32_t seed) noexcept {
    out[0] = 0x80 | kOpText;
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
/// buffer is too short or the frame claims to be masked (protocol error).
/// The caller advances its read position by `header_size + payload_len`.
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

// ── Server side: mock → bench binary ─────────────────────────────────

/// Build an unmasked server→client frame into `out` (capacity ≥ len+10).
/// Returns total bytes written.
inline size_t build_server_frame(uint8_t* out, uint8_t opcode,
                                 const uint8_t* payload, size_t len) noexcept {
    out[0] = static_cast<uint8_t>(0x80 | opcode);
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
        for (int i = 0; i < 8; ++i)
            out[2 + i] = static_cast<uint8_t>((len >> (56 - 8 * i)) & 0xFF);
        hdr = 10;
    }
    if (len > 0) std::memcpy(out + hdr, payload, len);
    return hdr + len;
}

/// Result of `parse_client_frame_inplace`: opcode, unmasked payload length,
/// and the total number of bytes consumed from the input buffer.
struct ClientFrame {
    uint8_t  opcode = 0;
    uint64_t payload_len = 0;
    size_t   total_consumed = 0; ///< header + mask + payload bytes consumed
};

/// Parse a client→server frame from `buf[0..buf_len)`. Unmasks the payload
/// in place. Returns `nullopt` if the buffer is too short, the frame would
/// exceed `max_payload`, or the frame is malformed.
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

} // namespace bench::ws_framing
