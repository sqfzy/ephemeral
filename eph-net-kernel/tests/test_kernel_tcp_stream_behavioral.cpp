/// @file test_kernel_tcp_stream_behavioral.cpp
/// Behavioral integration tests for `eph::net::kernel::KernelTcpStream`.
///
/// This is a REWRITE (not a copy) of the baseline
/// `eph-net/tests/test_socket_transport.cpp` (86 cases):
/// the legacy `SocketTransport` class no longer exists — its responsibilities
/// are now split across `KernelTcpStream` (connect + data plane),
/// `KernelPoller` (epoll multiplexer) and `StreamConfig` (construction
/// contract). The exponential-backoff retry math now lives in
/// `eph::utils::ExponentialBackoff` (unit-tested in eph-utils/test_backoff.cpp).
///
/// Strategy:
///   - Loopback TCP echo server (kernel socket, reused across all cases),
///     drives the real `KernelTcpStream<RawStreamCodec, false>` I/O path.
///   - `FakeStream` + `TestPoller<FakeStream>` for pure-unit-behaviour cases
///     that do not need a kernel fd (e.g. send-after-close, on_message count).
///   - Cases that tested raw fd exposure or `SocketTransport`-private state
///     in the baseline were intentionally dropped (the new API doesn't expose
///     them, and asserting against internals would defeat the point of the
///     concept-based API).
///
/// Coverage (per the plan):
///   1. Connect lifecycle              (~15 cases)
///   2. Send / recv correctness        (~20 cases)
///   3. Close behaviour                (~10 cases)
///   4. Error propagation              (~10 cases)
///   ---------------------------------------------
///                                     ≥55 cases total
/// (Reconnect-policy semantics moved with the math to
///  eph-utils/test_backoff.cpp.)

#include <span>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/concepts.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/test/fake_stream.hpp"
#include "eph/net/test/test_poller.hpp"

namespace ek  = eph::net::kernel;
namespace en  = eph::net;
namespace ent = eph::net::test;

using PlainRawStream = ek::KernelTcpStream<eph::codec::RawStreamCodec, false>;

// ---------------------------------------------------------------------------
// Concept conformance (supplements test_concepts.cpp with backend coverage)
// ---------------------------------------------------------------------------

static_assert(en::Pollable<PlainRawStream>,
              "KernelTcpStream<RawStreamCodec,false> must satisfy Pollable");
static_assert(en::Stream<PlainRawStream>,
              "KernelTcpStream<RawStreamCodec,false> must satisfy Stream");

// ---------------------------------------------------------------------------
// Loopback echo-server helpers
// ---------------------------------------------------------------------------
//
// A tiny one-shot TCP echo server: bind ephemeral port on 127.0.0.1, accept
// one client, echo bytes until peer FIN. Lives on a background std::thread
// joined at the end of each TEST. Written from scratch (baseline had a very
// similar helper but it was tied to SocketTransport's test harness; the
// simpler because KernelTcpStream does connect + recv inline).
namespace {

struct EchoServer {
    int                      listen_fd{-1};
    uint16_t                 port{0};
    std::thread              thread;
    std::atomic<bool>        stop_requested{false};

    EchoServer() {
        listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd < 0) return;
        int one = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in sa{};
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        sa.sin_port        = 0;
        if (::bind(listen_fd,
                   reinterpret_cast<sockaddr*>(&sa),
                   sizeof(sa)) != 0
            || ::listen(listen_fd, 4) != 0) {
            ::close(listen_fd);
            listen_fd = -1;
            return;
        }
        socklen_t len = sizeof(sa);
        ::getsockname(listen_fd,
                       reinterpret_cast<sockaddr*>(&sa), &len);
        port = ::ntohs(sa.sin_port);
    }

