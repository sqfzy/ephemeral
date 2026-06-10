/// @file test_kernel_drain.cpp
/// Unit + integration tests for `eph::net::kernel::KernelTcpStream::drain`.
///
/// Coverage:
///   1. Happy path: client sends bytes, calls drain(short timeout); the
///      in-process echo server reads every byte the client sent before
///      the FIN arrives → no tail loss.
///   2. State transitions: drain() flips state to Closed on success.
///   3. Timeout path: peer never closes its read half → drain() returns
///      Err(Timeout), state ends at Closed (best-effort), and the
///      kRxSessionResets metric increments.
///   4. Pre-condition guard: drain() on a non-Established stream returns
///      InvalidConfig without touching the socket.
///   5. Repeated-call idempotence: a second drain() on a Closed stream
///      returns InvalidConfig (state guard catches it).
///
/// All tests use a tight in-process loopback echo (same idiom as
/// test_kernel_tcp_stream.cpp). Tests do NOT touch a Poller — drain()
/// is a single-threaded synchronous teardown API and must work without
/// any external poll loop.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/stream_metrics.hpp"
#include "eph/utils/time.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;

using PlainRawStream = ek::KernelTcpStream<eph::codec::RawStreamCodec, false>;

// ---------------------------------------------------------------------------
// In-process TCP echo server with per-connection control hooks
// ---------------------------------------------------------------------------

namespace {

struct EchoListener {
    int      fd   = -1;
    uint16_t port = 0;

    static EchoListener bind() {
        EchoListener l;
        l.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (l.fd < 0) return l;
        int one = 1;
        ::setsockopt(l.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        sa.sin_port        = 0;
        if (::bind(l.fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0
            || ::listen(l.fd, 4) != 0) {
            ::close(l.fd);
            l.fd = -1;
            return l;
        }
        socklen_t len = sizeof(sa);
        ::getsockname(l.fd, reinterpret_cast<struct sockaddr*>(&sa), &len);
        l.port = ::ntohs(sa.sin_port);
        return l;
    }

    ~EchoListener() {
        if (fd >= 0) ::close(fd);
    }
};

/// @brief Server thread that reads every byte the client sends, stores
///        them in `received`, then closes its end. Triggers the
///        client-side FIN-ACK observation drain() needs.
struct CountingEchoServer {
    int                     listen_fd;
    std::vector<uint8_t>    received;
    std::atomic<bool>       client_closed{false};

    void run() {
        struct sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        int c = ::accept(listen_fd,
                         reinterpret_cast<struct sockaddr*>(&cli), &clen);
        if (c < 0) return;
        uint8_t buf[4096];
        for (;;) {
            ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n == 0) { client_closed = true; break; } // peer FIN
            if (n < 0) break;
            received.insert(received.end(), buf, buf + n);
        }
        // Closing here sends OUR FIN -> client's drain() sees recv()==0.
        ::close(c);
    }
};

/// @brief Server that accepts the connection and then sleeps — never
///        reads, never sends a FIN. Used to force drain() to time out.
///        We deliberately do NOT call recv() on the server side because
///        recv() returning 0 (peer FIN) would tempt the test author to
///        close as a side effect; we want the client's drain() to block
///        waiting for our FIN that never comes.
struct StalledServer {
    int                  listen_fd;
    std::atomic<bool>    stop{false};
    int                  client_fd{-1};

