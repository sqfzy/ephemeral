/// @file ws_server.hpp
/// Minimal RFC 6455 server-side framing used by the WS echo scenarios.
///
/// This is the inverse of `eph::codec::WsCodec` (which is the *client*
/// side: sends masked, receives unmasked). Here we accept masked client
/// frames and emit unmasked server frames — a subset that exactly
/// matches the bench client's expectations.
///
/// All entry points are templated on an `IoStream` type that exposes
/// `read_exact(buf,n)` / `read_some(buf,n)` / `write_all(buf,n)`. The
/// two adapters used in mockex are `mockex::io::RawFdStream` (plain
/// TCP) and `mockex::tls::TlsConn` (TLS-wrapped TCP).
///
/// Zero allocations after construction: the caller owns a fixed-size
/// recv scratch buffer and the encoder writes into a caller-supplied
/// output buffer. No fragmentation, no permessage-deflate, no
/// continuation frames — the bench client never sends them and a
/// compliant server that receives one can reject cleanly.
#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/net/detail/ws_handshake.hpp"  // ws_compute_accept

namespace mockex::ws {

/// RFC 6455 opcode values — only the four we ever encounter.
enum Opcode : uint8_t {
    kOpcodeContinuation = 0x0,
    kOpcodeText         = 0x1,
    kOpcodeBinary       = 0x2,
    kOpcodeClose        = 0x8,
    kOpcodePing         = 0x9,
    kOpcodePong         = 0xA,
};

/// Hard cap on a single decoded client frame payload. RFC 6455 §5.2
/// permits up to 2^63-1 bytes; we cap at 16 MiB so a malformed or
/// hostile client claiming a huge length cannot OOM/terminate the
/// bench mock via the noexcept `payload.resize(length)` below. The
/// bench protocol's largest legitimate frame (`ex_market` /
/// `ex_order`) is well under 64 KiB; 16 MiB matches `eph::codec::
/// LengthPrefixCodec::kMaxFrameLen` for symmetry.
inline constexpr std::uint64_t kMaxClientFramePayload =
    16u * 1024u * 1024u;

/// One decoded client→server frame.
struct Frame {
    Opcode              opcode;
    std::vector<uint8_t> payload;  ///< unmasked bytes, ready for re-framing
};

namespace detail {

/// Read the entire HTTP upgrade request into `out` until `\r\n\r\n`.
/// Caps at 64 KiB. Blocks until pattern found or stream ends.
///
/// The pre-TLS implementation used `::poll(fd, …, timeout_ms)` for a
/// 2-second deadline. The poll path doesn't translate to TLS (where
/// `::poll` on the underlying fd misses SSL-buffered bytes). For the
/// trusted bench environment we drop the timeout — mockex SIGTERM
/// cleans up any wedged conn.
template <class IoStream>
[[nodiscard]] inline bool
read_upgrade_request(IoStream& io, std::string& out) noexcept {
    out.clear();
    char buf[4096];
    while (out.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = io.read_some(buf, sizeof(buf));
        if (n <= 0) return false;
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 65536) return false;
    }
    return true;
}

/// Extract `Sec-WebSocket-Key` value from a raw request string. Case-
/// insensitive on the header name (RFC 7230).
[[nodiscard]] inline std::optional<std::string>
extract_sec_ws_key(std::string_view req) noexcept {
    constexpr std::string_view needle = "sec-websocket-key:";
    // Lower-case copy is expensive; do a manual case-insensitive search.
    for (size_t i = 0; i + needle.size() <= req.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            const char a = req[i + j];
            const char b = needle[j];
            const char la = (a >= 'A' && a <= 'Z')
                ? static_cast<char>(a + ('a' - 'A')) : a;
            if (la != b) { match = false; break; }
        }
        if (!match) continue;
        size_t p = i + needle.size();
        while (p < req.size() && (req[p] == ' ' || req[p] == '\t')) ++p;
        size_t e = req.find("\r\n", p);
        if (e == std::string_view::npos) return std::nullopt;
        // Strip trailing OWS (RFC 7230 §3.2.4: header values may have
        // whitespace before AND after the value). The eph WS client
        // never emits trailing OWS, but a non-conforming peer that
        // does would otherwise produce a key like "abc== " whose
        // SHA1 differs from "abc==", failing handshake-accept on
        // both sides for an opaque reason.
        size_t end = e;
        while (end > p && (req[end - 1] == ' ' || req[end - 1] == '\t')) --end;
        return std::string(req.substr(p, end - p));
    }
    return std::nullopt;
}

/// Full client-exact length decoding (1-byte | 2-byte | 8-byte).
template <class IoStream>
[[nodiscard]] inline std::optional<uint64_t>
read_length(IoStream& io, uint8_t raw_len) noexcept {
    if (raw_len < 126) return raw_len;
    if (raw_len == 126) {
        uint8_t b[2];
        if (!io.read_exact(b, 2)) return std::nullopt;
        return (static_cast<uint64_t>(b[0]) << 8) | b[1];
    }
    // raw_len == 127
    uint8_t b[8];
    if (!io.read_exact(b, 8)) return std::nullopt;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
    return v;
}

} // namespace detail