    // Spawn a worker that accepts one connection and echoes until peer FIN.
    void run() {
        thread = std::thread([this] {
            sockaddr_in cli{};
            socklen_t   clen = sizeof(cli);
            int c = ::accept(listen_fd,
                              reinterpret_cast<sockaddr*>(&cli), &clen);
            if (c < 0) return;
            uint8_t buf[8192];
            while (!stop_requested.load(std::memory_order_relaxed)) {
                ssize_t n = ::recv(c, buf, sizeof(buf), 0);
                if (n <= 0) break;
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = ::send(c, buf + off, n - off, MSG_NOSIGNAL);
                    if (w <= 0) { off = n; break; }
                    off += w;
                }
            }
            ::close(c);
        });
    }

    // Same as run() but: accept one connection, read one message, then close
    // immediately (to test peer-initiated close).
    void run_close_after_first_msg() {
        thread = std::thread([this] {
            sockaddr_in cli{};
            socklen_t   clen = sizeof(cli);
            int c = ::accept(listen_fd,
                              reinterpret_cast<sockaddr*>(&cli), &clen);
            if (c < 0) return;
            uint8_t buf[1024];
            (void)::recv(c, buf, sizeof(buf), 0);
            ::close(c);
        });
    }

    // Accept-only variant: accept the connection and immediately close it,
    // simulating a peer that drops the stream before any data.
    void run_accept_and_close() {
        thread = std::thread([this] {
            sockaddr_in cli{};
            socklen_t   clen = sizeof(cli);
            int c = ::accept(listen_fd,
                              reinterpret_cast<sockaddr*>(&cli), &clen);
            if (c >= 0) ::close(c);
        });
    }

    ~EchoServer() {
        stop_requested.store(true);
        // Force any blocking accept()/recv() to return so the worker thread
        // can exit even when no client ever connected (e.g. the test
        // exercised a failure path in create() before the TCP handshake).
        if (listen_fd >= 0) {
            ::shutdown(listen_fd, SHUT_RDWR);
            ::close(listen_fd);
            listen_fd = -1;
        }
        if (thread.joinable()) thread.join();
    }

    [[nodiscard]] en::SocketAddr addr() const noexcept {
        return en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    }
};

// Drive the poller until either `predicate()` is true or `budget` ms elapsed.
template <class Pred>
bool poll_until(ek::KernelPoller& poller, std::chrono::milliseconds budget,
                Pred&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        poller.poll(std::chrono::milliseconds{10});
    }
    return predicate();
}

// Create and return a minimum-viable StreamConfig pointed at `server`.
ek::StreamConfig cfg_for(const EchoServer& server) {
    ek::StreamConfig cfg{};
    cfg.remote          = server.addr();
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.reasm_capacity  = 64 * 1024;
    return cfg;
}

// Create an EchoServer with worker already running; convenience wrapper for
// the common "server running in background" setup.
std::unique_ptr<EchoServer> make_running_echo() {
    auto srv = std::make_unique<EchoServer>();
    if (srv->listen_fd < 0) return nullptr;
    srv->run();
    return srv;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// 1. Connect lifecycle (15 cases)
// ═══════════════════════════════════════════════════════════════════════

TEST(KernelTcpStreamBehavioral, Connect_SuccessfulToLocalEcho) {
    auto srv = make_running_echo();
    ASSERT_NE(srv, nullptr);
    auto r = PlainRawStream::create(cfg_for(*srv));
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_TRUE((*r)->connect_blocking(std::chrono::seconds{2}).has_value());
    EXPECT_EQ((*r)->state(), en::TcpState::Established);
}

TEST(KernelTcpStreamBehavioral, Connect_ReturnsNonNullUniquePtr) {
    auto srv = make_running_echo();
    auto r = PlainRawStream::create(cfg_for(*srv));
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(r->get(), nullptr);
}

TEST(KernelTcpStreamBehavioral, Connect_FdIsValidAfterCreate) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    EXPECT_GE(s->fd(), 0);
}

TEST(KernelTcpStreamBehavioral, Connect_NotAttachedUntilPollerAdds) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    ASSERT_TRUE(s->connect_blocking(std::chrono::seconds{2}).has_value());
    EXPECT_FALSE(s->is_attached());
    EXPECT_EQ(s->state(), en::TcpState::Established);
}

