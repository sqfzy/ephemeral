/// @file test_kernel_tcp_stream.cpp
/// Unit + integration tests for `eph::net::kernel::KernelTcpStream`.
///
/// Covers:
///   - concept conformance static_asserts for the common instantiations
///   - create() on a live in-process echo server succeeds (TLS off)
///   - create() with TLS enabled currently returns `TlsHandshakeFailed`
///   - send() before attach returns NotAttached
///   - send() + poll drives on_message with the echoed payload
///   - close_gracefully transitions state to FinWait1
///   - destructor auto-detaches the stream from a Poller

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

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;

using PlainRawStream = ek::KernelTcpStream<eph::codec::RawStreamCodec, false>;
using TlsRawStream   = ek::KernelTcpStream<eph::codec::RawStreamCodec, true>;

// ---------------------------------------------------------------------------
// Concept conformance
// ---------------------------------------------------------------------------

static_assert(eph::net::Pollable<PlainRawStream>,
              "KernelTcpStream<RawStreamCodec,false> must be Pollable");
static_assert(eph::net::Stream<PlainRawStream>,
              "KernelTcpStream<RawStreamCodec,false> must be Stream");
static_assert(eph::net::Pollable<TlsRawStream>,
              "KernelTcpStream<RawStreamCodec,true> must be Pollable");
static_assert(eph::net::Stream<TlsRawStream>,
              "KernelTcpStream<RawStreamCodec,true> must be Stream");

// ---------------------------------------------------------------------------
// In-process TCP echo helper (same idea as test_byte_socket.cpp)
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
    if (::bind(s, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0
        || ::listen(s, 4) != 0) {
        ::close(s);
        return {-1, 0};
    }
    socklen_t len = sizeof(sa);
    ::getsockname(s, reinterpret_cast<struct sockaddr*>(&sa), &len);
    return {s, ::ntohs(sa.sin_port)};
}

static void echo_once(int listen_fd) {
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

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(KernelTcpStream, CreatePlaintextConnectsToLocalEcho) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { echo_once(lfd); });

    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.reasm_capacity  = 16 * 1024;

    auto r = PlainRawStream::create(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    auto s = std::move(*r);
    ASSERT_NE(s.get(), nullptr);
    EXPECT_EQ(s->state(), en::TcpState::Established);
    EXPECT_FALSE(s->is_attached());
    EXPECT_GE(s->fd(), 0);

    (void)s->close_gracefully();
    server.join();
    ::close(lfd);
}

TEST(KernelTcpStream, CreateWithTlsReturnsStubError) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    // Server: accept, do nothing — we only need the TCP handshake to succeed.
    std::thread server([lfd] {
        struct sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        int c = ::accept(lfd,
                         reinterpret_cast<struct sockaddr*>(&cli), &clen);
        if (c >= 0) ::close(c);
    });

    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    auto r = TlsRawStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::TlsHandshakeFailed);
    server.join();
    ::close(lfd);
}

TEST(KernelTcpStream, SendBeforeAttachReturnsNotAttached) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { echo_once(lfd); });

    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    auto s = PlainRawStream::create(cfg).value();

    const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    auto sr = s->send(payload);
    ASSERT_FALSE(sr.has_value());
    EXPECT_EQ(sr.error().code, eph::core::Error::NotAttached);

    (void)s->close_gracefully();
    server.join();
    ::close(lfd);
}

TEST(KernelTcpStream, PollDeliversEchoedPayload) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { echo_once(lfd); });

    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    auto s = PlainRawStream::create(cfg).value();

    std::vector<uint8_t> captured;
    s->on_message = [&](const uint8_t* p, uint16_t n) {
        captured.insert(captured.end(), p, p + n);
    };

    auto poller = ek::KernelPoller::create().value();
    ASSERT_TRUE(poller->add(s.get()).has_value());
    ASSERT_TRUE(s->is_attached());

    const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    ASSERT_TRUE(s->send(payload).has_value());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (captured.size() < sizeof(payload)
           && std::chrono::steady_clock::now() < deadline) {
        poller->poll(std::chrono::milliseconds{50});
    }
    ASSERT_EQ(captured.size(), sizeof(payload));
    EXPECT_EQ(0, std::memcmp(captured.data(), payload, sizeof(payload)));

    ASSERT_TRUE(poller->remove(s.get()).has_value());
    EXPECT_FALSE(s->is_attached());

    // Release the stream so the echo thread sees EOF and exits; otherwise
    // server.join() would deadlock waiting for recv() to drain.
    s.reset();
    server.join();
    ::close(lfd);
}

TEST(KernelTcpStream, CloseGracefullyFlipsStateToFinWait1) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { echo_once(lfd); });

    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    auto s = PlainRawStream::create(cfg).value();
    ASSERT_EQ(s->state(), en::TcpState::Established);
    ASSERT_TRUE(s->close_gracefully().has_value());
    EXPECT_EQ(s->state(), en::TcpState::FinWait1);
    server.join();
    ::close(lfd);
}

TEST(KernelTcpStream, DestructorAutoDetachesFromPoller) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { echo_once(lfd); });

    auto poller = ek::KernelPoller::create().value();
    {
        ek::StreamConfig cfg{};
        cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
        auto s = PlainRawStream::create(cfg).value();
        ASSERT_TRUE(poller->add(s.get()).has_value());
        EXPECT_EQ(poller->size(), 1u);
        // s drops out of scope — ~KernelTcpStream must detach itself.
    }
    EXPECT_EQ(poller->size(), 0u);
    server.join();
    ::close(lfd);
}