/// Perform the RFC 6455 server handshake on `io`. On success the stream
/// is ready to exchange frames; on failure the caller should drop it.
///
/// Returns true on 101 Switching Protocols + well-formed `Accept`,
/// false on malformed request or IO error.
template <class IoStream>
[[nodiscard]] inline bool
accept_handshake(IoStream& io) noexcept {
    std::string req;
    if (!detail::read_upgrade_request(io, req)) {
        SPDLOG_WARN("[ws_server] handshake read failed");
        return false;
    }
    auto key = detail::extract_sec_ws_key(req);
    if (!key) {
        SPDLOG_WARN("[ws_server] missing Sec-WebSocket-Key");
        return false;
    }
    const std::string accept = eph::net::detail::ws_compute_accept(*key);
    std::string resp;
    resp.reserve(160);
    resp.append("HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: ");
    resp.append(accept);
    resp.append("\r\n\r\n");
    return io.write_all(resp.data(), resp.size());
}

/// Read one client→server frame from `io`. Returns nullopt on peer
/// close / IO error, or on a mandatory-mask violation (client frames
/// must be masked per RFC 6455 §5.3).
template <class IoStream>
[[nodiscard]] inline std::optional<Frame>
decode_frame(IoStream& io) noexcept {
    uint8_t hdr[2];
    if (!io.read_exact(hdr, 2)) return std::nullopt;
    const uint8_t opcode_raw = hdr[0] & 0x0F;
    const bool    masked     = (hdr[1] & 0x80) != 0;
    const uint8_t raw_len    = hdr[1] & 0x7F;
    auto len_opt = detail::read_length(io, raw_len);
    if (!len_opt) return std::nullopt;
    const uint64_t length = *len_opt;

    if (!masked) [[unlikely]] {
        SPDLOG_WARN("[ws_server] client frame missing mask bit");
        return std::nullopt;
    }

    // RFC 6455 §5.5: control frames (opcode high bit set) MUST have
    // payload ≤ 125 bytes and MUST NOT be fragmented (we don't track
    // FIN here, but length cap alone shuts the door on the OOM vector).
    // Without this, a malicious client could send a Ping with raw_len=127
    // claiming 8 EiB and force the mock to attempt a multi-MiB
    // `vector::resize` inside a noexcept function. The bench's own
    // client never sends control frames > 0 B, so this is a pure
    // hardening cap with zero functional cost.
    const bool is_control = (opcode_raw & 0x08) != 0;
    if (is_control && length > 125) [[unlikely]] {
        SPDLOG_WARN(
            "[ws_server] control frame opcode=0x{:x} payload={} "
            "exceeds RFC 6455 cap of 125; dropping",
            static_cast<unsigned>(opcode_raw), length);
        return std::nullopt;
    }

    // Reject implausibly large payload claims before allocating.
    // `decode_frame` is `noexcept`; an unguarded `vector::resize` of
    // a 64-bit untrusted length would throw `bad_alloc` and call
    // `std::terminate`, i.e. one malformed frame crashes the mock.
    if (length > kMaxClientFramePayload) [[unlikely]] {
        SPDLOG_WARN(
            "[ws_server] client frame payload length {} exceeds "
            "kMaxClientFramePayload={}; dropping connection "
            "(opcode=0x{:x}, raw_len={})",
            length, kMaxClientFramePayload,
            static_cast<unsigned>(opcode_raw),
            static_cast<unsigned>(raw_len));
        return std::nullopt;
    }

    uint8_t mask[4];
    if (!io.read_exact(mask, 4)) return std::nullopt;

    Frame f;
    f.opcode = static_cast<Opcode>(opcode_raw);
    if (length == 0) return f;
    f.payload.resize(static_cast<size_t>(length));
    if (!io.read_exact(f.payload.data(), length))
        return std::nullopt;

    // Unmask in-place. Loop is cheap compared to the syscall we just did.
    for (size_t i = 0; i < length; ++i) {
        f.payload[i] ^= mask[i & 3];
    }
    return f;
}

/// Encode a server→client frame into `out`. FIN=1, no mask. Returns
/// the total bytes written. `out` must have at least
/// `payload.size() + 10` bytes of capacity.
inline size_t
encode_frame(Opcode opcode, std::span<const uint8_t> payload,
             uint8_t* out) noexcept {
    size_t o = 0;
    out[o++] = static_cast<uint8_t>(0x80 | (opcode & 0x0F));
    const size_t n = payload.size();
    if (n < 126) {
        out[o++] = static_cast<uint8_t>(n);
    } else if (n < (1u << 16)) {
        out[o++] = 126;
        out[o++] = static_cast<uint8_t>((n >> 8) & 0xFF);
        out[o++] = static_cast<uint8_t>(n & 0xFF);
    } else {
        out[o++] = 127;
        for (int i = 7; i >= 0; --i) {
            out[o++] = static_cast<uint8_t>((n >> (i * 8)) & 0xFF);
        }
    }
    std::memcpy(out + o, payload.data(), n);
    return o + n;
}

/// Convenience: encode + write_all one server frame.
template <class IoStream>
[[nodiscard]] inline bool
send_frame(IoStream& io, Opcode opcode,
           std::span<const uint8_t> payload) noexcept {
    // Frame header is at most 10 bytes. One-shot buffer to keep the
    // send atomic (a split send would be visible as two TCP segments,
    // which some clients tolerate but is surprising for a bench).
    std::vector<uint8_t> buf(payload.size() + 10);
    const size_t n = encode_frame(opcode, payload, buf.data());
    return io.write_all(buf.data(), n);
}

} // namespace mockex::ws