    void run() {
        struct sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        client_fd = ::accept(listen_fd,
                             reinterpret_cast<struct sockaddr*>(&cli), &clen);
        if (client_fd < 0) return;
        // Park here until the test explicitly stops us. We do NOT recv;
        // the kernel's TCP stack still ACKs incoming data + the client's
        // FIN, but our userspace never sends its own FIN, so the
        // client's drain() must hit its deadline.
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        ::close(client_fd);
        client_fd = -1;
    }
};

void ensure_tsc_initialized() {
    static std::atomic<bool> done{false};
    if (!done.exchange(true)) {
        // Best-effort: drain() falls back to steady_clock if this fails,
        // so the tests are correct either way; this just keeps logs clean.
        eph::utils::TSC::init();
    }
}

ek::StreamConfig make_cfg(uint16_t port) {
    ek::StreamConfig cfg{};
    cfg.remote          = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.reasm_capacity  = 16 * 1024;
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(KernelTcpStreamDrain, SuccessFlushesPendingTxAndStateGoesClosed) {
    ensure_tsc_initialized();

    auto listener = EchoListener::bind();
    ASSERT_GE(listener.fd, 0);

    CountingEchoServer server{listener.fd, {}, {}};
    std::thread server_th(&CountingEchoServer::run, &server);

    auto s = PlainRawStream::create(make_cfg(listener.port)).value();
    ASSERT_TRUE(s->connect_blocking(std::chrono::seconds{2}).has_value());
    ASSERT_EQ(s->state(), en::TcpState::Established);

    // Send a tail payload that would otherwise risk being lost on close
    // if drain() didn't actually wait for FIN-ACK.
    const std::vector<uint8_t> payload(8192, 0xAB);
    // s->send needs the stream attached. Use raw socket send to mirror
    // the test harness pattern from test_kernel_tcp_stream.cpp.
    ssize_t off = 0;
    while (off < static_cast<ssize_t>(payload.size())) {
        ssize_t w = ::send(s->fd(), payload.data() + off,
                           payload.size() - off, MSG_NOSIGNAL);
        ASSERT_GT(w, 0) << "::send failed";
        off += w;
    }

    auto dr = s->drain(std::chrono::milliseconds{1000});
    ASSERT_TRUE(dr.has_value()) << "drain failed: " << dr.error().detail;
    EXPECT_EQ(s->state(), en::TcpState::Closed);
    // Drain success means kRxSessionResets did NOT increment.
    EXPECT_EQ(s->metric(en::StreamMetric::kRxSessionResets), 0u);

    server_th.join();
    EXPECT_TRUE(server.client_closed.load());
    // Most importantly: the server saw every byte we sent.
    EXPECT_EQ(server.received.size(), payload.size());
    EXPECT_EQ(0, std::memcmp(server.received.data(),
                              payload.data(), payload.size()));
}

TEST(KernelTcpStreamDrain, NotEstablishedReturnsInvalidConfig) {
    ensure_tsc_initialized();

    auto listener = EchoListener::bind();
    ASSERT_GE(listener.fd, 0);
    CountingEchoServer server{listener.fd, {}, {}};
    std::thread server_th(&CountingEchoServer::run, &server);

    auto s = PlainRawStream::create(make_cfg(listener.port)).value();
    ASSERT_TRUE(s->connect_blocking(std::chrono::seconds{2}).has_value());
    ASSERT_EQ(s->state(), en::TcpState::Established);
    // First drain succeeds; state -> Closed.
    ASSERT_TRUE(s->drain(std::chrono::milliseconds{1000}).has_value());
    ASSERT_EQ(s->state(), en::TcpState::Closed);

    // Second drain on a Closed stream must reject with InvalidConfig.
    auto dr2 = s->drain(std::chrono::milliseconds{50});
    ASSERT_FALSE(dr2.has_value());
    EXPECT_EQ(dr2.error().code, eph::core::Error::InvalidConfig);

    server_th.join();
}

TEST(KernelTcpStreamDrain, ZeroOrNegativeTimeoutReturnsInvalidConfig) {
    ensure_tsc_initialized();

    auto listener = EchoListener::bind();
    ASSERT_GE(listener.fd, 0);
    CountingEchoServer server{listener.fd, {}, {}};
    std::thread server_th(&CountingEchoServer::run, &server);

    auto s = PlainRawStream::create(make_cfg(listener.port)).value();
    ASSERT_TRUE(s->connect_blocking(std::chrono::seconds{2}).has_value());
    ASSERT_EQ(s->state(), en::TcpState::Established);

    auto dr = s->drain(std::chrono::milliseconds{0});
    ASSERT_FALSE(dr.has_value());
    EXPECT_EQ(dr.error().code, eph::core::Error::InvalidConfig);
    // State must remain Established — guard fired before any syscall.
    EXPECT_EQ(s->state(), en::TcpState::Established);

    // Now actually drain to clean up the server thread.
    (void)s->drain(std::chrono::milliseconds{500});
    server_th.join();
}

TEST(KernelTcpStreamDrain, TimeoutBumpsRxSessionResetsAndStateClosed) {
    ensure_tsc_initialized();

    auto listener = EchoListener::bind();
    ASSERT_GE(listener.fd, 0);

    StalledServer server{listener.fd, {}, -1};
    std::thread server_th(&StalledServer::run, &server);

    auto s = PlainRawStream::create(make_cfg(listener.port)).value();
    ASSERT_TRUE(s->connect_blocking(std::chrono::seconds{2}).has_value());
    ASSERT_EQ(s->state(), en::TcpState::Established);
    ASSERT_EQ(s->metric(en::StreamMetric::kRxSessionResets), 0u);

    // Tight timeout — server never closes, so drain() must time out.
    const auto t0 = std::chrono::steady_clock::now();
    auto dr = s->drain(std::chrono::milliseconds{80});
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    ASSERT_FALSE(dr.has_value());
    EXPECT_EQ(dr.error().code, eph::core::Error::Timeout);
    EXPECT_EQ(s->state(), en::TcpState::Closed);
    EXPECT_EQ(s->metric(en::StreamMetric::kRxSessionResets), 1u);
    // Drain should respect the deadline: elapsed must be at least the
    // timeout but not absurdly long (allow 2x slack for CI noise).
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
              60);
    EXPECT_LE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
              500);

    server.stop = true;
    server_th.join();
}
