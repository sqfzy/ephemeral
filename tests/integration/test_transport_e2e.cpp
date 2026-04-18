/// @file test_transport_e2e.cpp
/// End-to-end test of `KernelTcpStream<RawStreamCodec, false>` against a
/// localhost echo server driven by `KernelPoller`. Exercises the full
/// stack (Stream concept, Poller concept, RawStreamCodec) across a real
/// loopback socket.

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

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/socket_addr.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;
namespace ec = eph::codec;
using namespace std::chrono_literals;

namespace {

using PlainStream = ek::KernelTcpStream<ec::RawStreamCodec, /*Tls=*/false>;

/// Bind a TCP listener on 127.0.0.1:0 and return (fd, port).
std::pair<int, uint16_t> bind_loopback_listener() {
    int lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(lfd, 0);
    int one = 1;
    (void)::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    EXPECT_EQ(0, ::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
    EXPECT_EQ(0, ::listen(lfd, 1));

    socklen_t alen = sizeof(addr);
    EXPECT_EQ(0, ::getsockname(lfd,
                               reinterpret_cast<sockaddr*>(&addr), &alen));
    return {lfd, ::ntohs(addr.sin_port)};
}

/// Single-connection echo server that runs in a thread. Echoes every byte
/// it receives until the client closes the socket.
void run_echo_server(int lfd) {
    int cfd = ::accept(lfd, nullptr, nullptr);
    if (cfd < 0) return;
    char buf[2048];
    for (;;) {
        ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = ::send(cfd, buf + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) { off = n; break; }
            off += w;
        }
    }
    ::close(cfd);
}

} // namespace

// ---------------------------------------------------------------------------
// HappyPath: connect → send → receive → close gracefully
// ---------------------------------------------------------------------------
TEST(TransportE2EV3, HappyPath) {
    auto [lfd, port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { run_echo_server(lfd); });

    auto poller = ek::KernelPoller::create({}).value();

    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity  = 16 * 1024;
    cfg.connect_timeout = 1s;

    auto sr = PlainStream::create(cfg);
    ASSERT_TRUE(sr.has_value()) << "create failed: " << sr.error().detail;
    auto stream = std::move(*sr);

    std::vector<uint8_t> captured;
    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        captured.insert(captured.end(), app_frame.begin(), app_frame.end());
    };

    ASSERT_TRUE(poller->add(stream.get()).has_value());
    ASSERT_TRUE(stream->is_attached());
    EXPECT_EQ(stream->state(), en::TcpState::Established);

    // Send a frame, wait for the echo.
    const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    auto tx = stream->send(payload);
    ASSERT_TRUE(tx.has_value());
    EXPECT_EQ(*tx, sizeof(payload));

    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (captured.size() < sizeof(payload)
           && std::chrono::steady_clock::now() < deadline) {
        poller->poll(50ms);
    }
    ASSERT_EQ(captured.size(), sizeof(payload));
    EXPECT_EQ(0, std::memcmp(captured.data(), payload, sizeof(payload)));

    // Graceful close — half-close sends FIN, server thread exits.
    EXPECT_TRUE(stream->close_gracefully().has_value());

    EXPECT_TRUE(poller->remove(stream.get()).has_value());
    stream.reset();
    server.join();
    ::close(lfd);
}

// ---------------------------------------------------------------------------
// SendBeforeAttachFails: send() must return NotAttached prior to poller add
// ---------------------------------------------------------------------------
TEST(TransportE2EV3, SendBeforeAttachFails) {
    auto [lfd, port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { run_echo_server(lfd); });

    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity  = 4096;
    cfg.connect_timeout = 1s;

    auto sr = PlainStream::create(cfg);
    ASSERT_TRUE(sr.has_value());
    auto stream = std::move(*sr);

    const uint8_t payload[] = {'X'};
    auto tx = stream->send(payload);
    ASSERT_FALSE(tx.has_value());
    EXPECT_EQ(tx.error().code, eph::core::Error::NotAttached);

    // Now attach and gracefully close so the server thread can exit.
    auto poller = ek::KernelPoller::create({}).value();
    ASSERT_TRUE(poller->add(stream.get()).has_value());
    ASSERT_TRUE(stream->close_gracefully().has_value());
    ASSERT_TRUE(poller->remove(stream.get()).has_value());
    stream.reset();
    server.join();
    ::close(lfd);
}

// ---------------------------------------------------------------------------
// MultipleEchoes: drives several round-trips through the same poller loop
// ---------------------------------------------------------------------------
TEST(TransportE2EV3, MultipleEchoes) {
    auto [lfd, port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { run_echo_server(lfd); });

    auto poller = ek::KernelPoller::create({}).value();
    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity  = 16 * 1024;
    cfg.connect_timeout = 1s;
    auto stream = PlainStream::create(cfg).value();

    std::size_t bytes_in = 0;
    stream->on_message = [&](std::span<const uint8_t> app_frame) {
        bytes_in += app_frame.size();
    };

    ASSERT_TRUE(poller->add(stream.get()).has_value());

    constexpr int kRounds = 8;
    constexpr int kPayload = 32;
    uint8_t buf[kPayload];
    for (int i = 0; i < kPayload; ++i) buf[i] = static_cast<uint8_t>(i);

    std::size_t expected = 0;
    for (int r = 0; r < kRounds; ++r) {
        ASSERT_TRUE(stream->send(buf).has_value());
        expected += kPayload;
        auto deadline = std::chrono::steady_clock::now() + 1s;
        while (bytes_in < expected
               && std::chrono::steady_clock::now() < deadline) {
            poller->poll(50ms);
        }
        EXPECT_EQ(bytes_in, expected) << "round " << r;
    }

    ASSERT_TRUE(stream->close_gracefully().has_value());
    ASSERT_TRUE(poller->remove(stream.get()).has_value());
    stream.reset();
    server.join();
    ::close(lfd);
}
