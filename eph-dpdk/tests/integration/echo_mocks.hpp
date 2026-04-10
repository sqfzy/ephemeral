#pragma once

/// @file echo_mocks.hpp
/// Simple kernel-side echo servers for the DPDK E2E test suite.
///
/// These are intentionally separate from `benchmarks/latency/core/`'s
/// mock_fn::run, which has bench-specific framing (4-byte msg_size header
/// + TSC stamps in bytes 8-16).  Here we want plain echo with no protocol
/// coupling so that the DPDK client can be a vanilla TcpSession / UdpSender
/// driving raw bytes.
///
/// Each mock function:
///   * binds to (cfg.server_ip, port) on NIC_A in the host network namespace
///   * polls the listen fd with a 100ms timeout to remain responsive to
///     `running` flips even when no client is connecting
///   * is called from a thread inside the dispatcher's child process

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace eph::dpdk::test_e2e {

/// Poll a file descriptor for readability with a millisecond timeout.
/// Returns 1 if readable, 0 on timeout, -1 on error.  EINTR is auto-retried.
inline int poll_readable(int fd, int timeout_ms) noexcept {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int rc;
    do { rc = ::poll(&pfd, 1, timeout_ms); }
    while (rc < 0 && errno == EINTR);
    return rc;
}

/// Bind a TCP listening socket to (ip, port) with SO_REUSEADDR + TCP_NODELAY.
/// Returns the fd, or -1 on error (logs the failure).
inline int tcp_bind_listen(const std::string& ip, uint16_t port) noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        spdlog::error("test_e2e mock: socket() failed: {}", ::strerror(errno));
        return -1;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        spdlog::error("test_e2e mock: inet_pton({}) failed", ip);
        ::close(fd);
        return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        spdlog::error("test_e2e mock: bind({}:{}) failed: {}",
                      ip, port, ::strerror(errno));
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 16) != 0) {
        spdlog::error("test_e2e mock: listen failed: {}", ::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Bind a UDP socket to (ip, port) with SO_REUSEADDR.
inline int udp_bind(const std::string& ip, uint16_t port) noexcept {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        spdlog::error("test_e2e mock: udp socket() failed: {}", ::strerror(errno));
        return -1;
    }
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        spdlog::error("test_e2e mock: udp bind({}:{}) failed: {}",
                      ip, port, ::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

/// recv exactly `n` bytes from `fd` into `buf`.  Returns false on EOF or error.
inline bool recv_exact(int fd, void* buf, size_t n) noexcept {
    size_t got = 0;
    auto* p = static_cast<uint8_t*>(buf);
    while (got < n) {
        ssize_t r = ::recv(fd, p + got, n - got, 0);
        if (r == 0) return false;  // EOF
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

/// send exactly `n` bytes from `buf` to `fd`.
inline bool send_exact(int fd, const void* buf, size_t n) noexcept {
    size_t sent = 0;
    auto* p = static_cast<const uint8_t*>(buf);
    while (sent < n) {
        ssize_t r = ::send(fd, p + sent, n - sent, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(r);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// TCP echo mock — generic byte-stream echo, no framing.
//
// Reads up to 16 KiB chunks and echoes them back verbatim until the peer
// closes the connection.  The accept loop polls the listen fd with a
// 100 ms timeout so the thread checks `running` ten times per second
// even when idle, and shuts down promptly on dispatcher SIGTERM.
// ─────────────────────────────────────────────────────────────────────────
inline void tcp_echo_mock_thread(const std::string& ip, uint16_t port,
                                  std::atomic<bool>& running) noexcept {
    int listen_fd = tcp_bind_listen(ip, port);
    if (listen_fd < 0) return;
    spdlog::info("test_e2e tcp_echo_mock listening on {}:{}", ip, port);

    constexpr size_t kBufSize = 16384;
    std::vector<uint8_t> buf(kBufSize);

    while (running.load(std::memory_order_acquire)) {
        int pr = poll_readable(listen_fd, 100);
        if (pr <= 0) continue;
        int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR || errno == EBADF) break;
            spdlog::warn("test_e2e tcp_echo accept: {}", ::strerror(errno));
            continue;
        }
        int nodelay = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // Echo loop — also polls so we exit promptly on shutdown.
        while (running.load(std::memory_order_acquire)) {
            int rr = poll_readable(cfd, 100);
            if (rr == 0) continue;
            if (rr < 0) break;
            ssize_t n = ::recv(cfd, buf.data(), kBufSize, 0);
            if (n <= 0) break;
            if (!send_exact(cfd, buf.data(), static_cast<size_t>(n))) break;
        }
        ::close(cfd);
    }
    ::close(listen_fd);
    spdlog::info("test_e2e tcp_echo_mock @ :{} exiting", port);
}

// ─────────────────────────────────────────────────────────────────────────
// UDP echo mock — recvfrom + sendto in a poll loop.
// ─────────────────────────────────────────────────────────────────────────
inline void udp_echo_mock_thread(const std::string& ip, uint16_t port,
                                  std::atomic<bool>& running) noexcept {
    int fd = udp_bind(ip, port);
    if (fd < 0) return;
    spdlog::info("test_e2e udp_echo_mock listening on {}:{}", ip, port);

    constexpr size_t kBufSize = 2048;
    std::vector<uint8_t> buf(kBufSize);

    while (running.load(std::memory_order_acquire)) {
        int pr = poll_readable(fd, 100);
        if (pr <= 0) continue;
        sockaddr_in src{};
        socklen_t srclen = sizeof(src);
        ssize_t n = ::recvfrom(fd, buf.data(), kBufSize, 0,
                                reinterpret_cast<sockaddr*>(&src), &srclen);
        if (n < 0) {
            if (errno == EINTR || errno == EBADF) break;
            spdlog::warn("test_e2e udp_echo recvfrom: {}", ::strerror(errno));
            continue;
        }
        ::sendto(fd, buf.data(), static_cast<size_t>(n), MSG_NOSIGNAL,
                 reinterpret_cast<sockaddr*>(&src), srclen);
    }
    ::close(fd);
    spdlog::info("test_e2e udp_echo_mock @ :{} exiting", port);
}

// ─────────────────────────────────────────────────────────────────────────
// RST mock — accept then immediately abort with RST via SO_LINGER {1, 0}.
// Used by FailureE2E.PeerRstAfterAccept.
// ─────────────────────────────────────────────────────────────────────────
inline void tcp_rst_mock_thread(const std::string& ip, uint16_t port,
                                 std::atomic<bool>& running) noexcept {
    int listen_fd = tcp_bind_listen(ip, port);
    if (listen_fd < 0) return;
    spdlog::info("test_e2e tcp_rst_mock listening on {}:{}", ip, port);

    while (running.load(std::memory_order_acquire)) {
        int pr = poll_readable(listen_fd, 100);
        if (pr <= 0) continue;
        int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR || errno == EBADF) break;
            continue;
        }
        // SO_LINGER {on=1, linger=0} → close() drops the socket and
        // sends RST instead of going through FIN/ACK.
        struct linger lg{1, 0};
        ::setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        ::close(cfd);
    }
    ::close(listen_fd);
    spdlog::info("test_e2e tcp_rst_mock @ :{} exiting", port);
}

// ─────────────────────────────────────────────────────────────────────────
// FIN mock — accept, echo one chunk, then shutdown(WR) so the client
// observes a graceful FIN from the peer side.
// Used by FailureE2E.PeerFinAfterEcho.
// ─────────────────────────────────────────────────────────────────────────
inline void tcp_fin_mock_thread(const std::string& ip, uint16_t port,
                                 std::atomic<bool>& running) noexcept {
    int listen_fd = tcp_bind_listen(ip, port);
    if (listen_fd < 0) return;
    spdlog::info("test_e2e tcp_fin_mock listening on {}:{}", ip, port);

    constexpr size_t kBufSize = 4096;
    std::vector<uint8_t> buf(kBufSize);

    while (running.load(std::memory_order_acquire)) {
        int pr = poll_readable(listen_fd, 100);
        if (pr <= 0) continue;
        int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR || errno == EBADF) break;
            continue;
        }
        // Wait briefly for the client's first chunk.
        if (poll_readable(cfd, 1000) > 0) {
            ssize_t n = ::recv(cfd, buf.data(), kBufSize, 0);
            if (n > 0) {
                (void)send_exact(cfd, buf.data(), static_cast<size_t>(n));
            }
        }
        ::shutdown(cfd, SHUT_WR);
        // Drain anything the client sends after our FIN, then close fully.
        while (running.load(std::memory_order_acquire)) {
            int rr = poll_readable(cfd, 100);
            if (rr == 0) continue;
            if (rr < 0) break;
            ssize_t d = ::recv(cfd, buf.data(), kBufSize, 0);
            if (d <= 0) break;
        }
        ::close(cfd);
    }
    ::close(listen_fd);
    spdlog::info("test_e2e tcp_fin_mock @ :{} exiting", port);
}

// ─────────────────────────────────────────────────────────────────────────
// WebSocket echo mock — minimal RFC 6455 server.
//
// Speaks just enough to pass the eph-transport WsFramer client through:
//   1. Read HTTP/1.1 GET request, find Sec-WebSocket-Key
//   2. Compute SHA-1(key + GUID), base64 it, send 101 Switching Protocols
//   3. Loop: read masked client frame, unmask payload, echo unmasked
//      with FIN=1, opcode=binary
//
// We deliberately reimplement framing here (rather than reuse
// benchmarks/latency/core/ws_framing.hpp) because that header has
// bench-specific TSC stamping baked into the payload protocol.
// ─────────────────────────────────────────────────────────────────────────

namespace ws_detail {

/// Base64 encode (no newlines, padded).  Small inline impl to avoid pulling
/// in aws-lc into the test mock.
inline std::string b64_encode(const uint8_t* data, size_t len) {
    static constexpr char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len) v |= uint32_t(data[i + 2]);
        out.push_back(kAlpha[(v >> 18) & 0x3F]);
        out.push_back(kAlpha[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? kAlpha[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? kAlpha[v & 0x3F]        : '=');
    }
    return out;
}

/// Minimal SHA-1 — RFC 3174 reference impl.  Used so the mock has zero
/// dependency on OpenSSL/aws-lc.  Not constant-time but that's fine here.
inline void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };

    std::vector<uint8_t> buf(data, data + len);
    uint64_t bits = static_cast<uint64_t>(len) * 8;
    buf.push_back(0x80);
    while (buf.size() % 64 != 56) buf.push_back(0);
    for (int i = 7; i >= 0; --i) buf.push_back(static_cast<uint8_t>(bits >> (i * 8)));

    for (size_t off = 0; off < buf.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(buf[off + i*4 + 0]) << 24)
                 | (uint32_t(buf[off + i*4 + 1]) << 16)
                 | (uint32_t(buf[off + i*4 + 2]) <<  8)
                 |  uint32_t(buf[off + i*4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;            k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;            k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; ++i) {
        out[i*4 + 0] = static_cast<uint8_t>(h[i] >> 24);
        out[i*4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i*4 + 2] = static_cast<uint8_t>(h[i] >>  8);
        out[i*4 + 3] = static_cast<uint8_t>(h[i] >>  0);
    }
}

/// Find a header value in a raw HTTP request (case-insensitive name).
inline std::string find_header(const std::string& req, const std::string& name) {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
    std::string lreq;
    lreq.reserve(req.size());
    for (char c : req) lreq.push_back(lower(c));
    std::string lname;
    lname.reserve(name.size());
    for (char c : name) lname.push_back(lower(c));
    auto pos = lreq.find(lname + ":");
    if (pos == std::string::npos) return {};
    pos += lname.size() + 1;
    while (pos < req.size() && (req[pos] == ' ' || req[pos] == '\t')) ++pos;
    auto eol = req.find("\r\n", pos);
    if (eol == std::string::npos) return {};
    return req.substr(pos, eol - pos);
}

} // namespace ws_detail

inline void ws_echo_mock_thread(const std::string& ip, uint16_t port,
                                 std::atomic<bool>& running) noexcept {
    int listen_fd = tcp_bind_listen(ip, port);
    if (listen_fd < 0) return;
    spdlog::info("test_e2e ws_echo_mock listening on {}:{}", ip, port);

    constexpr size_t kBufSize = 65536;
    std::vector<uint8_t> buf(kBufSize);

    while (running.load(std::memory_order_acquire)) {
        int pr = poll_readable(listen_fd, 100);
        if (pr <= 0) continue;
        int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR || errno == EBADF) break;
            continue;
        }
        int nodelay = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // ── Phase 1: read the HTTP upgrade request ────────────────────
        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
            if (poll_readable(cfd, 2000) <= 0) { req.clear(); break; }
            ssize_t n = ::recv(cfd, buf.data(), kBufSize, 0);
            if (n <= 0) { req.clear(); break; }
            req.append(reinterpret_cast<char*>(buf.data()), static_cast<size_t>(n));
        }
        if (req.empty()) { ::close(cfd); continue; }

        std::string key = ws_detail::find_header(req, "Sec-WebSocket-Key");
        if (key.empty()) {
            spdlog::warn("test_e2e ws_echo: missing Sec-WebSocket-Key");
            ::close(cfd);
            continue;
        }

        // ── Phase 2: compute accept hash and send 101 ─────────────────
        constexpr const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string concat = key + kGuid;
        uint8_t hash[20];
        ws_detail::sha1(reinterpret_cast<const uint8_t*>(concat.data()),
                        concat.size(), hash);
        std::string accept_b64 = ws_detail::b64_encode(hash, 20);

        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept_b64 + "\r\n\r\n";
        if (!send_exact(cfd, resp.data(), resp.size())) {
            ::close(cfd);
            continue;
        }

        // ── Phase 3: frame echo loop ──────────────────────────────────
        // Read RFC 6455 client frames (always masked) and echo unmasked
        // with FIN=1, same opcode.
        bool conn_alive = true;
        while (conn_alive && running.load(std::memory_order_acquire)) {
            if (poll_readable(cfd, 100) == 0) continue;
            uint8_t hdr[2];
            if (!recv_exact(cfd, hdr, 2)) break;
            uint8_t opcode = hdr[0] & 0x0F;
            bool fin    = (hdr[0] & 0x80) != 0;
            bool masked = (hdr[1] & 0x80) != 0;
            uint64_t plen = hdr[1] & 0x7F;

            if (plen == 126) {
                uint8_t ext[2];
                if (!recv_exact(cfd, ext, 2)) break;
                plen = (uint64_t(ext[0]) << 8) | ext[1];
            } else if (plen == 127) {
                uint8_t ext[8];
                if (!recv_exact(cfd, ext, 8)) break;
                plen = 0;
                for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
            }

            uint8_t mask[4] = {0};
            if (masked) {
                if (!recv_exact(cfd, mask, 4)) break;
            }
            if (plen > kBufSize) break;
            if (plen > 0) {
                if (!recv_exact(cfd, buf.data(), static_cast<size_t>(plen))) break;
                if (masked) {
                    for (uint64_t i = 0; i < plen; ++i) {
                        buf[i] ^= mask[i & 3];
                    }
                }
            }

            // Close frame from client → echo close back, then exit.
            if (opcode == 0x8) {
                uint8_t close_hdr[2] = {0x88, static_cast<uint8_t>(plen)};
                send_exact(cfd, close_hdr, 2);
                if (plen > 0) send_exact(cfd, buf.data(), static_cast<size_t>(plen));
                conn_alive = false;
                break;
            }
            // Ping → reply with Pong, same payload.
            if (opcode == 0x9) {
                uint8_t pong_hdr[2] = {0x8A, static_cast<uint8_t>(plen)};
                send_exact(cfd, pong_hdr, 2);
                if (plen > 0) send_exact(cfd, buf.data(), static_cast<size_t>(plen));
                continue;
            }

            // Build server frame: FIN=1, same opcode, no mask.
            uint8_t out_hdr[10];
            size_t hdr_len = 0;
            out_hdr[0] = 0x80 | (opcode & 0x0F);
            if (plen < 126) {
                out_hdr[1] = static_cast<uint8_t>(plen);
                hdr_len = 2;
            } else if (plen <= 0xFFFF) {
                out_hdr[1] = 126;
                out_hdr[2] = static_cast<uint8_t>(plen >> 8);
                out_hdr[3] = static_cast<uint8_t>(plen & 0xFF);
                hdr_len = 4;
            } else {
                out_hdr[1] = 127;
                for (int i = 0; i < 8; ++i) {
                    out_hdr[2 + i] = static_cast<uint8_t>(plen >> ((7 - i) * 8));
                }
                hdr_len = 10;
            }
            if (!send_exact(cfd, out_hdr, hdr_len)) break;
            if (plen > 0 && !send_exact(cfd, buf.data(), static_cast<size_t>(plen))) break;
            (void)fin; // single-frame messages only — no fragment reassembly
        }
        ::close(cfd);
    }
    ::close(listen_fd);
    spdlog::info("test_e2e ws_echo_mock @ :{} exiting", port);
}

} // namespace eph::dpdk::test_e2e