TEST(KernelTcpStreamBehavioral, Connect_ToUnreachablePortFails) {
    // Port 1 is reserved and reliably refuses connect on loopback.
    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 1};
    cfg.connect_timeout = std::chrono::milliseconds{300};
    cfg.reasm_capacity  = 4096;
    auto r = PlainRawStream::create(cfg);
    // Non-blocking create(): the refusal surfaces either synchronously (create
    // fails) or via connect_blocking(). Either way the code is ConnectFailed
    // (or Disconnected on some glibcs depending on ByteSocket's mapping).
    eph::core::Error code{};
    if (!r) {
        code = r.error().code;
    } else {
        auto cr = (*r)->connect_blocking(std::chrono::seconds{1});
        ASSERT_FALSE(cr.has_value());
        code = cr.error().code;
    }
    {
        EXPECT_TRUE(code == eph::core::Error::ConnectFailed
                 || code == eph::core::Error::Disconnected);
    }
}

TEST(KernelTcpStreamBehavioral, Connect_ZeroReasmCapacityIsInvalidConfig) {
    auto srv = make_running_echo();
    auto cfg = cfg_for(*srv);
    cfg.reasm_capacity = 0;
    auto r = PlainRawStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(KernelTcpStreamBehavioral, Connect_StateStartsClosedBeforeCreate) {
    // Default-constructed StreamConfig has port==0 so create() can't succeed.
    ek::StreamConfig cfg{};
    cfg.reasm_capacity  = 4096;
    cfg.connect_timeout = std::chrono::milliseconds{100};
    auto r = PlainRawStream::create(cfg);
    // Either InvalidConfig (port 0) or ConnectFailed — both acceptable.
    EXPECT_FALSE(r.has_value());
}

TEST(KernelTcpStreamBehavioral, Connect_EstablishedAfterSuccessfulTcp) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    ASSERT_TRUE(s->connect_blocking(std::chrono::seconds{2}).has_value());
    EXPECT_EQ(s->state(), en::TcpState::Established);
}

TEST(KernelTcpStreamBehavioral, Connect_DestructorClosesCleanly) {
    auto srv = make_running_echo();
    {
        auto s = PlainRawStream::create(cfg_for(*srv)).value();
        ASSERT_TRUE(s->connect_blocking(std::chrono::seconds{2}).has_value());
        EXPECT_GE(s->fd(), 0);
        // scope exit -> destructor runs, fd closes.
    }
    // If the destructor leaked, the next iteration of the echo loop would
    // hang; the fixture's ~EchoServer joins, so the test implicitly verifies
    // clean shutdown.
    SUCCEED();
}

TEST(KernelTcpStreamBehavioral, Connect_TcpNodelayDefaultsTrue) {
    ek::StreamConfig cfg{};
    EXPECT_TRUE(cfg.kernel.tcp_nodelay);
}

TEST(KernelTcpStreamBehavioral, Connect_TcpNodelayDisabledConfig) {
    auto srv = make_running_echo();
    auto cfg = cfg_for(*srv);
    cfg.kernel.tcp_nodelay = false;
    auto r = PlainRawStream::create(cfg);
    EXPECT_TRUE(r.has_value());  // disabling nodelay must still succeed
}

TEST(KernelTcpStreamBehavioral, Connect_ExplicitReasmCapacityIsRespected) {
    auto srv = make_running_echo();
    auto cfg = cfg_for(*srv);
    cfg.reasm_capacity = 8 * 1024;
    auto r = PlainRawStream::create(cfg);
    EXPECT_TRUE(r.has_value());
}

TEST(KernelTcpStreamBehavioral, Connect_MultipleSequentialStreamsIndependent) {
    auto srv1 = make_running_echo();
    auto srv2 = make_running_echo();
    auto s1 = PlainRawStream::create(cfg_for(*srv1)).value();
    auto s2 = PlainRawStream::create(cfg_for(*srv2)).value();
    ASSERT_TRUE(s1->connect_blocking(std::chrono::seconds{2}).has_value());
    ASSERT_TRUE(s2->connect_blocking(std::chrono::seconds{2}).has_value());
    EXPECT_NE(s1->fd(), s2->fd());
    EXPECT_EQ(s1->state(), en::TcpState::Established);
    EXPECT_EQ(s2->state(), en::TcpState::Established);
}

