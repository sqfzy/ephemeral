/// @file test_transport_tls_ws_e2e.cpp
/// End-to-end regression test: Transport / DirectTxTransport / DirectTransport
/// with TLS 1.3 + WebSocket framing against an in-process echo server.
///
/// **Why this test exists**: prior to commit 4eab3fb, the codebase had no
/// integration test that exercised the combination
/// `Transport<...> + use_tls=true + WsFramer + real send/recv round-trip`.
/// The TLS hot-path AEAD ordering bug
/// (`.artifacts/fix-tls-ordering-symptoms-20260409.txt`) was therefore latent
/// for ~7 days. This test pins that combination and protects all 4 affected
/// callsites (Transport::create, Transport reconnect, DirectTxTransport::create,
/// DirectTxTransport reconnect) plus DirectTransport which already had the
/// correct ordering.
///
/// Each test spins up a fresh `TlsWsEchoServer` (in-process, ephemeral cert,
/// ephemeral port) and connects via SocketTransport with TLS+WS, sends a
/// message, and verifies the echo arrives. The reconnect tests additionally
/// kill the server-side session mid-flight and verify reconnect + recovery.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "eph/net/socket_config.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/transport/direct_transport.hpp"
#include "eph/transport/direct_tx_transport.hpp"
#include "eph/transport/presets.hpp"
#include "eph/transport/transport.hpp"
#include "eph/transport/transport_types.hpp"

#include "tls_ws_echo_server.hpp"

