/// @file test_kernel_integration.cpp
/// End-to-end integration test combining `KernelTcpStream`, `KernelPoller`,
/// and `RawStreamCodec`.
///
/// Scenario:
///   1. Spawn an in-process TCP echo server on an ephemeral loopback port.
///   2. Create a plaintext `KernelTcpStream` and attach it to a
///      `KernelPoller`.
///   3. Send "ping" and drive the poller until `on_message` captures it.
///   4. Send "bye" and do the same.
///   5. Close gracefully and verify the server thread exits.
///
/// This test exercises the full data path: ByteSocket → epoll → codec
/// decode → application callback → poll-driven send.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/kernel/udp_socket.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;

using PlainStream = ek::KernelTcpStream<eph::codec::RawStreamCodec, false>;
using PlainUdp    = ek::KernelUdpSocket<eph::codec::RawDatagramCodec>;

namespace {

/// @brief Bind a TCP listener on loopback + ephemeral port.
std::pair<int, uint16_t> bind_loopback_listener() {
    int s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return {-1, 0};
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    if (::bind(s, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0
        || ::listen(s, 4) != 0) {
        ::close(s);
        return {-1, 0};
    }
    socklen_t len = sizeof(sa);
    ::getsockname(s, reinterpret_cast<struct sockaddr*>(&sa), &len);
    return {s, ::ntohs(sa.sin_port)};
}

/// @brief Minimal blocking echo server: accept one client, loop until EOF.
void run_echo_server(int listen_fd) {
    struct sockaddr_in cli{};
    socklen_t clen = sizeof(cli);
    int c = ::accept(listen_fd,
                     reinterpret_cast<struct sockaddr*>(&cli), &clen);
    if (c < 0) return;
    uint8_t buf[4096];
    for (;;) {
        ssize_t n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = ::send(c, buf + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) break;
            off += w;
        }
    }
    ::close(c);
}

} // namespace

TEST(KernelIntegration, FullEchoCycleViaPoller) {
    auto [lfd, port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { run_echo_server(lfd); });

    auto poller = ek::KernelPoller::create().value();

    ek::StreamConfig cfg{};
    cfg.remote         = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity = 16 * 1024;
    auto stream = PlainStream::create(cfg).value();

    std::vector<uint8_t> captured;
    stream->on_message = [&](const uint8_t* p, uint16_t n) {
        captured.insert(captured.end(), p, p + n);
    };

    ASSERT_TRUE(poller->add(stream.get()).has_value());
    ASSERT_TRUE(stream->is_attached());
    ASSERT_EQ(stream->state(), en::TcpState::Established);

    auto wait_for = [&](std::size_t want) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{2};
        while (captured.size() < want
               && std::chrono::steady_clock::now() < deadline) {
            poller->poll(std::chrono::milliseconds{50});
        }
    };

    // 1) send "ping", expect echo
    const uint8_t ping[] = {'p', 'i', 'n', 'g'};
    ASSERT_TRUE(stream->send(ping).has_value());
    wait_for(sizeof(ping));
    ASSERT_EQ(captured.size(), sizeof(ping));
    EXPECT_EQ(0, std::memcmp(captured.data(), ping, sizeof(ping)));

    captured.clear();

    // 2) send "bye", expect echo
    const uint8_t bye[] = {'b', 'y', 'e'};
    ASSERT_TRUE(stream->send(bye).has_value());
    wait_for(sizeof(bye));
    ASSERT_EQ(captured.size(), sizeof(bye));
    EXPECT_EQ(0, std::memcmp(captured.data(), bye, sizeof(bye)));

    // 3) close gracefully — SHUT_WR sends FIN, server recv returns 0 and
    // the thread exits.
    ASSERT_TRUE(stream->close_gracefully().has_value());
    EXPECT_EQ(stream->state(), en::TcpState::FinWait1);

    // Drive the poller briefly so any RX straggler (the server may echo
    // nothing here, but poll_once_ must not fault on the half-open fd).
    poller->poll(std::chrono::milliseconds{50});

    // 4) remove + release before joining to guarantee the server thread
    // sees full teardown.
    ASSERT_TRUE(poller->remove(stream.get()).has_value());
    stream.reset();

    server.join();
    ::close(lfd);
}