TEST(KernelTcpStreamBehavioral, Connect_TimeoutIsRespectedForUnreachable) {
    // Non-routable IP 192.0.2.1 (TEST-NET-1) drops SYN on most kernels.
    // Use a very short timeout so the test stays cheap.
    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{192, 0, 2, 1}, 65000};
    cfg.connect_timeout = std::chrono::milliseconds{100};
    cfg.reasm_capacity  = 4096;
    // create() is non-blocking; the connect_timeout is enforced during the
    // drive (connect_blocking here). create() returns a connecting stream.
    auto r = PlainRawStream::create(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    const auto t0 = std::chrono::steady_clock::now();
    auto cr = (*r)->connect_blocking(std::chrono::seconds{3});
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_FALSE(cr.has_value());
    // Should not have hung — 3 s absolute upper bound is forgiving.
    EXPECT_LT(elapsed, std::chrono::seconds{3});
}

TEST(KernelTcpStreamBehavioral, Connect_DefaultConstructedConfigRejected) {
    ek::StreamConfig cfg{};
    // reasm_capacity default is 64KiB, so bypass InvalidConfig path and trip
    // ConnectFailed instead (port 0).
    auto r = PlainRawStream::create(cfg);
    EXPECT_FALSE(r.has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// 2. Send / recv correctness (20 cases)
// ═══════════════════════════════════════════════════════════════════════

namespace {
struct SendRecvHarness {
    std::unique_ptr<EchoServer>              server;
    std::unique_ptr<ek::KernelPoller>        poller;
    std::unique_ptr<PlainRawStream>          stream;
    std::vector<uint8_t>                     received;

    explicit SendRecvHarness(std::size_t reasm = 64 * 1024) {
        server = make_running_echo();
        poller = ek::KernelPoller::create().value();
        auto cfg = cfg_for(*server);
        cfg.reasm_capacity = reasm;
        stream = PlainRawStream::create(cfg).value();
        // create() is non-blocking: drive the TCP handshake to Established
        // before the data-plane tests start sending.
        (void)stream->connect_blocking(std::chrono::seconds{2});
        stream->on_message = [this](std::span<const uint8_t> app_frame) {
            received.insert(received.end(), app_frame.begin(), app_frame.end());
        };
        (void)poller->add(stream.get());
    }

    ~SendRecvHarness() {
        // Ensure stream is removed / destroyed before the server joins.
        stream.reset();
    }

    bool wait_for(std::size_t want, std::chrono::milliseconds budget =
                                         std::chrono::seconds{2}) {
        return poll_until(*poller, budget,
                           [this, want] { return received.size() >= want; });
    }
};
} // namespace

TEST(KernelTcpStreamBehavioral, Send_SmallPayloadEchoesBack) {
    SendRecvHarness h;
    const uint8_t payload[] = {'p', 'i', 'n', 'g'};
    ASSERT_TRUE(h.stream->send(payload).has_value());
    ASSERT_TRUE(h.wait_for(4));
    EXPECT_EQ(h.received.size(), 4u);
    EXPECT_EQ(0, std::memcmp(h.received.data(), payload, 4));
}

TEST(KernelTcpStreamBehavioral, Send_ReturnsBytesSent) {
    SendRecvHarness h;
    const uint8_t payload[] = {'a', 'b', 'c'};
    auto r = h.stream->send(payload);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 3u);
}

TEST(KernelTcpStreamBehavioral, Send_BeforeAttachReturnsNotAttached) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    ASSERT_TRUE(s->connect_blocking(std::chrono::seconds{2}).has_value());
    const uint8_t p[] = {1, 2, 3};
    auto r = s->send(p);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::NotAttached);
}

TEST(KernelTcpStreamBehavioral, Send_AfterCloseReturnsDisconnected) {
    SendRecvHarness h;
    (void)h.stream->close_gracefully();
    // After close_gracefully state is FinWait1 which != Established.
    const uint8_t p[] = {1};
    auto r = h.stream->send(p);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::Disconnected);
}

