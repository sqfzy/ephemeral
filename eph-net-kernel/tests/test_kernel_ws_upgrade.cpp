/// @file test_kernel_ws_upgrade.cpp
/// Integration tests for `KernelTcpStream::create` with a populated
/// `StreamConfig.ws_path`. Spins up a minimal in-process HTTP/1.1 mock
/// server on loopback, accepts one client per test, performs the WS
/// handshake (server-side), and echoes raw bytes thereafter.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/core/detail/base64.hpp"
#include "eph/net/detail/ws_handshake.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;

using PlainStream = ek::KernelTcpStream<eph::codec::RawStreamCodec, false>;

namespace {

// ─────────────────────────────────────────────────────────────────────────
// Mock server primitives
// ─────────────────────────────────────────────────────────────────────────

std::pair<int, uint16_t> bind_loopback_listener() {
    int s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return {-1, 0};
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    if (::bind(s, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0 ||
        ::listen(s, 4) != 0) {
        ::close(s);
        return {-1, 0};
    }
    socklen_t len = sizeof(sa);
    ::getsockname(s, reinterpret_cast<struct sockaddr*>(&sa), &len);
    return {s, ::ntohs(sa.sin_port)};
}

// Read the incoming HTTP request until the \r\n\r\n terminator.
bool read_request(int fd, std::string& out,
                  std::chrono::milliseconds deadline) {
    const auto expiry = std::chrono::steady_clock::now() + deadline;
    while (out.find("\r\n\r\n") == std::string::npos) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= expiry) return false;
        const int rem_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                expiry - now).count());
        struct pollfd p{ .fd = fd, .events = POLLIN, .revents = 0 };
        if (::poll(&p, 1, rem_ms) <= 0) return false;
        char buf[1024];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, static_cast<size_t>(n));
    }
    return true;
}

