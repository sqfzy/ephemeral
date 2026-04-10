/// @file core/ws_handshake.hpp
/// Server-side WebSocket HTTP Upgrade handshake.
///
/// Reads the HTTP request, extracts `Sec-WebSocket-Key`, computes the
/// `Sec-WebSocket-Accept` digest (SHA-1 of key + RFC magic string, base64
/// encoded), and writes the 101 response.
///
/// Bundles a minimal SHA-1 implementation (80 lines) because the bench
/// mock has no link dependency on aws-lc/openssl. Base64 reuses
/// `eph::core::detail::base64_encode` (standalone, no TLS deps).
///
/// Blocking reads with a millisecond-level deadline. Suitable for the
/// mock's bootstrap path; not suitable for the hot loop.
#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "eph/core/detail/base64.hpp"

namespace bench {

namespace handshake_detail {

// ── SHA-1 (RFC 3174) ─────────────────────────────────────────────────────
struct Sha1 {
    uint32_t h[5];
    uint64_t length;
    uint8_t  buffer[64];
    size_t   buffer_used;

    static constexpr uint32_t rotl(uint32_t x, int n) {
        return (x << n) | (x >> (32 - n));
    }

    void init() {
        h[0] = 0x67452301; h[1] = 0xEFCDAB89; h[2] = 0x98BADCFE;
        h[3] = 0x10325476; h[4] = 0xC3D2E1F0;
        length = 0; buffer_used = 0;
    }

    void process_block(const uint8_t* block) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(block[i*4]) << 24) | (uint32_t(block[i*4+1]) << 16) |
                   (uint32_t(block[i*4+2]) << 8) | uint32_t(block[i*4+3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;            k = 0xCA62C1D6; }
            uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const uint8_t* data, size_t len) {
        length += len * 8;
        while (len > 0) {
            size_t take = std::min<size_t>(len, 64 - buffer_used);
            std::memcpy(buffer + buffer_used, data, take);
            buffer_used += take; data += take; len -= take;
            if (buffer_used == 64) { process_block(buffer); buffer_used = 0; }
        }
    }

    void finalize(uint8_t out[20]) {
        // RFC 3174: encode the ORIGINAL message length (in bits) at the
        // tail of the padded buffer.  Save it BEFORE any padding bytes
        // get fed through update() — update() unconditionally bumps
        // `length` by 8 bits per byte, so by the time we finish padding
        // `length` already counts the 0x80 byte plus all the zero
        // bytes, not just the original input.  The previous version
        // tried to subtract just the pad byte (`length - 8`) which
        // missed the zero-padding contribution and produced wrong
        // hashes for any input not aligned to 56 mod 64.
        uint64_t len_bits = length;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buffer_used != 56) update(&zero, 1);
        for (int i = 7; i >= 0; --i) {
            uint8_t b = static_cast<uint8_t>((len_bits >> (i * 8)) & 0xFF);
            update(&b, 1);
        }
        for (int i = 0; i < 5; ++i) {
            out[i*4]   = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
            out[i*4+1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
            out[i*4+2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
            out[i*4+3] = static_cast<uint8_t>(h[i] & 0xFF);
        }
    }
};

inline std::array<uint8_t, 20> sha1(std::string_view input) {
    Sha1 ctx; ctx.init();
    ctx.update(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    std::array<uint8_t, 20> digest;
    ctx.finalize(digest.data());
    return digest;
}

inline std::string compute_accept(std::string_view key) {
    std::string concat(key);
    concat += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    auto digest = sha1(concat);
    return eph::core::detail::base64_encode(digest.data(), 20);
}

inline bool read_with_deadline(int fd, void* out, size_t want,
                               size_t* got, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    *got = 0;
    while (*got < want) {
        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (rem.count() <= 0) return false;
        pollfd p{}; p.fd = fd; p.events = POLLIN;
        int rv = ::poll(&p, 1, static_cast<int>(rem.count()));
        if (rv <= 0) return false;
        ssize_t n = ::recv(fd, static_cast<uint8_t*>(out) + *got, want - *got, 0);
        if (n <= 0) return *got > 0;
        *got += static_cast<size_t>(n);
    }
    return true;
}

inline bool send_all(int fd, const void* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, static_cast<const uint8_t*>(data) + sent,
                           len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

} // namespace handshake_detail

/// Perform a server-side WebSocket Upgrade handshake on `fd`. Reads the
/// HTTP request, computes Sec-WebSocket-Accept, and writes the 101
/// response. Returns an error string on parse / I/O failure.
[[nodiscard]] inline std::expected<void, std::string>
ws_server_handshake(int fd, std::chrono::milliseconds timeout =
                    std::chrono::milliseconds(2000)) {
    using namespace handshake_detail;

    char req[4096];
    size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string_view marker{"\r\n\r\n"};

    while (got < sizeof(req)) {
        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (rem.count() <= 0) return std::unexpected("ws_handshake: read timeout");
        pollfd p{}; p.fd = fd; p.events = POLLIN;
        int rv = ::poll(&p, 1, static_cast<int>(rem.count()));
        if (rv <= 0) return std::unexpected("ws_handshake: poll failed");
        ssize_t n = ::recv(fd, req + got, sizeof(req) - got, 0);
        if (n <= 0) return std::unexpected("ws_handshake: recv failed");
        got += static_cast<size_t>(n);
        std::string_view view(req, got);
        if (view.find(marker) != std::string_view::npos) break;
    }
    if (got == 0) return std::unexpected("ws_handshake: empty request");

    std::string_view view(req, got);
    auto kpos = view.find("Sec-WebSocket-Key:");
    if (kpos == std::string_view::npos) {
        return std::unexpected("ws_handshake: missing Sec-WebSocket-Key");
    }
    kpos += std::string_view{"Sec-WebSocket-Key:"}.size();
    while (kpos < view.size() && (view[kpos] == ' ' || view[kpos] == '\t')) ++kpos;
    auto kend = view.find("\r\n", kpos);
    if (kend == std::string_view::npos) {
        return std::unexpected("ws_handshake: malformed key line");
    }
    std::string_view key = view.substr(kpos, kend - kpos);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
        key.remove_suffix(1);
    }

    std::string accept = compute_accept(key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    if (!send_all(fd, resp.data(), resp.size())) {
        return std::unexpected("ws_handshake: send failed");
    }
    return {};
}

} // namespace bench
