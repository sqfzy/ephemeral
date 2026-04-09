/// @file ws/client.hpp
/// Client-side WebSocket helper: TCP connect + HTTP Upgrade + masked
/// frame send + unmasked frame parse.
///
/// Plain WS (no TLS). The mock server speaks the same protocol via
/// `core/ws_handshake.hpp` + `core/ws_framing.hpp`.
///
/// We do NOT use eph-net::Transport here because that wraps WSS-over-TLS
/// with worker threads, which doesn't fit a controlled bench loop. The
/// plan explicitly allows raw client implementations when eph-net does
/// not fit cleanly.
#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace bench::ws {

namespace detail {

inline bool send_all(int fd, const void* data, size_t len) noexcept {
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

inline bool recv_some(int fd, void* buf, size_t want, size_t* got_out) noexcept {
    ssize_t n = ::recv(fd, buf, want, 0);
    if (n <= 0) {
        if (n < 0 && errno == EINTR) { *got_out = 0; return true; }
        return false;
    }
    *got_out = static_cast<size_t>(n);
    return true;
}

inline bool recv_exact(int fd, void* buf, size_t len) noexcept {
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, static_cast<uint8_t*>(buf) + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

} // namespace detail

/// Connect a plain TCP socket and perform the client-side WebSocket
/// upgrade handshake. Returns the connected fd on success.
[[nodiscard]] inline std::expected<int, std::string>
connect_ws(std::string_view ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::unexpected(std::string("socket: ") + std::strerror(errno));

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string ip_str(ip);
    if (::inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return std::unexpected("invalid server ip: " + ip_str);
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string e = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return std::unexpected(std::move(e));
    }

    // Send a minimal WS upgrade request. The mock validates the
    // Sec-WebSocket-Key field exists and computes the accept digest.
    // This key is fixed because the bench is single-shot — TLS / random
    // key generation are not required for a localhost mock.
    char host[64];
    std::snprintf(host, sizeof(host), "%s:%u", ip_str.c_str(), port);
    std::string req =
        std::string("GET / HTTP/1.1\r\nHost: ") + host +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!detail::send_all(fd, req.data(), req.size())) {
        ::close(fd);
        return std::unexpected("ws upgrade send failed");
    }

    // Read response until we see "\r\n\r\n".
    char resp[4096];
    size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (got < sizeof(resp)) {
        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (rem.count() <= 0) {
            ::close(fd);
            return std::unexpected("ws upgrade response timeout");
        }
        pollfd pfd{}; pfd.fd = fd; pfd.events = POLLIN;
        int rv = ::poll(&pfd, 1, static_cast<int>(rem.count()));
        if (rv <= 0) { ::close(fd); return std::unexpected("ws upgrade poll failed"); }
        ssize_t n = ::recv(fd, resp + got, sizeof(resp) - got, 0);
        if (n <= 0) { ::close(fd); return std::unexpected("ws upgrade recv failed"); }
        got += static_cast<size_t>(n);
        std::string_view v(resp, got);
        if (v.find("\r\n\r\n") != std::string_view::npos) break;
    }
    std::string_view v(resp, got);
    if (v.find("101") == std::string_view::npos) {
        ::close(fd);
        return std::unexpected("ws upgrade rejected (no 101 in response)");
    }
    return fd;
}

/// Read one server→client (unmasked) frame's payload into `out` (capacity `cap`).
/// Returns the payload length on success or 0 on failure / non-data frame.
/// Uses 2-3 recv() syscalls per frame; amortization across multiple frames
/// is not needed at the current default push rates (~1 kHz per symbol).
inline size_t recv_one_frame(int fd, uint8_t* out, size_t cap) noexcept {
    uint8_t hdr[2];
    if (!detail::recv_exact(fd, hdr, 2)) return 0;
    uint8_t opcode = hdr[0] & 0x0F;
    if ((hdr[1] & 0x80) != 0) return 0; // server frames must NOT be masked
    uint64_t plen = hdr[1] & 0x7F;
    if (plen == 126) {
        uint8_t ext[2];
        if (!detail::recv_exact(fd, ext, 2)) return 0;
        plen = (uint64_t(ext[0]) << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        if (!detail::recv_exact(fd, ext, 8)) return 0;
        plen = 0;
        for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
    }
    if (plen > cap) return 0;
    if (plen > 0 && !detail::recv_exact(fd, out, plen)) return 0;
    if (opcode != 0x01 && opcode != 0x02) return 0; // ignore control frames
    return static_cast<size_t>(plen);
}

} // namespace bench::ws
