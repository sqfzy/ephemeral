/// @file test_kernel_udp_socket.cpp
/// Unit tests for `eph::net::kernel::KernelUdpSocket`.
///
/// Covers:
///   - concept conformance static_asserts
///   - bind + send_to + recv round-trip through a peer UDP socket
///   - NotAttached when send_to called before attach
///   - connect_to (source filtering) API returns Ok on an open socket
///   - join_multicast / leave_multicast API surface (Ok on a valid group
///     with loopback interface — no real traffic verification)
///   - destructor cleanup: socket is closed, detach fires on Poller
///
/// We do NOT verify real multicast delivery because the EC2 CI environment
/// may not route multicast on loopback. The API-surface tests confirm the
/// setsockopt path compiles and runs cleanly.

#include <span>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/udp_socket.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;

using RawUdp = ek::KernelUdpSocket<eph::codec::RawDatagramCodec>;

// ---------------------------------------------------------------------------
// Concept conformance
// ---------------------------------------------------------------------------

static_assert(eph::net::Pollable<RawUdp>,
              "KernelUdpSocket<RawDatagramCodec> must be Pollable");
static_assert(eph::net::Datagram<RawUdp>,
              "KernelUdpSocket<RawDatagramCodec> must be Datagram");
static_assert(eph::net::kernel::KernelPollable<RawUdp>,
              "KernelUdpSocket<RawDatagramCodec> must be KernelPollable");

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(KernelUdpSocket, CreateBindsToEphemeralPort) {
    ek::UdpConfig cfg{};
    cfg.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};

    auto r = RawUdp::create(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    auto s = std::move(*r);
    ASSERT_NE(s.get(), nullptr);
    EXPECT_GE(s->fd(), 0);
    EXPECT_FALSE(s->is_attached());
}

TEST(KernelUdpSocket, SendToBeforeAttachReturnsNotAttached) {
    ek::UdpConfig cfg{};
    cfg.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
    auto s = RawUdp::create(cfg).value();

    const uint8_t payload[] = {'x'};
    auto sr = s->send_to(payload, en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1},
                                                  12345});
    ASSERT_FALSE(sr.has_value());
    EXPECT_EQ(sr.error().code, eph::core::Error::NotAttached);
}

TEST(KernelUdpSocket, RoundTripThroughPeerSocketViaPoller) {
    // Create a peer raw UDP socket that echoes the single datagram we send.
    const int peer = ::socket(AF_INET,
                               SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    ASSERT_GE(peer, 0);
    struct sockaddr_in peer_sa{};
    peer_sa.sin_family      = AF_INET;
    peer_sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    peer_sa.sin_port        = 0;
    ASSERT_EQ(0, ::bind(peer,
                        reinterpret_cast<struct sockaddr*>(&peer_sa),
                        sizeof(peer_sa)));
    socklen_t plen = sizeof(peer_sa);
    ASSERT_EQ(0, ::getsockname(peer,
                               reinterpret_cast<struct sockaddr*>(&peer_sa),
                               &plen));
    const uint16_t peer_port = ::ntohs(peer_sa.sin_port);

    // Our client socket.
    ek::UdpConfig cfg{};
    cfg.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
    auto sock = RawUdp::create(cfg).value();

    std::vector<uint8_t> captured;
    sock->on_datagram = [&](std::span<const uint8_t> app_datagram,
                             const en::SocketAddr&) {
        captured.insert(captured.end(),
                        app_datagram.begin(), app_datagram.end());
    };

    auto poller = ek::KernelPoller::create().value();
    ASSERT_TRUE(poller->add(sock.get()).has_value());

    // Send to peer.
    const uint8_t payload[] = {'h', 'i', '!'};
    auto sr = sock->send_to(payload,
                             en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1},
                                             peer_port});
    ASSERT_TRUE(sr.has_value()) << sr.error().detail;
    EXPECT_EQ(*sr, sizeof(payload));

    // Peer receives the datagram and echoes it back to the source.
    uint8_t            rxbuf[128];
    struct sockaddr_in src{};
    socklen_t          slen = sizeof(src);
    // Poll the peer fd until a datagram arrives (should be instant on loopback).
    ssize_t n = -1;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < deadline) {
        n = ::recvfrom(peer, rxbuf, sizeof(rxbuf), 0,
                       reinterpret_cast<struct sockaddr*>(&src), &slen);
        if (n >= 0) break;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    ASSERT_GT(n, 0);
    ASSERT_EQ(n, (ssize_t)sizeof(payload));
    ssize_t w = ::sendto(peer, rxbuf, n, 0,
                         reinterpret_cast<struct sockaddr*>(&src), slen);
    ASSERT_EQ(w, n);

    // Drive the client poller until on_datagram fires.
    const auto d2 =
        std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (captured.empty() && std::chrono::steady_clock::now() < d2) {
        poller->poll(std::chrono::milliseconds{50});
    }
    ASSERT_EQ(captured.size(), sizeof(payload));
    EXPECT_EQ(0, std::memcmp(captured.data(), payload, sizeof(payload)));

    (void)poller->remove(sock.get());
    ::close(peer);
}

