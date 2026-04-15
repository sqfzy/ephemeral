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
// DNS mock — minimal RFC 1035 A-record responder.
//
// Listens on (ip, port) and answers any incoming query with a single A
// record pointing to `resolved_ip` (host byte order).  Echoes the
// transaction ID, sets QR/RD/RA, and uses a compression pointer (0xc00c)
// to reuse the question's QNAME in the answer section so the wire format
// stays minimal but still exercises dns::detail::skip_dns_name's pointer
// path on the client side.
//
// Intentionally trusts the query's qd_count=1 / single-question shape
// (which is what dns::resolve always sends).  Malformed queries get no
// reply — the client will time out.
// ─────────────────────────────────────────────────────────────────────────
inline void dns_mock_thread(const std::string& ip, uint16_t port,
                             uint32_t resolved_ip,
                             std::atomic<bool>& running) noexcept {
    auto fd_r = eph::net::posix::udp_bind(ip, port);
    if (!fd_r) {
        spdlog::error("test_e2e dns_mock {}:{} bind: {}",
                      ip, port, fd_r.error());
        return;
    }
    int fd = *fd_r;
    spdlog::info("test_e2e dns_mock listening on {}:{} (resolved_ip=0x{:08x})",
                 ip, port, resolved_ip);

    constexpr size_t kBufSize = 1500;
    std::vector<uint8_t> rx(kBufSize);
    std::vector<uint8_t> tx(kBufSize);

    while (running.load(std::memory_order_acquire)) {
        sockaddr_in src{};
        socklen_t srclen = sizeof(src);
        ssize_t n = ::recvfrom(fd, rx.data(), kBufSize, 0,
                                reinterpret_cast<sockaddr*>(&src), &srclen);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        // Need at least DNS header (12) + minimal question (5: 1-byte
        // label + null + qtype(2) + qclass(2)).  Reject smaller packets.
        if (n < 17) continue;

        // Walk the question's QNAME so we know where it ends — needed
        // both to compute the response size and to validate the query
        // wasn't truncated mid-label.
        size_t q_off = 12;  // start of question
        while (q_off < static_cast<size_t>(n)) {
            uint8_t lbl = rx[q_off];
            if (lbl == 0) { q_off += 1; break; }
            // Reject pointer-form QNAMEs in queries; not used by the
            // client and not worth implementing in a mock.
            if ((lbl & 0xC0) != 0) { q_off = 0; break; }
            if (lbl > 63) { q_off = 0; break; }
            q_off += 1u + lbl;
        }
        if (q_off == 0 || q_off + 4 > static_cast<size_t>(n)) continue;
        size_t qname_len = q_off - 12;  // includes terminating 0
        size_t question_len = qname_len + 4;  // + qtype + qclass

        // ── Build response in tx[] ────────────────────────────────────
        // Header: echo id, flags = QR|RD|RA, qd=1, an=1, ns=ar=0
        std::memcpy(tx.data(), rx.data(), 2);  // tx_id
        tx[2] = 0x81;  // QR=1 RD=1
        tx[3] = 0x80;  // RA=1, RCODE=0
        tx[4] = 0; tx[5] = 1;  // qd_count = 1
        tx[6] = 0; tx[7] = 1;  // an_count = 1
        tx[8] = 0; tx[9] = 0;  // ns_count = 0
        tx[10] = 0; tx[11] = 0;  // ar_count = 0

        // Echo the question section verbatim
        std::memcpy(tx.data() + 12, rx.data() + 12, question_len);
        size_t off = 12 + question_len;

        // Answer: NAME = compression pointer to question QNAME at offset 12
        tx[off++] = 0xc0;
        tx[off++] = 0x0c;
        // TYPE = A (1)
        tx[off++] = 0x00; tx[off++] = 0x01;
        // CLASS = IN (1)
        tx[off++] = 0x00; tx[off++] = 0x01;
        // TTL = 300 seconds
        tx[off++] = 0x00; tx[off++] = 0x00;
        tx[off++] = 0x01; tx[off++] = 0x2c;
        // RDLENGTH = 4
        tx[off++] = 0x00; tx[off++] = 0x04;
        // RDATA = resolved_ip in network byte order
        tx[off++] = static_cast<uint8_t>((resolved_ip >> 24) & 0xff);
        tx[off++] = static_cast<uint8_t>((resolved_ip >> 16) & 0xff);
        tx[off++] = static_cast<uint8_t>((resolved_ip >>  8) & 0xff);
        tx[off++] = static_cast<uint8_t>( resolved_ip        & 0xff);

        ::sendto(fd, tx.data(), off, MSG_NOSIGNAL,
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

// ─────────────────────────────────────────────────────────────────────────
// WebSocket server-initiated ping mock — proves the client (DpdkTcpStream
// under WsCodec) flushes an auto-response pong from drain_codec_.
//
// Protocol:
//   1. Accept, perform server-side WS upgrade (same bench helpers as
//      ws_echo_mock_thread).
//   2. Send ONE unmasked ping frame with payload "hi".
//   3. Read one client frame. It must be a masked pong with payload "hi".
//   4. On successful verification, send a data (binary) frame with
//      payload "OK" so the client's on_message handler can observe the
//      round trip. This is the signal the e2e test asserts on.
//   5. Drain client bytes until FIN, then close.
//
// Pre-fix bug: step 3 never arrives because drain_codec_() discards the
// out_sink bytes on return, so the server would hang at recv() until the
// client closes. The test deadline (~3s) makes this a hard failure.
// ─────────────────────────────────────────────────────────────────────────
inline void ws_server_ping_mock_thread(const std::string& ip, uint16_t port,
                                        std::atomic<bool>& running) noexcept {
    auto listen_r = eph::net::posix::tcp_bind_listen(ip, port);
    if (!listen_r) {
        spdlog::error("test_e2e ws_server_ping_mock {}:{} bind: {}",
                      ip, port, listen_r.error());
        return;
    }
    int listen_fd = *listen_r;
    spdlog::info("test_e2e ws_server_ping_mock listening on {}:{}", ip, port);

    constexpr size_t kBufSize = 4096;
    constexpr size_t kMaxPayload = 256;
    std::vector<uint8_t> buf(kBufSize);

    while (running.load(std::memory_order_acquire)) {
        auto cfd_r = eph::net::posix::accept_one(listen_fd, running);
        if (!cfd_r) continue;
        if (*cfd_r < 0) break;
        int cfd = *cfd_r;

        // ── 1) HTTP upgrade handshake ────────────────────────────────
        if (auto h = bench::ws_server_handshake(cfd); !h) {
            spdlog::warn("test_e2e ws_server_ping: handshake failed: {}",
                         h.error());
            ::close(cfd);
            continue;
        }

        // ── 2) Proactively send a ping with payload "hi" ─────────────
        const uint8_t ping_payload[] = {'h', 'i'};
        uint8_t ping_frame[16];
        size_t ping_len = bench::ws_framing::build_server_frame(
            ping_frame, bench::ws_framing::kOpPing,
            ping_payload, sizeof(ping_payload));
        if (!eph::net::posix::send_all(cfd, ping_frame, ping_len)) {
            ::close(cfd);
            continue;
        }

        // ── 3) Read the client's reply. Expect a masked pong with
        //       payload "hi". ────────────────────────────────────────
        size_t buf_used = 0;
        bool verified = false;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds{3};
        while (std::chrono::steady_clock::now() < deadline) {
            auto f_opt = bench::ws_framing::parse_client_frame_inplace(
                buf.data(), buf_used, kMaxPayload);
            if (!f_opt) {
                if (buf_used >= kBufSize) break;
                ssize_t n = ::recv(cfd, buf.data() + buf_used,
                                    kBufSize - buf_used, 0);
                if (n <= 0) break;
                buf_used += static_cast<size_t>(n);
                continue;
            }
            const auto& f = *f_opt;
            if (f.opcode == bench::ws_framing::kOpPong &&
                f.payload_len == sizeof(ping_payload)) {
                const uint8_t* p = buf.data() +
                    (f.total_consumed - f.payload_len);
                if (std::memcmp(p, ping_payload, sizeof(ping_payload)) == 0) {
                    verified = true;
                }
            }
            break;
        }

        // ── 4) On verification, send "OK" as a binary frame for the
        //       client's on_message to observe. ─────────────────────
        if (verified) {
            const uint8_t ok_payload[] = {'O', 'K'};
            uint8_t ok_frame[16];
            size_t ok_len = bench::ws_framing::build_server_frame(
                ok_frame, bench::ws_framing::kOpBinary,
                ok_payload, sizeof(ok_payload));
            (void)eph::net::posix::send_all(cfd, ok_frame, ok_len);
        } else {
            spdlog::warn("test_e2e ws_server_ping: pong verification failed "
                         "(buf_used={})", buf_used);
        }

        // ── 5) Drain until peer FIN, then close ────────────────────
        char drain[256];
        while (running.load(std::memory_order_acquire)) {
            ssize_t n = ::recv(cfd, drain, sizeof(drain), 0);
            if (n <= 0) break;
        }
        ::close(cfd);
    }
    ::close(listen_fd);
}

} // namespace eph::dpdk::test_e2e
