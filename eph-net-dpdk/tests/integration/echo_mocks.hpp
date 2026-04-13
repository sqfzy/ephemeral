#pragma once

/// @file echo_mocks.hpp
/// Kernel-side echo servers for the DPDK E2E test suite.
///
/// All five mock variants below (TCP echo, UDP echo, RST, FIN, WS echo)
/// run inside the dispatcher's child process — one thread each, all
/// bound to NIC_A in the host network namespace.  They call the POSIX
/// bind/listen helpers in `eph::net::posix::` (from `eph/net/posix_io.hpp`
/// and `eph/net/posix_listener.hpp`) so the kernel-side I/O paths stay
/// byte-identical to what a KernelTcpStream would exercise.
///
/// Shutdown semantics: the dispatcher catches SIGTERM, flips its
/// running flag, and immediately `_exit(0)`s.  Worker threads are not
/// joined — process death tears them down.  This means inner recv
/// loops are allowed to block on EINTR retry without compromising
/// clean shutdown.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/net/posix_io.hpp"
#include "eph/net/posix_listener.hpp"

#include "ws_framing.hpp"
#include "ws_handshake.hpp"

namespace eph::dpdk::test_e2e {

// ─────────────────────────────────────────────────────────────────────────
// TCP echo mock — generic byte-stream echo, no framing.
//
// recv up to 16 KiB chunks and echo verbatim until the client closes.
// ─────────────────────────────────────────────────────────────────────────
inline void tcp_echo_mock_thread(const std::string& ip, uint16_t port,
                                  std::atomic<bool>& running) noexcept {
    auto listen_r = eph::net::posix::tcp_bind_listen(ip, port);
    if (!listen_r) {
        spdlog::error("test_e2e tcp_echo_mock {}:{} bind: {}",
                      ip, port, listen_r.error());
        return;
    }
    int listen_fd = *listen_r;
    spdlog::info("test_e2e tcp_echo_mock listening on {}:{}", ip, port);

    constexpr size_t kBufSize = 16384;
    std::vector<uint8_t> buf(kBufSize);

    while (running.load(std::memory_order_acquire)) {
        auto cfd_r = eph::net::posix::accept_one(listen_fd, running);
        if (!cfd_r) {
            spdlog::warn("test_e2e tcp_echo accept: {}", cfd_r.error());
            continue;
        }
        if (*cfd_r < 0) break; // shutdown requested
        int cfd = *cfd_r;

        while (true) {
            ssize_t n = ::recv(cfd, buf.data(), kBufSize, 0);
            if (n <= 0) break;
            if (!eph::net::posix::send_all(cfd, buf.data(), static_cast<size_t>(n))) break;
        }
        ::close(cfd);
    }
    ::close(listen_fd);
}

// ─────────────────────────────────────────────────────────────────────────
// UDP echo mock — recvfrom + sendto.
// ─────────────────────────────────────────────────────────────────────────
inline void udp_echo_mock_thread(const std::string& ip, uint16_t port,
                                  std::atomic<bool>& running) noexcept {
    auto fd_r = eph::net::posix::udp_bind(ip, port);
    if (!fd_r) {
        spdlog::error("test_e2e udp_echo_mock {}:{} bind: {}",
                      ip, port, fd_r.error());
        return;
    }
    int fd = *fd_r;
    spdlog::info("test_e2e udp_echo_mock listening on {}:{}", ip, port);

    constexpr size_t kBufSize = 2048;
    std::vector<uint8_t> buf(kBufSize);

    while (running.load(std::memory_order_acquire)) {
        sockaddr_in src{};
        socklen_t srclen = sizeof(src);
        ssize_t n = ::recvfrom(fd, buf.data(), kBufSize, 0,
                                reinterpret_cast<sockaddr*>(&src), &srclen);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        ::sendto(fd, buf.data(), static_cast<size_t>(n), MSG_NOSIGNAL,
                 reinterpret_cast<sockaddr*>(&src), srclen);
    }
    ::close(fd);
}

// ─────────────────────────────────────────────────────────────────────────
// RST mock — accept then immediately abort with RST via SO_LINGER {1, 0}.
// Used by FailureE2E.PeerRstAfterAccept.
// ─────────────────────────────────────────────────────────────────────────
inline void tcp_rst_mock_thread(const std::string& ip, uint16_t port,
                                 std::atomic<bool>& running) noexcept {
    auto listen_r = eph::net::posix::tcp_bind_listen(ip, port);
    if (!listen_r) {
        spdlog::error("test_e2e tcp_rst_mock {}:{} bind: {}",
                      ip, port, listen_r.error());
        return;
    }
    int listen_fd = *listen_r;
    spdlog::info("test_e2e tcp_rst_mock listening on {}:{}", ip, port);

    while (running.load(std::memory_order_acquire)) {
        auto cfd_r = eph::net::posix::accept_one(listen_fd, running);
        if (!cfd_r) continue;
        if (*cfd_r < 0) break;
        int cfd = *cfd_r;

        // SO_LINGER {on=1, linger=0} → close() drops the socket and
        // sends RST instead of going through FIN/ACK.
        struct linger lg{1, 0};
        ::setsockopt(cfd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        ::close(cfd);
    }
    ::close(listen_fd);
}

// ─────────────────────────────────────────────────────────────────────────
// FIN mock — accept, echo one chunk, then shutdown(WR) so the client
// observes a graceful FIN from the peer side.
// Used by FailureE2E.PeerFinAfterEcho.
// ─────────────────────────────────────────────────────────────────────────
inline void tcp_fin_mock_thread(const std::string& ip, uint16_t port,
                                 std::atomic<bool>& running) noexcept {
    auto listen_r = eph::net::posix::tcp_bind_listen(ip, port);
    if (!listen_r) {
        spdlog::error("test_e2e tcp_fin_mock {}:{} bind: {}",
                      ip, port, listen_r.error());
        return;
    }
    int listen_fd = *listen_r;
    spdlog::info("test_e2e tcp_fin_mock listening on {}:{}", ip, port);

    constexpr size_t kBufSize = 4096;
    std::vector<uint8_t> buf(kBufSize);

    while (running.load(std::memory_order_acquire)) {
        auto cfd_r = eph::net::posix::accept_one(listen_fd, running);
        if (!cfd_r) continue;
        if (*cfd_r < 0) break;
        int cfd = *cfd_r;

        // Read one chunk, echo it, then half-close so the client sees FIN.
        ssize_t n = ::recv(cfd, buf.data(), kBufSize, 0);
        if (n > 0) {
            (void)eph::net::posix::send_all(cfd, buf.data(), static_cast<size_t>(n));
        }
        ::shutdown(cfd, SHUT_WR);
        // Drain anything the client sends after our FIN, then close fully.
        while (true) {
            ssize_t d = ::recv(cfd, buf.data(), kBufSize, 0);
            if (d <= 0) break;
        }
        ::close(cfd);
    }
    ::close(listen_fd);
}

// ─────────────────────────────────────────────────────────────────────────
// WebSocket echo mock — minimal RFC 6455 server.
//
// Reuses bench::ws_server_handshake (HTTP upgrade with bundled SHA-1 +
// base64) and bench::ws_framing helpers (frame parse + build) so the
// kernel-side WS path is byte-identical to the lat_ws bench mock.
//
// Single-frame messages only — no fragment reassembly.  Pipelined
// frames are supported via incremental parse_client_frame_inplace
// from a sliding read buffer.
// ─────────────────────────────────────────────────────────────────────────
inline void ws_echo_mock_thread(const std::string& ip, uint16_t port,
                                 std::atomic<bool>& running) noexcept {
    auto listen_r = eph::net::posix::tcp_bind_listen(ip, port);
    if (!listen_r) {
        spdlog::error("test_e2e ws_echo_mock {}:{} bind: {}",
                      ip, port, listen_r.error());
        return;
    }
    int listen_fd = *listen_r;
    spdlog::info("test_e2e ws_echo_mock listening on {}:{}", ip, port);

    constexpr size_t kBufSize = 65536;
    constexpr size_t kMaxPayload = 32768;
    std::vector<uint8_t> buf(kBufSize);

    while (running.load(std::memory_order_acquire)) {
        auto cfd_r = eph::net::posix::accept_one(listen_fd, running);
        if (!cfd_r) continue;
        if (*cfd_r < 0) break;
        int cfd = *cfd_r;

        // ── Step 1: HTTP upgrade handshake ────────────────────────────
        if (auto h = bench::ws_server_handshake(cfd); !h) {
            spdlog::warn("test_e2e ws_echo: handshake failed: {}", h.error());
            ::close(cfd);
            continue;
        }

        // ── Step 2: frame echo loop with incremental read buffer ─────
        // Bytes accumulate in `buf[0..buf_used)`; parse_client_frame_inplace
        // unmasks in place and reports total_consumed.  After processing
        // we compact and read more.
        size_t buf_used = 0;
        bool conn_alive = true;
        while (conn_alive) {
            // Try to parse a complete frame from what's already buffered.
            auto f_opt = bench::ws_framing::parse_client_frame_inplace(
                buf.data(), buf_used, kMaxPayload);
            if (!f_opt) {
                // Need more bytes.
                if (buf_used >= kBufSize) {
                    spdlog::warn("test_e2e ws_echo: frame too large, dropping");
                    break;
                }
                ssize_t n = ::recv(cfd, buf.data() + buf_used,
                                   kBufSize - buf_used, 0);
                if (n <= 0) break;
                buf_used += static_cast<size_t>(n);
                continue;
            }
            const auto& f = *f_opt;
            uint8_t* payload = buf.data() + (f.total_consumed - f.payload_len);

            // Close frame → echo close back, exit.
            if (f.opcode == bench::ws_framing::kOpClose) {
                uint8_t out[10];
                size_t out_len = bench::ws_framing::build_server_frame(
                    out, bench::ws_framing::kOpClose, payload,
                    static_cast<size_t>(f.payload_len));
                eph::net::posix::send_all(cfd, out, out_len);
                conn_alive = false;
                break;
            }
            // Ping → reply Pong with same payload.
            if (f.opcode == bench::ws_framing::kOpPing) {
                std::vector<uint8_t> out(10 + f.payload_len);
                size_t out_len = bench::ws_framing::build_server_frame(
                    out.data(), bench::ws_framing::kOpPong, payload,
                    static_cast<size_t>(f.payload_len));
                if (!eph::net::posix::send_all(cfd, out.data(), out_len)) break;
            } else {
                // Data frame → echo unmasked, same opcode.
                std::vector<uint8_t> out(10 + f.payload_len);
                size_t out_len = bench::ws_framing::build_server_frame(
                    out.data(), f.opcode, payload,
                    static_cast<size_t>(f.payload_len));
                if (!eph::net::posix::send_all(cfd, out.data(), out_len)) break;
            }

            // Compact: drop the consumed frame from the read buffer.
            if (f.total_consumed < buf_used) {
                std::memmove(buf.data(), buf.data() + f.total_consumed,
                             buf_used - f.total_consumed);
            }
            buf_used -= f.total_consumed;
        }
        ::close(cfd);
    }
    ::close(listen_fd);
}

} // namespace eph::dpdk::test_e2e