// ---------------------------------------------------------------------------
// Heterogeneous Poller: TCP Stream + UDP Socket on one KernelPoller.
// Verifies the P2 function-pointer type-erase across distinct Pollable types.
// ---------------------------------------------------------------------------

TEST(KernelIntegration, TcpAndUdpOnSinglePoller) {
    // ── TCP echo server ──────────────────────────────────────────────────
    auto [lfd, tcp_port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { run_echo_server(lfd); });

    // ── UDP peer that echoes the first datagram back ────────────────────
    int peer = ::socket(AF_INET,
                        SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    ASSERT_GE(peer, 0);
    struct sockaddr_in psa{};
    psa.sin_family      = AF_INET;
    psa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    psa.sin_port        = 0;
    ASSERT_EQ(0, ::bind(peer,
                        reinterpret_cast<struct sockaddr*>(&psa),
                        sizeof(psa)));
    socklen_t plen = sizeof(psa);
    ASSERT_EQ(0, ::getsockname(peer,
                               reinterpret_cast<struct sockaddr*>(&psa),
                               &plen));
    const uint16_t udp_port = ::ntohs(psa.sin_port);

    auto poller = ek::KernelPoller::create().value();

    // TCP side
    ek::StreamConfig scfg{};
    scfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, tcp_port};
    auto tcp_stream = PlainStream::create(scfg).value();
    std::vector<uint8_t> tcp_captured;
    tcp_stream->on_message = [&](const uint8_t* p, uint16_t n) {
        tcp_captured.insert(tcp_captured.end(), p, p + n);
    };
    ASSERT_TRUE(poller->add(tcp_stream.get()).has_value());

    // UDP side
    ek::UdpConfig ucfg{};
    ucfg.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
    auto udp_sock = PlainUdp::create(ucfg).value();
    std::vector<uint8_t> udp_captured;
    udp_sock->on_datagram = [&](const uint8_t* p, uint16_t n,
                                 const en::SocketAddr&) {
        udp_captured.insert(udp_captured.end(), p, p + n);
    };
    ASSERT_TRUE(poller->add(udp_sock.get()).has_value());
    EXPECT_EQ(poller->size(), 2u);

    // Fire both TX paths.
    const uint8_t tcp_payload[] = {'T', 'C', 'P'};
    ASSERT_TRUE(tcp_stream->send(tcp_payload).has_value());

    const uint8_t udp_payload[] = {'U', 'D', 'P'};
    ASSERT_TRUE(udp_sock->send_to(
        udp_payload,
        en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, udp_port}).has_value());

    // Peer echoes the UDP datagram back to its source.
    uint8_t            rxbuf[64];
    struct sockaddr_in src{};
    socklen_t          slen = sizeof(src);
    ssize_t n = -1;
    const auto udp_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < udp_deadline) {
        n = ::recvfrom(peer, rxbuf, sizeof(rxbuf), 0,
                       reinterpret_cast<struct sockaddr*>(&src), &slen);
        if (n >= 0) break;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    ASSERT_GT(n, 0);
    ASSERT_EQ(::sendto(peer, rxbuf, n, 0,
                       reinterpret_cast<struct sockaddr*>(&src), slen),
              n);

    // Drive the single poller until both callbacks have fired.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while ((tcp_captured.size() < sizeof(tcp_payload)
            || udp_captured.size() < sizeof(udp_payload))
           && std::chrono::steady_clock::now() < deadline) {
        poller->poll(std::chrono::milliseconds{50});
    }
    EXPECT_EQ(tcp_captured.size(), sizeof(tcp_payload));
    EXPECT_EQ(udp_captured.size(), sizeof(udp_payload));
    EXPECT_EQ(0, std::memcmp(tcp_captured.data(), tcp_payload,
                             sizeof(tcp_payload)));
    EXPECT_EQ(0, std::memcmp(udp_captured.data(), udp_payload,
                             sizeof(udp_payload)));

    (void)poller->remove(tcp_stream.get());
    (void)poller->remove(udp_sock.get());
    tcp_stream.reset();
    udp_sock.reset();
    server.join();
    ::close(lfd);
    ::close(peer);
}