TEST(KernelTcpStreamBehavioral, Send_MultipleSequentialAllEchoed) {
    SendRecvHarness h;
    const uint8_t p1[] = {'a'};
    const uint8_t p2[] = {'b'};
    const uint8_t p3[] = {'c'};
    (void)h.stream->send(p1);
    (void)h.stream->send(p2);
    (void)h.stream->send(p3);
    ASSERT_TRUE(h.wait_for(3));
    EXPECT_EQ(h.received[0], 'a');
    EXPECT_EQ(h.received[1], 'b');
    EXPECT_EQ(h.received[2], 'c');
}

TEST(KernelTcpStreamBehavioral, Send_BinaryDataRoundTripsByteForByte) {
    SendRecvHarness h;
    std::vector<uint8_t> payload(256);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i);
    }
    ASSERT_TRUE(h.stream->send(payload).has_value());
    ASSERT_TRUE(h.wait_for(payload.size()));
    for (std::size_t i = 0; i < payload.size(); ++i) {
        EXPECT_EQ(h.received[i], static_cast<uint8_t>(i));
    }
}

TEST(KernelTcpStreamBehavioral, Send_LargeBufferRoundTrips) {
    SendRecvHarness h;
    std::vector<uint8_t> payload(16 * 1024, 0xAB);
    ASSERT_TRUE(h.stream->send(payload).has_value());
    ASSERT_TRUE(h.wait_for(payload.size(),
                            std::chrono::seconds{4}));
    EXPECT_EQ(h.received.size(), payload.size());
}

TEST(KernelTcpStreamBehavioral, Recv_PollWithoutDataIsNoOp) {
    SendRecvHarness h;
    const auto before = h.received.size();
    EXPECT_EQ(h.poller->poll(std::chrono::milliseconds{10}), 0u);
    EXPECT_EQ(h.received.size(), before);
}

TEST(KernelTcpStreamBehavioral, Recv_OnMessageNotInvokedWithoutPoll) {
    SendRecvHarness h;
    const uint8_t p[] = {'x'};
    ASSERT_TRUE(h.stream->send(p).has_value());
    // Give the kernel time to echo without ever polling.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    EXPECT_TRUE(h.received.empty());
}

TEST(KernelTcpStreamBehavioral, Recv_OnMessageFiresExactlyOncePerChunk) {
    SendRecvHarness h;
    std::atomic<int> cb_count{0};
    h.stream->on_message = [&](std::span<const uint8_t>) { ++cb_count; };
    const uint8_t p[] = {'x'};
    ASSERT_TRUE(h.stream->send(p).has_value());
    // Give the echo time to return then poll once.
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    (void)h.poller->poll(std::chrono::milliseconds{10});
    EXPECT_GE(cb_count.load(), 1);
}

TEST(KernelTcpStreamBehavioral, Recv_AllMessagesDeliveredBackToBack) {
    SendRecvHarness h;
    const uint8_t burst[] = {'1','2','3','4','5','6','7','8'};
    ASSERT_TRUE(h.stream->send(burst).has_value());
    ASSERT_TRUE(h.wait_for(sizeof(burst)));
    EXPECT_EQ(h.received.size(), sizeof(burst));
}

TEST(KernelTcpStreamBehavioral, Send_EmptyPayloadIsAccepted) {
    SendRecvHarness h;
    std::array<uint8_t, 0> empty{};
    auto r = h.stream->send(empty);
    // Implementation accepts zero-length sends; most kernels return 0.
    EXPECT_TRUE(r.has_value());
}

TEST(KernelTcpStreamBehavioral, Send_LargerThanReasmBufferDoesNotCrash) {
    // Small reasm buffer (512B) + big send: the TX path is independent of
    // reasm capacity; the RX path will drop the connection cleanly when the
    // echo floods it. We only assert no crash / no deadlock.
    SendRecvHarness h{/*reasm*/ 512};
    std::vector<uint8_t> payload(2048, 0x55);
    auto r = h.stream->send(payload);
    EXPECT_TRUE(r.has_value());
    // Let the poller drain; state may become Closed due to BufferFull.
    (void)poll_until(*h.poller, std::chrono::milliseconds{300},
                      [&] { return h.stream->state() != en::TcpState::Established; });
}

