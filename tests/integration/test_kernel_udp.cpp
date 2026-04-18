/// @file test_kernel_udp_v3.cpp
/// Cross-module integration test for the new `KernelUdpSocket` API.
///
/// Verifies the design doc Example 2 UDP shape:
///
///   1. KernelPoller::create
///   2. KernelUdpSocket<RawDatagramCodec>::create with bind addr
///   3. attach to poller
///   4. send_to + receive via on_datagram
///
/// against a loopback peer socket.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/udp_socket.hpp"
#include "eph/net/socket_addr.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;
namespace ec = eph::codec;
using namespace std::chrono_literals;

namespace {

using PlainUdp = ek::KernelUdpSocket<ec::RawDatagramCodec>;

/// Bind a kernel UDP socket on 127.0.0.1:0 and return (fd, port).
std::pair<int, uint16_t> bind_loopback_udp() {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    EXPECT_EQ(0, ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)));
    socklen_t len = sizeof(sa);
    EXPECT_EQ(0, ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len));
    return {fd, ::ntohs(sa.sin_port)};
}

} // namespace

// ---------------------------------------------------------------------------
// SendAndReceive: bind a v3 socket, send to a peer, receive the echo
// ---------------------------------------------------------------------------
TEST(KernelUdpV3, SendAndReceive) {
    // Peer (raw kernel socket) that will echo back any datagram it receives.
    auto [peer_fd, peer_port] = bind_loopback_udp();
    ASSERT_GE(peer_fd, 0);

    auto poller = ek::KernelPoller::create({}).value();

    // Build the v3 socket on a different ephemeral port.
    ek::UdpConfig cfg{};
    cfg.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
    auto sr = PlainUdp::create(cfg);
    ASSERT_TRUE(sr.has_value()) << sr.error().detail;
    auto sock = std::move(*sr);

    std::vector<uint8_t> rx;
    en::SocketAddr       rx_src{};
    sock->on_datagram = [&](std::span<const uint8_t> app_datagram,
                            const en::SocketAddr& src) {
        rx.insert(rx.end(), app_datagram.begin(), app_datagram.end());
        rx_src = src;
    };

    ASSERT_TRUE(poller->add(sock.get()).has_value());
    ASSERT_TRUE(sock->is_attached());

    // Send a datagram to the peer.
    const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    auto tx = sock->send_to(payload,
        en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, peer_port});
    ASSERT_TRUE(tx.has_value()) << tx.error().detail;
    EXPECT_EQ(*tx, sizeof(payload));

    // Echo loop in the peer thread: read one datagram, send it back to the
    // source the v3 socket used.
    std::thread peer([peer_fd] {
        uint8_t buf[256];
        sockaddr_in src{};
        socklen_t   slen = sizeof(src);
        // Spin until the v3 socket's datagram arrives (level-triggered fd).
        for (int i = 0; i < 200; ++i) {
            ssize_t n = ::recvfrom(peer_fd, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&src), &slen);
            if (n > 0) {
                ::sendto(peer_fd, buf, static_cast<size_t>(n), 0,
                         reinterpret_cast<sockaddr*>(&src), slen);
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
    });

    // Drive the poller until we see the echo or the deadline.
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (rx.empty() && std::chrono::steady_clock::now() < deadline) {
        poller->poll(50ms);
    }

    peer.join();
    ::close(peer_fd);

    ASSERT_EQ(rx.size(), sizeof(payload));
    EXPECT_EQ(0, std::memcmp(rx.data(), payload, sizeof(payload)));
    EXPECT_EQ(rx_src.port, peer_port);
}

// ---------------------------------------------------------------------------
// JoinLeaveMulticastSurfaces: smoke-test the multicast API surface (no
// real multicast traffic — just ensure the setsockopt path returns OK).
// ---------------------------------------------------------------------------
TEST(KernelUdpV3, JoinLeaveMulticastSurfaces) {
    auto poller = ek::KernelPoller::create({}).value();

    ek::UdpConfig cfg{};
    cfg.bind       = en::SocketAddr{en::Ipv4Addr{0, 0, 0, 0}, 0};
    cfg.reuse_addr = true;
    auto sock = PlainUdp::create(cfg).value();
    ASSERT_TRUE(poller->add(sock.get()).has_value());

    en::SocketAddr group{en::Ipv4Addr{239, 0, 0, 1}, 30000};
    auto j = sock->join_multicast(group);
    ASSERT_TRUE(j.has_value()) << j.error().detail;

    auto l = sock->leave_multicast(group);
    EXPECT_TRUE(l.has_value()) << (l.has_value() ? "" : l.error().detail);
}
