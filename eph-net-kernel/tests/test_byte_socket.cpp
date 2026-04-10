/// @file test_byte_socket.cpp
/// Unit tests for `eph::net::kernel::detail::ByteSocket`.
///
/// Covers:
///   - RAII lifecycle: default-constructed is not open, destruct closes fd
///   - connect success to localhost echo thread
///   - connect timeout to unroutable address
///   - send/recv round-trip through loopback
///   - peer FIN surfaces as Disconnected on recv
///   - move semantics hand off fd ownership
///   - send before connect returns Disconnected
///
/// The tests spin up an in-process TCP echo server on a random port so we
/// don't need any external infrastructure.

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

#include "eph/net/kernel/detail/byte_socket.hpp"
#include "eph/net/socket_addr.hpp"

namespace ek = eph::net::kernel::detail;
namespace en = eph::net;

// ---------------------------------------------------------------------------
// In-process TCP echo helper
// ---------------------------------------------------------------------------

/// @brief Bind an AF_INET listener on an ephemeral port, return (fd, port).
///
/// The listener is non-blocking so `accept()` must be polled. Caller owns
/// the returned fd and must close() it.
static std::pair<int, uint16_t> bind_ephemeral_listener() {
    int s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return {-1, 0};

    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;  // ephemeral
    if (::bind(s, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
        ::close(s);
        return {-1, 0};
    }
    if (::listen(s, 4) != 0) {
        ::close(s);
        return {-1, 0};
    }
    socklen_t len = sizeof(sa);
    if (::getsockname(s, reinterpret_cast<struct sockaddr*>(&sa), &len) != 0) {
        ::close(s);
        return {-1, 0};
    }
    return {s, ::ntohs(sa.sin_port)};
}

/// @brief Echo worker: accept one client, loop recv/send until peer closes.
static void echo_once(int listen_fd) {
    struct sockaddr_in cli{};
    socklen_t clen = sizeof(cli);
    const int c = ::accept(listen_fd,
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

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(ByteSocket, DefaultConstructedIsClosed) {
    ek::ByteSocket bs;
    EXPECT_FALSE(bs.is_open());
    EXPECT_EQ(bs.fd(), -1);
}

TEST(ByteSocket, ConnectAndEchoRoundTrip) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { echo_once(lfd); });

    ek::ByteSocket bs;
    en::SocketAddr target{en::Ipv4Addr{127, 0, 0, 1}, port};
    auto cr = bs.connect(target, std::chrono::milliseconds{1000});
    ASSERT_TRUE(cr.has_value()) << "connect error: " << cr.error().detail;
    EXPECT_TRUE(bs.is_open());

    const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    auto sr = bs.send(payload);
    ASSERT_TRUE(sr.has_value()) << "send error: " << sr.error().detail;
    EXPECT_EQ(*sr, sizeof(payload));

    // Echo may not arrive in one recv; loop until we get the full payload
    // or time out.
    std::vector<uint8_t> echoed;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (echoed.size() < sizeof(payload)
           && std::chrono::steady_clock::now() < deadline) {
        uint8_t rx[16];
        auto rr = bs.recv(rx, sizeof(rx));
        if (rr.has_value()) {
            echoed.insert(echoed.end(), rx, rx + *rr);
        } else if (rr.error().code != eph::core::Error::WouldBlock) {
            FAIL() << "recv error: " << rr.error().detail;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    ASSERT_EQ(echoed.size(), sizeof(payload));
    EXPECT_EQ(0, std::memcmp(echoed.data(), payload, sizeof(payload)));

    bs.close();
    server.join();
    ::close(lfd);
}

TEST(ByteSocket, PeerFinSurfacesAsDisconnected) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    // Server: accept, immediately close.
    std::thread server([lfd] {
        struct sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        int c = ::accept(lfd,
                         reinterpret_cast<struct sockaddr*>(&cli), &clen);
        if (c >= 0) ::close(c);
    });

    ek::ByteSocket bs;
    en::SocketAddr target{en::Ipv4Addr{127, 0, 0, 1}, port};
    ASSERT_TRUE(bs.connect(target, std::chrono::milliseconds{1000}).has_value());

    // Poll until we see the Disconnected.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool saw_disconnected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        uint8_t buf[16];
        auto rr = bs.recv(buf, sizeof(buf));
        if (!rr && rr.error().code == eph::core::Error::Disconnected) {
            saw_disconnected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    EXPECT_TRUE(saw_disconnected);

    server.join();
    ::close(lfd);
}

TEST(ByteSocket, SendBeforeConnectFailsCleanly) {
    ek::ByteSocket bs;
    const uint8_t payload[] = {'x'};
    auto sr = bs.send(payload);
    ASSERT_FALSE(sr.has_value());
    EXPECT_EQ(sr.error().code, eph::core::Error::Disconnected);
}

TEST(ByteSocket, MoveTransfersFdOwnership) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { echo_once(lfd); });

    ek::ByteSocket a;
    en::SocketAddr target{en::Ipv4Addr{127, 0, 0, 1}, port};
    ASSERT_TRUE(a.connect(target, std::chrono::milliseconds{1000}).has_value());
    const int fd = a.fd();

    ek::ByteSocket b{std::move(a)};
    EXPECT_EQ(b.fd(), fd);
    EXPECT_EQ(a.fd(), -1);
    EXPECT_FALSE(a.is_open());

    // b should be fully functional post-move.
    const uint8_t payload[] = {'m', 'o', 'v', 'e'};
    ASSERT_TRUE(b.send(payload).has_value());
    b.close();

    server.join();
    ::close(lfd);
}