// Boundary: oversized payload classification.
// IPv4/UDP wire-level limit is 65507 bytes (65535 IP total - 20 IPv4 hdr -
// 8 UDP hdr); the kernel responds with EMSGSIZE for anything larger than
// the configured wmem_max / interface MTU. The send_to error-classification
// path (see udp_socket.hpp:223-227) maps EMSGSIZE → InvalidConfig with a
// distinct payload-rejected detail string, separating "bad caller payload"
// from generic Disconnected. Without this test, a future refactor could
// silently re-collapse the EMSGSIZE branch into the catch-all and operators
// would lose the actionable signal.
TEST(KernelUdpSocket, SendToOversizedPayloadIsInvalidConfig) {
    auto poller = ek::KernelPoller::create().value();
    ek::UdpConfig cfg{};
    cfg.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
    auto s = RawUdp::create(cfg).value();
    ASSERT_TRUE(poller->add(s.get()).has_value());

    // 70000 bytes is unambiguously past the 65507-byte UDP/IPv4 ceiling and
    // also past every common wmem_max (2 MiB default) so the kernel returns
    // EMSGSIZE before the packet ever leaves the socket layer.
    std::vector<uint8_t> too_big(70000, 0xAB);
    auto r = s->send_to(too_big,
        en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 12345});
    ASSERT_FALSE(r.has_value())
        << "70000-byte UDP send must surface EMSGSIZE";
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    // detail string must mention EMSGSIZE/EINVAL so log greps still work.
    EXPECT_NE(std::string_view{r.error().detail}.find("EMSGSIZE"),
              std::string_view::npos)
        << "detail: " << r.error().detail;
}

TEST(KernelUdpSocket, ConnectToSucceedsOnOpenSocket) {
    ek::UdpConfig cfg{};
    cfg.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
    auto s = RawUdp::create(cfg).value();
    auto cr = s->connect_to(en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 9999});
    EXPECT_TRUE(cr.has_value());
}

TEST(KernelUdpSocket, JoinLeaveMulticastApiSurface) {
    ek::UdpConfig cfg{};
    cfg.bind       = en::SocketAddr{en::Ipv4Addr{0, 0, 0, 0}, 0};
    cfg.reuse_addr = true;
    auto s = RawUdp::create(cfg).value();

    // 239.1.1.1 is an administratively-scoped multicast address.
    en::SocketAddr group{en::Ipv4Addr{239, 1, 1, 1}, 0};
    // The join may fail on hosts with no multicast-capable interface; we
    // accept either Ok or InvalidConfig here to avoid CI flakes. The
    // important thing is that the API path compiles and runs without
    // crashing.
    auto jr = s->join_multicast(group);
    if (jr.has_value()) {
        auto lr = s->leave_multicast(group);
        EXPECT_TRUE(lr.has_value());
    } else {
        EXPECT_EQ(jr.error().code, eph::core::Error::InvalidConfig);
    }
}

TEST(KernelUdpSocket, DestructorDetachesFromPoller) {
    auto poller = ek::KernelPoller::create().value();
    {
        ek::UdpConfig cfg{};
        cfg.bind = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 0};
        auto s = RawUdp::create(cfg).value();
        ASSERT_TRUE(poller->add(s.get()).has_value());
        EXPECT_EQ(poller->size(), 1u);
    }
    EXPECT_EQ(poller->size(), 0u);
}