namespace {

using namespace std::chrono_literals;
using eph::net::SocketConfig;
using eph::net::SocketTransport;
using eph::net::TransportConfig;
using eph::net::ws::opcode::kText;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Build a TransportConfig pointing at the in-process server's loopback port.
TransportConfig make_config(uint16_t port) {
    TransportConfig cfg;
    cfg.remote_host = "127.0.0.1";
    cfg.remote_port = port;
    cfg.use_tls     = true;
    cfg.verify_peer = false;       // ephemeral self-signed cert
    cfg.ws_path     = "/";
    cfg.skip_utf8_validation = true;  // we send arbitrary bytes as text frames
    cfg.tls_timeout = std::chrono::milliseconds{2000};
    cfg.ws_timeout  = std::chrono::milliseconds{2000};
    cfg.max_reconnect_attempts = 5;
    cfg.reconnect_interval     = std::chrono::milliseconds{20};
    cfg.max_reconnect_backoff  = std::chrono::milliseconds{200};
    return cfg;
}

/// Build a SocketTransport TcpFactory pointing at 127.0.0.1:port.
auto make_tcp_factory(uint16_t port) {
    return [port]() -> std::expected<std::unique_ptr<SocketTransport>, std::string> {
        SocketConfig sc;
        sc.host        = "127.0.0.1";
        sc.port        = port;
        sc.tcp_nodelay = true;
        auto tcp = std::make_unique<SocketTransport>(sc);
        auto r = tcp->connect(std::chrono::milliseconds{2000});
        if (!r) return std::unexpected(r.error());
        return tcp;
    };
}

/// Spin until predicate is true or timeout expires.
template <typename Pred>
bool spin_until(Pred&& p, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return p();
}

// ═════════════════════════════════════════════════════════════════════════════
// Threaded Transport (DefaultTransport<SocketTransport>)
// ═════════════════════════════════════════════════════════════════════════════

TEST(TransportTlsWsE2E, ThreadedTransport_RoundTrip) {
    using TT = eph::net::DefaultTransport<SocketTransport>;

    eph::test::TlsWsEchoServer server;
    server.start();

    auto cfg = make_config(server.port());
    auto result = TT::create(make_tcp_factory(server.port()), cfg);
    ASSERT_TRUE(result.has_value())
        << "Transport::create failed: " << result.error().message();
    auto& tp = *result;

    const char msg[] = "hello tls ws";
    ASSERT_EQ(tp->send_text(msg, sizeof(msg) - 1),
              eph::net::SendError::kOk);

    std::string received;
    bool got = tp->wait_recv(
        [&](const uint8_t* data, size_t len) {
            received.assign(reinterpret_cast<const char*>(data), len);
        },
        2000ms);

    EXPECT_TRUE(got) << "no message received within 2s — pre-fix this would "
                        "fail with TLS decrypt error and trigger reconnect";
    EXPECT_EQ(received, "hello tls ws");

    auto stats = tp->stats();
    EXPECT_EQ(stats.decrypt_errors, 0u)
        << "decrypt errors > 0 indicates the AEAD ordering bug is back";
    EXPECT_EQ(stats.reconnect_count, 0u);

    tp->stop();
}

TEST(TransportTlsWsE2E, ThreadedTransport_ReconnectAfterServerClose) {
    using TT = eph::net::DefaultTransport<SocketTransport>;

    eph::test::TlsWsEchoServer server;
    server.start();

    auto cfg = make_config(server.port());
    auto result = TT::create(make_tcp_factory(server.port()), cfg);
    ASSERT_TRUE(result.has_value())
        << "Transport::create failed: " << result.error().message();
    auto& tp = *result;

    // Round 1
    ASSERT_EQ(tp->send_text("round1", 6), eph::net::SendError::kOk);
    std::string r1;
    ASSERT_TRUE(tp->wait_recv(
        [&](const uint8_t* d, size_t l) {
            r1.assign(reinterpret_cast<const char*>(d), l);
        }, 2000ms));
    EXPECT_EQ(r1, "round1");

    // Server-side disconnect → client-side reconnect
    server.kill_active_sessions();

    // Wait for the reconnect logic to bring the connection back
    bool reconnected = spin_until([&] {
        return tp->stats().reconnect_count >= 1
            && tp->state() == eph::net::TransportState::kConnected;
    }, 5000ms);
    ASSERT_TRUE(reconnected)
        << "client did not reconnect within 5s — pre-fix the reconnect path "
           "hits the same AEAD ordering bug";

    // Round 2 — exercises the reconnect callsite of arm_aead_crypto
    ASSERT_EQ(tp->send_text("round2", 6), eph::net::SendError::kOk);
    std::string r2;
    ASSERT_TRUE(tp->wait_recv(
        [&](const uint8_t* d, size_t l) {
            r2.assign(reinterpret_cast<const char*>(d), l);
        }, 2000ms));
    EXPECT_EQ(r2, "round2");

    auto stats = tp->stats();
    EXPECT_EQ(stats.decrypt_errors, 0u);
    EXPECT_GE(stats.reconnect_count, 1u);

    tp->stop();
}

// ═════════════════════════════════════════════════════════════════════════════
// DirectTxTransport (DirectTxDefaultTransport<SocketTransport>)
// ═════════════════════════════════════════════════════════════════════════════

TEST(TransportTlsWsE2E, DirectTxTransport_RoundTrip) {
    using TT = eph::net::DirectTxDefaultTransport<SocketTransport>;

    eph::test::TlsWsEchoServer server;
    server.start();

    auto cfg = make_config(server.port());
    auto result = TT::create(make_tcp_factory(server.port()), cfg);
    ASSERT_TRUE(result.has_value())
        << "DirectTxTransport::create failed: " << result.error().message();
    auto& tp = *result;

    ASSERT_EQ(tp->send_text("hello-direct-tx", 15),
              eph::net::SendError::kOk);

    std::string received;
    bool got = tp->wait_recv(
        [&](const uint8_t* d, size_t l) {
            received.assign(reinterpret_cast<const char*>(d), l);
        },
        2000ms);

    EXPECT_TRUE(got);
    EXPECT_EQ(received, "hello-direct-tx");

    auto stats = tp->stats();
    EXPECT_EQ(stats.decrypt_errors, 0u);
    EXPECT_EQ(stats.reconnect_count, 0u);

    tp->stop();
}

TEST(TransportTlsWsE2E, DirectTxTransport_ReconnectAfterServerClose) {
    using TT = eph::net::DirectTxDefaultTransport<SocketTransport>;

    eph::test::TlsWsEchoServer server;
    server.start();

    auto cfg = make_config(server.port());
    auto result = TT::create(make_tcp_factory(server.port()), cfg);
    ASSERT_TRUE(result.has_value())
        << "DirectTxTransport::create failed: " << result.error().message();
    auto& tp = *result;

    ASSERT_EQ(tp->send_text("rt1", 3), eph::net::SendError::kOk);
    std::string r1;
    ASSERT_TRUE(tp->wait_recv(
        [&](const uint8_t* d, size_t l) {
            r1.assign(reinterpret_cast<const char*>(d), l);
        }, 2000ms));
    EXPECT_EQ(r1, "rt1");

    server.kill_active_sessions();

    bool reconnected = spin_until([&] {
        return tp->stats().reconnect_count >= 1
            && tp->state() == eph::net::TransportState::kConnected;
    }, 5000ms);
    ASSERT_TRUE(reconnected);

    ASSERT_EQ(tp->send_text("rt2", 3), eph::net::SendError::kOk);
    std::string r2;
    ASSERT_TRUE(tp->wait_recv(
        [&](const uint8_t* d, size_t l) {
            r2.assign(reinterpret_cast<const char*>(d), l);
        }, 2000ms));
    EXPECT_EQ(r2, "rt2");

    auto stats = tp->stats();
    EXPECT_EQ(stats.decrypt_errors, 0u);

    tp->stop();
}

// ═════════════════════════════════════════════════════════════════════════════
// DirectTransport (DirectDefaultTransport<SocketTransport>)
// — uses on_message callback + manual poll() instead of wait_recv
// ═════════════════════════════════════════════════════════════════════════════

TEST(TransportTlsWsE2E, DirectTransport_RoundTrip) {
    using TT = eph::net::DirectDefaultTransport<SocketTransport>;

    eph::test::TlsWsEchoServer server;
    server.start();

    std::atomic<bool> got{false};
    std::string received;

    auto cfg = make_config(server.port());
    cfg.on_message = [&](const uint8_t* d, uint16_t l, uint8_t /*op*/) {
        received.assign(reinterpret_cast<const char*>(d), l);
        got.store(true, std::memory_order_release);
    };

    auto result = TT::create(make_tcp_factory(server.port()), cfg);
    ASSERT_TRUE(result.has_value())
        << "DirectTransport::create failed: " << result.error().message();
    auto& tp = *result;

    ASSERT_EQ(tp->send_text("hello-direct", 12),
              eph::net::SendError::kOk);

    bool received_in_time = spin_until([&] {
        // DirectTransport requires the app to poll() to drain RX
        auto pr = tp->poll();
        (void)pr;
        return got.load(std::memory_order_acquire);
    }, 2000ms);

    EXPECT_TRUE(received_in_time);
    EXPECT_EQ(received, "hello-direct");

    tp->stop();
}

} // namespace