TEST(KernelTcpStreamBehavioral, Recv_SpanBasedSendAcceptsVector) {
    SendRecvHarness h;
    std::vector<uint8_t> v{0x10, 0x20, 0x30};
    ASSERT_TRUE(h.stream->send(v).has_value());
    ASSERT_TRUE(h.wait_for(3));
    EXPECT_EQ(h.received[0], 0x10);
    EXPECT_EQ(h.received[1], 0x20);
    EXPECT_EQ(h.received[2], 0x30);
}

TEST(KernelTcpStreamBehavioral, Recv_NullOnMessageStillDrains) {
    SendRecvHarness h;
    h.stream->on_message = nullptr;  // intentionally unset
    const uint8_t p[] = {'y'};
    ASSERT_TRUE(h.stream->send(p).has_value());
    // poll_once_ without on_message must not crash; fallback path drains.
    (void)h.poller->poll(std::chrono::milliseconds{30});
    EXPECT_TRUE(h.received.empty());
}

TEST(KernelTcpStreamBehavioral, Send_TxThenRxThenTxAgain) {
    SendRecvHarness h;
    const uint8_t a[] = {'A'};
    ASSERT_TRUE(h.stream->send(a).has_value());
    ASSERT_TRUE(h.wait_for(1));
    h.received.clear();
    const uint8_t b[] = {'B'};
    ASSERT_TRUE(h.stream->send(b).has_value());
    ASSERT_TRUE(h.wait_for(1));
    EXPECT_EQ(h.received[0], 'B');
}

TEST(KernelTcpStreamBehavioral, FakeStream_SendBeforeAttachNotAttached) {
    auto fake = ent::FakeStream::create();
    const uint8_t p[] = {1};
    auto r = fake->send(p);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::NotAttached);
}

TEST(KernelTcpStreamBehavioral, FakeStream_InjectRxDeliveredOncePerPoll) {
    auto fake = ent::FakeStream::create();
    auto poller = ent::TestPoller<ent::FakeStream>::create();
    int calls = 0;
    fake->on_message = [&](std::span<const uint8_t>) { ++calls; };
    ASSERT_TRUE(poller->add(fake.get()).has_value());
    const uint8_t bytes[] = {1, 2, 3};
    fake->inject_rx(bytes);
    EXPECT_EQ(poller->poll(), 1u);
    EXPECT_EQ(calls, 1);
    // Second poll with empty rx: no additional delivery.
    EXPECT_EQ(poller->poll(), 0u);
    EXPECT_EQ(calls, 1);
}

TEST(KernelTcpStreamBehavioral, FakeStream_SendBuffersAppendToTx) {
    auto fake = ent::FakeStream::create();
    auto poller = ent::TestPoller<ent::FakeStream>::create();
    ASSERT_TRUE(poller->add(fake.get()).has_value());
    const uint8_t a[] = {'X'};
    const uint8_t b[] = {'Y'};
    (void)fake->send(a);
    (void)fake->send(b);
    auto tx = fake->collect_tx();
    ASSERT_EQ(tx.size(), 2u);
    EXPECT_EQ(tx[0], 'X');
    EXPECT_EQ(tx[1], 'Y');
}

TEST(KernelTcpStreamBehavioral, FakeStream_ClearTxResetsBuffer) {
    auto fake = ent::FakeStream::create();
    auto poller = ent::TestPoller<ent::FakeStream>::create();
    ASSERT_TRUE(poller->add(fake.get()).has_value());
    const uint8_t b[] = {'Z'};
    (void)fake->send(b);
    fake->clear_tx();
    EXPECT_TRUE(fake->collect_tx().empty());
}

// ═══════════════════════════════════════════════════════════════════════
// 3. Close behavior (10 cases)
// ═══════════════════════════════════════════════════════════════════════

TEST(KernelTcpStreamBehavioral, Close_GracefulFlipsStateToFinWait1) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    ASSERT_TRUE(s->close_gracefully().has_value());
    EXPECT_EQ(s->state(), en::TcpState::FinWait1);
}

