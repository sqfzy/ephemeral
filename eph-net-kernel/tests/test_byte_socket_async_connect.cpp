/// @file test_byte_socket_async_connect.cpp
/// Unit tests for `ByteSocket`'s non-blocking connect primitives
/// (`begin_connect` / `poll_connect`) introduced for the non-blocking
/// connect state machine.
///
/// Covers:
///   - begin_connect + drive-to-completion via poll_connect reaches a usable
///     socket (send/recv round-trip)
///   - poll_connect on a connection-refused target surfaces ConnectFailed
///   - poll_connect before begin_connect returns InvalidConfig
///   - begin_connect twice (already open) returns InvalidConfig
///   - the blocking connect() wrapper still behaves (regression for the
///     refactor that re-expressed it on top of the new primitives)
///
/// Uses an in-process loopback listener so no external infra is required.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "eph/net/kernel/detail/byte_socket.hpp"
#include "eph/net/socket_addr.hpp"

namespace ek = eph::net::kernel::detail;
namespace en = eph::net;

// ---------------------------------------------------------------------------
// Loopback helpers (mirrors test_byte_socket.cpp)
// ---------------------------------------------------------------------------

static std::pair<int, uint16_t> bind_ephemeral_listener() {
    int s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return {-1, 0};
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    if (::bind(s, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
        ::close(s);
        return {-1, 0};
    }
    if (::listen(s, 4) != 0) { ::close(s); return {-1, 0}; }
    socklen_t len = sizeof(sa);
    if (::getsockname(s, reinterpret_cast<struct sockaddr*>(&sa), &len) != 0) {
        ::close(s);
        return {-1, 0};
    }
    return {s, ::ntohs(sa.sin_port)};
}

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

/// @brief Drive `poll_connect()` to a terminal result with a deadline.
///        Mirrors what the Stream poll loop will do across poll cycles.
static std::expected<bool, eph::core::ErrorInfo>
drive_to_connected(ek::ByteSocket& bs, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        // Wait for writability like the epoll loop would (zero-busy-spin).
        struct pollfd pfd{};
        pfd.fd     = bs.fd();
        pfd.events = POLLOUT;
        ::poll(&pfd, 1, 20);
        auto r = bs.poll_connect();
        if (!r) return r;            // failed
        if (*r) return true;          // connected
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(eph::core::ErrorInfo{
                eph::core::Error::Timeout, "drive_to_connected: deadline"});
        }
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(ByteSocketAsyncConnect, BeginThenPollReachesUsableSocket) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { echo_once(lfd); });

    ek::ByteSocket bs;
    en::SocketAddr target{en::Ipv4Addr{127, 0, 0, 1}, port};

    auto begun = bs.begin_connect(target);
    ASSERT_TRUE(begun.has_value()) << "begin_connect: " << begun.error().detail;
    // fd is owned from begin_connect onward, whether or not connect completed.
    EXPECT_GE(bs.fd(), 0);

    auto done = drive_to_connected(bs, std::chrono::milliseconds{1000});
    ASSERT_TRUE(done.has_value()) << "drive: " << done.error().detail;
    EXPECT_TRUE(*done);

    const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    auto sr = bs.send(payload);
    ASSERT_TRUE(sr.has_value()) << "send: " << sr.error().detail;

    std::vector<uint8_t> echoed;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (echoed.size() < sizeof(payload)
           && std::chrono::steady_clock::now() < deadline) {
        uint8_t rx[16];
        auto rr = bs.recv(rx, sizeof(rx));
        if (rr) echoed.insert(echoed.end(), rx, rx + *rr);
        else if (rr.error().code != eph::core::Error::WouldBlock)
            FAIL() << "recv: " << rr.error().detail;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    ASSERT_EQ(echoed.size(), sizeof(payload));

    bs.close();
    server.join();
    ::close(lfd);
}

TEST(ByteSocketAsyncConnect, RefusedConnectionSurfacesConnectFailed) {
    // Bind+listen to grab an ephemeral port, then close the listener so the
    // port is (almost certainly) refused. A non-blocking connect returns
    // EINPROGRESS and poll_connect harvests ECONNREFUSED via SO_ERROR.
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    ::close(lfd);

    ek::ByteSocket bs;
    en::SocketAddr target{en::Ipv4Addr{127, 0, 0, 1}, port};
    auto begun = bs.begin_connect(target);
    ASSERT_TRUE(begun.has_value()) << "begin_connect: " << begun.error().detail;

    auto done = drive_to_connected(bs, std::chrono::milliseconds{1000});
    ASSERT_FALSE(done.has_value());
    EXPECT_EQ(done.error().code, eph::core::Error::ConnectFailed);
}

TEST(ByteSocketAsyncConnect, PollConnectBeforeBeginIsInvalidConfig) {
    ek::ByteSocket bs;
    auto r = bs.poll_connect();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(ByteSocketAsyncConnect, BeginConnectTwiceIsInvalidConfig) {
    // Only a listening port is needed so begin_connect succeeds; we never
    // accept (the backlog absorbs the SYN), so no echo thread to join.
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);

    ek::ByteSocket bs;
    en::SocketAddr target{en::Ipv4Addr{127, 0, 0, 1}, port};
    ASSERT_TRUE(bs.begin_connect(target).has_value());
    auto again = bs.begin_connect(target);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code, eph::core::Error::InvalidConfig);

    bs.close();
    ::close(lfd);
}

TEST(ByteSocketAsyncConnect, BlockingConnectWrapperStillWorks) {
    // Regression: the blocking connect() was re-expressed on top of
    // begin_connect/poll_connect. It must still connect and round-trip.
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { echo_once(lfd); });

    ek::ByteSocket bs;
    en::SocketAddr target{en::Ipv4Addr{127, 0, 0, 1}, port};
    auto cr = bs.connect(target, std::chrono::milliseconds{1000});
    ASSERT_TRUE(cr.has_value()) << "connect: " << cr.error().detail;
    EXPECT_TRUE(bs.is_open());

    bs.close();
    server.join();
    ::close(lfd);
}

TEST(ByteSocketAsyncConnect, BlockingConnectTimesOutToUnroutable) {
    // 10.255.255.1 is in RFC1918 space and typically unroutable on dev hosts;
    // the connect should not complete within a tight deadline.
    ek::ByteSocket bs;
    en::SocketAddr target{en::Ipv4Addr{10, 255, 255, 1}, 9};
    auto cr = bs.connect(target, std::chrono::milliseconds{150});
    ASSERT_FALSE(cr.has_value());
    EXPECT_EQ(cr.error().code, eph::core::Error::Timeout);
    EXPECT_FALSE(bs.is_open());  // wrapper closes the fd on the timeout path
}
