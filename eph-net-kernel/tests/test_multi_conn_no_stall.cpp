/// @file test_multi_conn_no_stall.cpp
/// The headline guarantee of the non-blocking connect work: on a single
/// Poller, one connection stalled mid-handshake must NOT block the others.
///
/// Setup: two `KernelTcpStream`s on one `KernelPoller`.
///   - Stream A dials a blackhole (TEST-NET-1, RFC 5737) — its SYN is dropped,
///     so it stays in `TcpConnecting` for the whole test.
///   - Stream B dials a live in-process loopback echo server.
///
/// With the OLD blocking `create()`, building either stream blocked the
/// calling thread for the full handshake/timeout; a poll loop driving a
/// reconnect would stall every other fd. With the non-blocking state machine,
/// `create()` returns immediately and `poll_once_()` advances each stream
/// independently — so B reaches `Established` and round-trips a payload while
/// A is still frozen at `TcpConnecting`.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;

using PlainRawStream = ek::KernelTcpStream<eph::codec::RawStreamCodec, false>;

namespace {

std::pair<int, uint16_t> bind_ephemeral_listener() {
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

void echo_once(int listen_fd) {
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

} // namespace

TEST(KernelMultiConnNoStall, StalledHandshakeDoesNotBlockOthers) {
    auto [lfd, port] = bind_ephemeral_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { echo_once(lfd); });

    auto poller = ek::KernelPoller::create().value();

    // Stream A → blackhole. A long connect_timeout keeps it stuck in
    // TcpConnecting for the duration of the test (never fails, never blocks).
    ek::StreamConfig cfg_a{};
    cfg_a.remote          = en::SocketAddr{en::Ipv4Addr{192, 0, 2, 1}, 65000};
    cfg_a.connect_timeout = std::chrono::seconds{30};
    cfg_a.reasm_capacity  = 4096;
    auto a = PlainRawStream::create(cfg_a).value();
    ASSERT_EQ(a->handshake_phase(), en::HandshakePhase::TcpConnecting);
    ASSERT_TRUE(poller->add(a.get()).has_value());

    // Stream B → live echo server.
    ek::StreamConfig cfg_b{};
    cfg_b.remote         = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg_b.reasm_capacity = 16 * 1024;
    auto b = PlainRawStream::create(cfg_b).value();
    std::vector<uint8_t> captured;
    b->on_message = [&](std::span<const uint8_t> f) {
        captured.insert(captured.end(), f.begin(), f.end());
    };
    ASSERT_TRUE(poller->add(b.get()).has_value());

    // Drive the SHARED poll loop. B must connect + echo while A stays frozen.
    const uint8_t payload[] = {'a', 'l', 'i', 'v', 'e'};
    bool b_sent = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll(std::chrono::milliseconds{10});
        if (!b_sent && b->handshake_phase() == en::HandshakePhase::Established) {
            ASSERT_TRUE(b->send(payload).has_value());
            b_sent = true;
        }
        if (captured.size() >= sizeof(payload)) break;
    }

    // B made full progress on the shared poll loop...
    EXPECT_TRUE(b_sent) << "stream B never reached Established";
    EXPECT_EQ(captured.size(), sizeof(payload));
    EXPECT_EQ(0, std::memcmp(captured.data(), payload, sizeof(payload)));
    EXPECT_EQ(b->handshake_phase(), en::HandshakePhase::Established);

    // ...while A is STILL stuck mid-handshake — proving its stalled connect
    // neither blocked B nor was waited on by the poll loop.
    EXPECT_EQ(a->handshake_phase(), en::HandshakePhase::TcpConnecting)
        << "stream A should still be handshaking; if it advanced or the loop "
           "blocked on it, the non-blocking guarantee is broken";

    (void)poller->remove(a.get());
    (void)poller->remove(b.get());
    b.reset();
    a.reset();
    server.join();
    ::close(lfd);
}