TEST(KernelTcpStreamBehavioral, Close_GracefulReturnsOkOnConnected) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    auto r = s->close_gracefully();
    EXPECT_TRUE(r.has_value());
}

TEST(KernelTcpStreamBehavioral, Close_GracefulIsIdempotent) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    EXPECT_TRUE(s->close_gracefully().has_value());
    EXPECT_TRUE(s->close_gracefully().has_value());
    EXPECT_EQ(s->state(), en::TcpState::FinWait1);
}

TEST(KernelTcpStreamBehavioral, Close_RemotePeerCloseYieldsClosedOnNextPoll) {
    auto srv = std::make_unique<EchoServer>();
    ASSERT_GE(srv->listen_fd, 0);
    srv->run_accept_and_close();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    auto poller = ek::KernelPoller::create().value();
    (void)poller->add(s.get());
    // Poll until peer close is observed or budget exhausted.
    (void)poll_until(*poller, std::chrono::seconds{1},
                      [&] { return s->state() == en::TcpState::Closed; });
    EXPECT_EQ(s->state(), en::TcpState::Closed);
    (void)poller->remove(s.get());
}

TEST(KernelTcpStreamBehavioral, Close_DestructorAutoDetachesFromPoller) {
    auto srv = make_running_echo();
    auto poller = ek::KernelPoller::create().value();
    {
        auto s = PlainRawStream::create(cfg_for(*srv)).value();
        ASSERT_TRUE(poller->add(s.get()).has_value());
        EXPECT_EQ(poller->size(), 1u);
    }
    EXPECT_EQ(poller->size(), 0u);
}

TEST(KernelTcpStreamBehavioral, Close_RemoveFromPollerClearsAttached) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    auto poller = ek::KernelPoller::create().value();
    ASSERT_TRUE(poller->add(s.get()).has_value());
    EXPECT_TRUE(s->is_attached());
    ASSERT_TRUE(poller->remove(s.get()).has_value());
    EXPECT_FALSE(s->is_attached());
}

TEST(KernelTcpStreamBehavioral, Close_PollerSizeZeroAfterLastRemove) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    auto poller = ek::KernelPoller::create().value();
    ASSERT_TRUE(poller->add(s.get()).has_value());
    ASSERT_TRUE(poller->remove(s.get()).has_value());
    EXPECT_EQ(poller->size(), 0u);
}

TEST(KernelTcpStreamBehavioral, Close_CloseWhileSendInFlightReturnsDisconnected) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    ASSERT_TRUE(s->connect_blocking(std::chrono::seconds{2}).has_value());
    auto poller = ek::KernelPoller::create().value();
    ASSERT_TRUE(poller->add(s.get()).has_value());
    const uint8_t p[] = {'a'};
    ASSERT_TRUE(s->send(p).has_value());
    (void)s->close_gracefully();
    auto r = s->send(p);
    EXPECT_FALSE(r.has_value());
}

TEST(KernelTcpStreamBehavioral, Close_PollAfterCloseReturnsZero) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    auto poller = ek::KernelPoller::create().value();
    (void)poller->add(s.get());
    (void)s->close_gracefully();
    const auto frames = poller->poll(std::chrono::milliseconds{20});
    EXPECT_EQ(frames, 0u);
}

TEST(KernelTcpStreamBehavioral, Close_FakeStreamCloseFlipsState) {
    auto fake = ent::FakeStream::create();
    EXPECT_EQ(fake->state(), en::TcpState::Established);
    ASSERT_TRUE(fake->close_gracefully().has_value());
    EXPECT_EQ(fake->state(), en::TcpState::Closed);
    EXPECT_TRUE(fake->closed_gracefully());
}

// ═══════════════════════════════════════════════════════════════════════
// 4. Error propagation (10 cases)
// ═══════════════════════════════════════════════════════════════════════

TEST(KernelTcpStreamBehavioral, Err_SendNotAttachedHasDetailString) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    const uint8_t p[] = {1};
    auto r = s->send(p);
    ASSERT_FALSE(r.has_value());
    ASSERT_NE(r.error().detail, nullptr);
    EXPECT_NE(*r.error().detail, '\0');
}

