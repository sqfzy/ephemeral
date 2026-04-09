/// @file ws/client.hpp
/// Client-side WebSocket helper: TCP connect + HTTP Upgrade + masked
/// frame send + unmasked frame parse.
///
/// Plain WS (no TLS). The mock server speaks the same protocol via
/// `mock/lib/ws_handshake.hpp` + `mock/lib/ws_frame.hpp`.
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

/// Build a masked client→server text frame into `out`. Returns total bytes
/// written. `out` must hold at least 14 + len bytes (worst-case header).
inline size_t build_masked_text_frame(uint8_t* out, const void* payload, size_t len,
                                      uint32_t mask_seed) noexcept {
    out[0] = 0x81; // FIN=1 + opcode=text
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
        for (int i = 0; i < 8; ++i) {
            out[2 + i] = static_cast<uint8_t>((len >> (56 - 8 * i)) & 0xFF);
        }
        cursor = 10;
    }

    // Mask key (4 bytes). A fixed seed per call is fine — bench is not
    // adversarial.
    uint8_t mask[4];
    mask[0] = static_cast<uint8_t>(mask_seed);
    mask[1] = static_cast<uint8_t>(mask_seed >> 8);
    mask[2] = static_cast<uint8_t>(mask_seed >> 16);
    mask[3] = static_cast<uint8_t>(mask_seed >> 24);
    std::memcpy(out + cursor, mask, 4);
    cursor += 4;

    auto* src = static_cast<const uint8_t*>(payload);
    for (size_t i = 0; i < len; ++i) {
        out[cursor + i] = src[i] ^ mask[i & 3];
    }
    return cursor + len;
}

/// Buffered server→client WebSocket frame reader.
///
/// The naive "one recv() per header byte + one recv() per payload" pattern
/// adds 2-3 syscalls per frame. A real HFT client amortizes across hundreds
/// of frames by reading a large buffer in one syscall, then parsing frames
/// from that buffer. At 30k frames/sec (3 symbols × 10kHz bookTicker)
/// this drops syscall count from ~60k/sec to ~100/sec.
///
/// `next_frame()` returns a pointer INTO the internal buffer, valid until
/// the next call. Callers must consume or copy before calling again.
/// Returns {nullptr, 0} on socket error / disconnect.
class FrameReader {
public:
    explicit FrameReader(int fd) : fd_(fd) {}

    /// Blocking read: return the next complete data frame's payload.
    std::pair<const uint8_t*, size_t> next_frame() {
        for (;;) {
            if (auto f = try_parse(); f.first) return f;
            if (!fill()) return {nullptr, 0};
        }
    }

private:
    bool fill() {
        // Compact if we'd run off the end.
        if (end_ == sizeof(buf_)) {
            std::memmove(buf_, buf_ + pos_, end_ - pos_);
            end_ -= pos_;
            pos_ = 0;
        }
        ssize_t n = ::recv(fd_, buf_ + end_, sizeof(buf_) - end_, 0);
        if (n <= 0) return false;
        end_ += static_cast<size_t>(n);
        return true;
    }

    /// Try to parse one complete unmasked server frame at `buf_[pos_..end_]`.
    /// Returns {payload, len} and advances pos_ on success; {nullptr, 0}
    /// if the buffer holds only a partial frame.
    std::pair<const uint8_t*, size_t> try_parse() noexcept {
        const size_t avail = end_ - pos_;
        if (avail < 2) return {nullptr, 0};
        const uint8_t* h = buf_ + pos_;
        // Servers MUST NOT mask (RFC 6455); skip anything that claims to be masked.
        if ((h[1] & 0x80) != 0) return {nullptr, 0};
        uint64_t plen = h[1] & 0x7F;
        size_t hdr_len = 2;
        if (plen == 126) {
            if (avail < 4) return {nullptr, 0};
            plen = (uint64_t(h[2]) << 8) | h[3];
            hdr_len = 4;
        } else if (plen == 127) {
            if (avail < 10) return {nullptr, 0};
            plen = 0;
            for (int i = 0; i < 8; ++i) plen = (plen << 8) | h[2 + i];
            hdr_len = 10;
        }
        if (avail < hdr_len + plen) return {nullptr, 0};
        const uint8_t* payload = buf_ + pos_ + hdr_len;
        pos_ += hdr_len + static_cast<size_t>(plen);
        return {payload, static_cast<size_t>(plen)};
    }

    int fd_;
    uint8_t buf_[65536];
    size_t pos_ = 0;
    size_t end_ = 0;
};

/// Legacy: read one frame with 2-3 syscalls. Kept for the simple ws echo
/// scenario where frame rate is low (1 per RTT) so syscall overhead is
/// negligible. High-rate paths should use `FrameReader` instead.
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