bool send_all(int fd, const void* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, static_cast<const char*>(data) + off,
                            len - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// Parse the client's Sec-WebSocket-Key from a request string.
std::string extract_key(std::string_view req) {
    constexpr std::string_view needle = "Sec-WebSocket-Key:";
    auto p = req.find(needle);
    if (p == std::string_view::npos) return {};
    p += needle.size();
    while (p < req.size() && (req[p] == ' ' || req[p] == '\t')) ++p;
    auto e = req.find("\r\n", p);
    if (e == std::string_view::npos) return {};
    return std::string(req.substr(p, e - p));
}

// Full, correct WS server handshake. After sending the 101 response we
// echo all subsequent bytes verbatim (NO framing — the test uses
// RawStreamCodec, so the "frame" is simply whatever bytes arrive). This
// covers the post-upgrade data path without pulling in the WS codec.
enum class ServerMode {
    Normal,        ///< well-formed 101
    WrongAccept,   ///< wrong Sec-WebSocket-Accept
    Not101,        ///< 404 Not Found
    MissingUpgrade,///< 101 without Upgrade header
};

void run_mock_server(int listen_fd, ServerMode mode) {
    struct sockaddr_in cli{};
    socklen_t clen = sizeof(cli);
    int c = ::accept(listen_fd,
                     reinterpret_cast<struct sockaddr*>(&cli), &clen);
    if (c < 0) return;

    std::string req;
    if (!read_request(c, req, std::chrono::seconds{2})) {
        ::close(c);
        return;
    }

    std::string resp;
    switch (mode) {
    case ServerMode::Normal: {
        auto key = extract_key(req);
        auto acc = eph::net::detail::ws_compute_accept(key);
        resp = "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " + acc + "\r\n\r\n";
        break;
    }
    case ServerMode::WrongAccept:
        resp = "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: deliberatelyWrong==\r\n\r\n";
        break;
    case ServerMode::Not101:
        resp = "HTTP/1.1 404 Not Found\r\n"
               "Content-Length: 0\r\n\r\n";
        break;
    case ServerMode::MissingUpgrade: {
        auto key = extract_key(req);
        auto acc = eph::net::detail::ws_compute_accept(key);
        resp = "HTTP/1.1 101 Switching Protocols\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " + acc + "\r\n\r\n";
        break;
    }
    }
    if (!send_all(c, resp.data(), resp.size())) {
        ::close(c);
        return;
    }

    // Post-upgrade: echo raw bytes until peer FIN.
    if (mode == ServerMode::Normal) {
        char buf[4096];
        for (;;) {
            ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (!send_all(c, buf, static_cast<size_t>(n))) break;
        }
    }
    ::close(c);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// 1. Plain ws:// handshake succeeds and the stream is Established
// ═══════════════════════════════════════════════════════════════════════

TEST(KernelWsUpgrade, PlainHandshakeSucceedsAndStreamEstablished) {
    auto [lfd, port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { run_mock_server(lfd, ServerMode::Normal); });

    ek::StreamConfig cfg{};
    cfg.remote  = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.ws_path = "/ws/feed";
    cfg.ws_host = "localhost";

    auto stream_r = PlainStream::create(cfg);
    ASSERT_TRUE(stream_r.has_value())
        << (stream_r ? "" : stream_r.error().detail);
    auto stream = std::move(*stream_r);
    EXPECT_EQ(stream->state(), en::TcpState::Established);

    stream.reset();  // triggers close and lets the server thread exit.
    server.join();
    ::close(lfd);
}

// ═══════════════════════════════════════════════════════════════════════
// 2. Connecting to a port with no listener yields ConnectFailed
//    (backwards-compat: ws_path does NOT mask the underlying connect error)
// ═══════════════════════════════════════════════════════════════════════

TEST(KernelWsUpgrade, ConnectFailureBubblesUp) {
    ek::StreamConfig cfg{};
    // Use a port that is almost certainly not listening on loopback.
    cfg.remote  = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, 1};
    cfg.ws_path = "/ws";
    cfg.ws_host = "localhost";
    cfg.connect_timeout = std::chrono::milliseconds{300};

    auto stream_r = PlainStream::create(cfg);
    ASSERT_FALSE(stream_r.has_value());
    EXPECT_EQ(stream_r.error().code, eph::core::Error::ConnectFailed);
}

// ═══════════════════════════════════════════════════════════════════════
// 3. Wrong server response → WsHandshakeFailed
// ═══════════════════════════════════════════════════════════════════════

TEST(KernelWsUpgrade, WrongServerResponseFailsHandshake) {
    auto [lfd, port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { run_mock_server(lfd, ServerMode::WrongAccept); });

    ek::StreamConfig cfg{};
    cfg.remote  = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.ws_path = "/ws";
    cfg.ws_host = "localhost";

    auto stream_r = PlainStream::create(cfg);
    ASSERT_FALSE(stream_r.has_value());
    EXPECT_EQ(stream_r.error().code, eph::core::Error::WsHandshakeFailed);

    server.join();
    ::close(lfd);
}

// ═══════════════════════════════════════════════════════════════════════
// 4. Empty ws_path (no upgrade) → identical behaviour to pre-9.5 TCP stream
// ═══════════════════════════════════════════════════════════════════════

TEST(KernelWsUpgrade, EmptyWsPathSkipsHandshake) {
    auto [lfd, port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);
    // Raw echo server — no HTTP semantics, just read/echo/close.
    std::thread server([lfd] {
        struct sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        int c = ::accept(lfd,
                         reinterpret_cast<struct sockaddr*>(&cli), &clen);
        if (c < 0) return;
        char buf[4096];
        for (;;) {
            ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            (void)!send_all(c, buf, static_cast<size_t>(n));
        }
        ::close(c);
    });

    ek::StreamConfig cfg{};
    cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    // Deliberately leave ws_path empty.
    auto stream_r = PlainStream::create(cfg);
    ASSERT_TRUE(stream_r.has_value())
        << (stream_r ? "" : stream_r.error().detail);
    auto stream = std::move(*stream_r);
    EXPECT_EQ(stream->state(), en::TcpState::Established);
    stream.reset();
    server.join();
    ::close(lfd);
}

// ═══════════════════════════════════════════════════════════════════════
// 5. After the handshake, normal data flow works through the poller path
// ═══════════════════════════════════════════════════════════════════════

TEST(KernelWsUpgrade, PostHandshakeEchoRoundTrips) {
    auto [lfd, port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { run_mock_server(lfd, ServerMode::Normal); });

    ek::StreamConfig cfg{};
    cfg.remote  = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.ws_path = "/ws/feed";
    cfg.ws_host = "localhost";

    auto stream_r = PlainStream::create(cfg);
    ASSERT_TRUE(stream_r.has_value())
        << (stream_r ? "" : stream_r.error().detail);
    auto stream = std::move(*stream_r);

    auto poller = ek::KernelPoller::create().value();

    std::vector<uint8_t> captured;
    stream->on_message = [&](const uint8_t* p, uint16_t n) {
        captured.insert(captured.end(), p, p + n);
    };
    ASSERT_TRUE(poller->add(stream.get()).has_value());

    const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    ASSERT_TRUE(stream->send(payload).has_value());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (captured.size() < sizeof(payload) &&
           std::chrono::steady_clock::now() < deadline) {
        poller->poll(std::chrono::milliseconds{50});
    }
    ASSERT_EQ(captured.size(), sizeof(payload));
    EXPECT_EQ(0, std::memcmp(captured.data(), payload, sizeof(payload)));

    ASSERT_TRUE(poller->remove(stream.get()).has_value());
    stream.reset();
    server.join();
    ::close(lfd);
}

// ═══════════════════════════════════════════════════════════════════════
// 6. Missing Upgrade header in server response → WsHandshakeFailed
// ═══════════════════════════════════════════════════════════════════════

TEST(KernelWsUpgrade, MissingUpgradeHeaderFailsHandshake) {
    auto [lfd, port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);
    std::thread server([lfd] { run_mock_server(lfd, ServerMode::MissingUpgrade); });

    ek::StreamConfig cfg{};
    cfg.remote  = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.ws_path = "/ws";
    cfg.ws_host = "localhost";

    auto stream_r = PlainStream::create(cfg);
    ASSERT_FALSE(stream_r.has_value());
    EXPECT_EQ(stream_r.error().code, eph::core::Error::WsHandshakeFailed);

    server.join();
    ::close(lfd);
}

// ═══════════════════════════════════════════════════════════════════════
// 7. ws_timeout is enforced — server accepts but never sends the 101
//    response; client must fail with Error::Timeout within the configured
//    budget (+ some slack for scheduling).
//
// batch3-round3 MEDIUM-3: the `ws_timeout` StreamConfig field was shipped
// without a test; this closes the gap.
// ═══════════════════════════════════════════════════════════════════════

TEST(KernelWsUpgrade, WsTimeoutIsEnforcedWhenServerStalls) {
    auto [lfd, port] = bind_loopback_listener();
    ASSERT_GE(lfd, 0);

    // Stalled server: accept the connection but never read or write.
    // Holding the client fd open is enough to keep the TCP session alive
    // for the duration of the test.
    std::atomic<bool> stop{false};
    std::thread server([lfd, &stop] {
        struct sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        int c = ::accept(lfd,
                         reinterpret_cast<struct sockaddr*>(&cli), &clen);
        if (c < 0) return;
        // Park until the test signals shutdown.
        while (!stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        ::close(c);
    });

    ek::StreamConfig cfg{};
    cfg.remote     = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.ws_path    = "/ws";
    cfg.ws_host    = "localhost";
    cfg.ws_timeout = std::chrono::milliseconds{150};

    const auto start = std::chrono::steady_clock::now();
    auto stream_r = PlainStream::create(cfg);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(stream_r.has_value());
    EXPECT_EQ(stream_r.error().code, eph::core::Error::Timeout);

    // Tolerate ±1 second of scheduling slack on WSL / loaded CI — the
    // assertion is "did the timeout fire at all within a reasonable
    // bound", not exact timing.
    EXPECT_LT(elapsed, std::chrono::milliseconds{2000})
        << "ws_timeout was not observed within the configured budget";

    stop.store(true, std::memory_order_release);
    server.join();
    ::close(lfd);
}