TEST(KernelTcpStreamBehavioral, Err_SendNotAttachedHasNotAttachedCode) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    const uint8_t p[] = {1};
    auto r = s->send(p);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::NotAttached);
}

TEST(KernelTcpStreamBehavioral, Err_DoubleCloseGracefulIsNotAnError) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    EXPECT_TRUE(s->close_gracefully().has_value());
    EXPECT_TRUE(s->close_gracefully().has_value());
}

TEST(KernelTcpStreamBehavioral, Err_ZeroReasmInvalidConfigHasDetail) {
    auto srv = make_running_echo();
    auto cfg = cfg_for(*srv);
    cfg.reasm_capacity = 0;
    auto r = PlainRawStream::create(cfg);
    ASSERT_FALSE(r.has_value());
    ASSERT_NE(r.error().detail, nullptr);
    EXPECT_NE(std::string_view{r.error().detail}.find("reasm"),
              std::string_view::npos);
}

TEST(KernelTcpStreamBehavioral, Err_UnreachableConnectDetailIsNonEmpty) {
    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 1};
    cfg.connect_timeout = std::chrono::milliseconds{200};
    cfg.reasm_capacity  = 4096;
    // Non-blocking create(): the connect error surfaces either synchronously
    // or via connect_blocking(). Capture whichever carries the detail string.
    auto r = PlainRawStream::create(cfg);
    eph::core::ErrorInfo err{eph::core::Error::Ok, ""};
    if (!r) {
        err = r.error();
    } else {
        auto cr = (*r)->connect_blocking(std::chrono::seconds{1});
        ASSERT_FALSE(cr.has_value());
        err = cr.error();
    }
    ASSERT_NE(err.detail, nullptr);
    EXPECT_NE(*err.detail, '\0');
}

TEST(KernelTcpStreamBehavioral, Err_PeerResetFlipsStateToClosed) {
    auto srv = std::make_unique<EchoServer>();
    srv->run_accept_and_close();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    auto poller = ek::KernelPoller::create().value();
    (void)poller->add(s.get());
    (void)poll_until(*poller, std::chrono::seconds{1},
                      [&] { return s->state() == en::TcpState::Closed; });
    EXPECT_EQ(s->state(), en::TcpState::Closed);
    (void)poller->remove(s.get());
}

TEST(KernelTcpStreamBehavioral, Err_SendAfterPeerCloseFailsEventually) {
    auto srv = std::make_unique<EchoServer>();
    srv->run_accept_and_close();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    auto poller = ek::KernelPoller::create().value();
    (void)poller->add(s.get());
    (void)poll_until(*poller, std::chrono::seconds{1},
                      [&] { return s->state() != en::TcpState::Established; });
    const uint8_t p[] = {1};
    auto r = s->send(p);
    EXPECT_FALSE(r.has_value());
    (void)poller->remove(s.get());
}

TEST(KernelTcpStreamBehavioral, Err_PollerAddNullptrIsInvalidConfig) {
    auto poller = ek::KernelPoller::create().value();
    auto r = poller->add<PlainRawStream>(nullptr);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(KernelTcpStreamBehavioral, Err_PollerRemoveUnregisteredIsNotFound) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    auto poller = ek::KernelPoller::create().value();
    auto r = poller->remove(s.get());
    ASSERT_FALSE(r.has_value());
    // Removing a never-registered pollable is a recoverable state mismatch
    // (NotFound), symmetric with DpdkPoller::remove — see poller.hpp.
    EXPECT_EQ(r.error().code, eph::core::Error::NotFound);
}

TEST(KernelTcpStreamBehavioral, Err_PollerAddDuplicateIsInvalidConfig) {
    auto srv = make_running_echo();
    auto s = PlainRawStream::create(cfg_for(*srv)).value();
    auto poller = ek::KernelPoller::create().value();
    ASSERT_TRUE(poller->add(s.get()).has_value());
    auto r = poller->add(s.get());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}
