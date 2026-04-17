/// @file ws_server.hpp
/// Minimal RFC 6455 server-side framing used by the WS echo scenarios.
///
/// This is the inverse of `eph::codec::WsCodec` (which is the *client*
/// side: sends masked, receives unmasked). Here we accept masked client
/// frames and emit unmasked server frames — a subset that exactly
/// matches the bench client's expectations.
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

#include <poll.h>
#include <sys/socket.h>

#include <spdlog/spdlog.h>

#include "eph/net/detail/ws_handshake.hpp"  // ws_compute_accept
#include "eph/net/posix_io.hpp"

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

/// One decoded client→server frame.
struct Frame {
    Opcode              opcode;
    std::vector<uint8_t> payload;  ///< unmasked bytes, ready for re-framing
};

namespace detail {

/// Read the entire HTTP upgrade request into `out` until `\r\n\r\n`.
/// Caps at 64 KiB like the Python mock. Returns false on peer close,
/// timeout, or oversized request.
[[nodiscard]] inline bool
read_upgrade_request(int fd, std::string& out,
                     int timeout_ms = 2000) noexcept {
    out.clear();
    pollfd p{ .fd = fd, .events = POLLIN, .revents = 0 };
    while (out.find("\r\n\r\n") == std::string::npos) {
        int rv = ::poll(&p, 1, timeout_ms);
        if (rv <= 0) return false;  // timeout or error
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
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
        return std::string(req.substr(p, e - p));
    }
    return std::nullopt;
}

/// Full client-exact length decoding (1-byte | 2-byte | 8-byte).
[[nodiscard]] inline std::optional<uint64_t>
read_length(int fd, uint8_t raw_len) noexcept {
    if (raw_len < 126) return raw_len;
    if (raw_len == 126) {
        uint8_t b[2];
        if (!eph::net::posix::recv_exact(fd, b, 2)) return std::nullopt;
        return (static_cast<uint64_t>(b[0]) << 8) | b[1];
    }
    // raw_len == 127
    uint8_t b[8];
    if (!eph::net::posix::recv_exact(fd, b, 8)) return std::nullopt;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
    return v;
}

} // namespace detail

/// Perform the RFC 6455 server handshake on `fd`. On success the socket
/// is ready to exchange frames; on failure the caller should close it.
///
/// Returns true on 101 Switching Protocols + well-formed `Accept`,
/// false on malformed request, IO error, or timeout.
[[nodiscard]] inline bool
accept_handshake(int fd, int timeout_ms = 2000) noexcept {
    std::string req;
    if (!detail::read_upgrade_request(fd, req, timeout_ms)) {
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
    return eph::net::posix::send_all(fd, resp.data(), resp.size());
}

/// Read one client→server frame from `fd`. Returns nullopt on peer
/// close / IO error, or on a mandatory-mask violation (client frames
/// must be masked per RFC 6455 §5.3).
[[nodiscard]] inline std::optional<Frame>
decode_frame(int fd) noexcept {
    uint8_t hdr[2];
    if (!eph::net::posix::recv_exact(fd, hdr, 2)) return std::nullopt;
    const uint8_t opcode_raw = hdr[0] & 0x0F;
    const bool    masked     = (hdr[1] & 0x80) != 0;
    const uint8_t raw_len    = hdr[1] & 0x7F;
    auto len_opt = detail::read_length(fd, raw_len);
    if (!len_opt) return std::nullopt;
    const uint64_t length = *len_opt;

    if (!masked) {
        SPDLOG_WARN("[ws_server] client frame missing mask bit");
        return std::nullopt;
    }

    uint8_t mask[4];
    if (!eph::net::posix::recv_exact(fd, mask, 4)) return std::nullopt;

    Frame f;
    f.opcode = static_cast<Opcode>(opcode_raw);
    if (length == 0) return f;
    f.payload.resize(length);
    if (!eph::net::posix::recv_exact(fd, f.payload.data(), length))
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

/// Convenience: encode + send_all one server frame.
[[nodiscard]] inline bool
send_frame(int fd, Opcode opcode, std::span<const uint8_t> payload) noexcept {
    // Frame header is at most 10 bytes. One-shot buffer to keep the
    // send atomic (a split send would be visible as two TCP segments,
    // which some clients tolerate but is surprising for a bench).
    std::vector<uint8_t> buf(payload.size() + 10);
    const size_t n = encode_frame(opcode, payload, buf.data());
    return eph::net::posix::send_all(fd, buf.data(), n);
}

} // namespace mockex::ws
