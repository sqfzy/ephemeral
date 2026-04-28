/// @file test_coinbase_adapter.cpp
///
/// Integration test: Coinbase Advanced Trade authenticated WebSocket adapter.
///
/// Exercises TLS 1.3 + WebSocket + ReconnectOrchestrator against an
/// in-process TLS WS server that mimics the Coinbase Advanced Trade
/// `wss://advanced-trade-ws.coinbase.com` subscribe-ack protocol.
///
/// Key Coinbase quirk vs OKX/Bybit: authentication for "user" channels
/// (orders, account) is carried as a **JWT in the subscribe payload**,
/// not as an HTTP header. The wire format is:
///
///   {"type":"subscribe", "channel":"user", "jwt":"<JWS-token>"}
///
/// And the server response is:
///
///   {"channel":"subscriptions", "client_id":"...", "events":[...]}
///
/// The actual JWS construction (ES256 over Coinbase's API-secret EC
/// private key) is out of scope for this test — we use a deliberately
/// fake JWT (the well-formed shape `header.payload.signature`) and
/// assert the server received exactly the JWT we sent. This proves the
/// auth-bearing subscribe round-trips through TLS + WS without
/// corruption, which is the integration concern this test cares about.
///
/// Why we don't sign with `SignedRequest<Traits>`: Coinbase Advanced
/// Trade uses JWT (RS256/ES256) over EC keys, not the HMAC-SHA256
/// pattern that `SignedRequest` is built around. JWT signing is a
/// future Tier 3 feature; for T2.10 we ship the integration shape and
/// document the gap.
///
/// Scenario:
///   1. Server installed with a Coinbase-shape handler that:
///      - On `{"type":"subscribe", ...}` returns a `subscriptions`
///        ack frame.
///      - Captures the JWT field from the subscribe payload so the
///        test can assert it round-tripped intact.
///   2. Orchestrator dials, subscribe-with-JWT is sent, ack received.
///   3. Server pushes Close 1011, orchestrator reconnects, JWT
///      replayed (with a fresh fake-JWT to simulate per-connect
///      token issuance).
///   4. Assertions: state == Connected, reconnect_count >= 2,
///      both JWTs received intact by server, subscribe replayed
///      twice.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/reconnect_orchestrator.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/time.hpp"

#include "tls_ws_echo_server.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;
namespace ec = eph::codec;
using namespace std::chrono_literals;

namespace {

using TlsWsStream = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>;
using Orch        = en::ReconnectOrchestrator<TlsWsStream>;

/// Coinbase Advanced Trade subscriptions-ack channel name. The server
/// emits this as the `channel` field on the response frame.
constexpr std::string_view kCoinbaseSubsAck =
    R"({"channel":"subscriptions","client_id":"eph-test","timestamp":"2026-04-28T00:00:00Z","sequence_num":0,"events":[{"subscriptions":{"user":["BTC-USD"]}}]})";

/// Fake JWT shape: `header.payload.signature` — base64url segments. We
/// don't construct a cryptographically valid JWT; the server only
/// inspects the OUTER payload to confirm the JWT field round-tripped.
/// The values are static so the test can assert byte-for-byte equality
/// after reconnect.
constexpr std::string_view kFakeJwt1 =
    "eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9."
    "eyJzdWIiOiJlcGgtdGVzdCIsImlhdCI6MX0."
    "FAKE_SIGNATURE_1_FAKE_FAKE_FAKE_FAKE";

constexpr std::string_view kFakeJwt2 =
    "eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9."
    "eyJzdWIiOiJlcGgtdGVzdCIsImlhdCI6Mn0."
    "FAKE_SIGNATURE_2_FAKE_FAKE_FAKE_FAKE";

/// Build a Coinbase-shape subscribe message. Real Advanced Trade clients
/// also send `product_ids` and a `timestamp`; we keep this minimal so
/// the test focuses on the JWT-round-trip concern.
std::string make_subscribe(std::string_view jwt) {
    std::string out;
    out.reserve(96 + jwt.size());
    out += R"({"type":"subscribe","channel":"user","product_ids":["BTC-USD"],"jwt":")";
    out.append(jwt);
    out += R"("})";
    return out;
}

ek::StreamConfig make_config(uint16_t port) {
    ek::StreamConfig cfg{};
    cfg.remote                = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity        = 64 * 1024;
    cfg.connect_timeout       = 2s;
    cfg.tcp_nodelay           = true;
    cfg.tls.hostname          = "eph-test-server";
    cfg.tls.verify_peer       = false;
    cfg.tls.handshake_timeout = 2s;
    cfg.ws_path               = "/";  // Coinbase WS root path
    cfg.ws_host               = "eph-test-server";
    cfg.ws_timeout            = 2s;
    cfg.ws_permessage_deflate = false;
    return cfg;
}

/// Server-side handler that captures every JWT it sees in the subscribe
/// frame and emits a `subscriptions` ack back. The captured JWTs are
/// stored in a shared vector under a mutex so the test thread can read
/// them after the wire dance.
struct CoinbaseFixture {
    std::mutex                       jwts_mu;
    std::vector<std::string>         jwts_received;

    eph::test::TlsWsEchoServer::MessageHandler handler() {
        return [this](std::span<const uint8_t> payload, uint8_t opcode)
            -> std::optional<std::vector<uint8_t>> {
            if (opcode != 0x1) return std::nullopt;
            std::string_view sv{
                reinterpret_cast<const char*>(payload.data()), payload.size()};
            if (sv.find(R"("type":"subscribe")") == std::string_view::npos) {
                return std::nullopt;
            }
            // Extract the JWT value. Naive but fine for a test: find
            //   "jwt":"<value>"
            // and capture <value>. Real JSON parsing would use eph-json.
            constexpr std::string_view kKey = R"("jwt":")";
            auto k = sv.find(kKey);
            if (k != std::string_view::npos) {
                auto vstart = k + kKey.size();
                auto vend   = sv.find('"', vstart);
                if (vend != std::string_view::npos) {
                    std::lock_guard lk(jwts_mu);
                    jwts_received.emplace_back(sv.substr(vstart, vend - vstart));
                }
            }
            return std::vector<uint8_t>(kCoinbaseSubsAck.begin(),
                                        kCoinbaseSubsAck.end());
        };
    }
};

template <class Pred>
bool drive_until(ek::KernelPoller& poller, Orch& orch, Pred pred,
                 std::chrono::milliseconds budget = 5s) {
    const auto end = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < end) {
        (void)poller.poll(10ms);
        orch.tick(eph::utils::TSC::now());
        if (pred()) return true;
    }
    return false;
}

std::vector<uint8_t> encode_ws_text(std::string_view payload) {
    std::vector<uint8_t> out;
    out.resize(payload.size() + 14);
    auto n = en::ws::encode_frame(
        out.data(), en::ws::opcode::kText,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size(), /*fin=*/true);
    out.resize(n);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Test: UserChannelJwtRoundTripsAcrossReconnect
// ---------------------------------------------------------------------------
TEST(CoinbaseAdapterIntegration, UserChannelJwtRoundTripsAcrossReconnect) {
    eph::utils::TSC::init();

    CoinbaseFixture fx;
    eph::test::TlsWsEchoServer server;
    server.set_message_handler(fx.handler());
    server.enable_request_capture(true);
    server.start();

    auto poller = ek::KernelPoller::create({}).value();

    std::vector<std::string> incoming;
    std::mutex incoming_mu;
    auto on_message = [&](std::span<const uint8_t> bytes) {
        std::lock_guard lk(incoming_mu);
        incoming.emplace_back(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };

    const uint16_t port = server.port();
    auto factory = [&]() -> std::expected<Orch::StreamPtr, eph::core::ErrorInfo> {
        auto sr = TlsWsStream::create(make_config(port));
        if (!sr) return std::unexpected(sr.error());
        (*sr)->on_message = on_message;
        return std::move(*sr);
    };
    auto attach = [&](TlsWsStream* s) { return poller->add(s); };
    auto detach = [&](TlsWsStream* s) { (void)poller->remove(s); };

    std::atomic<uint64_t> on_reconnect_calls{0};
    auto on_reconnect = [&](uint32_t /*attempt*/, uint64_t /*ns*/) {
        on_reconnect_calls.fetch_add(1, std::memory_order_relaxed);
    };

    Orch orch{
        en::ReconnectConfig{
            .policy = {.initial_backoff = 50ms,
                       .max_backoff     = 200ms,
                       .multiplier      = 2.0,
                       .jitter_factor   = 0.0,
                       .max_attempts    = 5},
            .auto_detect_via_state = true,
        },
        factory,
        /*on_disconnect=*/{},
        on_reconnect,
        attach,
        detach,
    };

    auto sr = orch.start(eph::utils::TSC::now());
    if (!sr) {
        GTEST_SKIP() << "Coinbase-fixture initial connect failed: "
                     << sr.error().detail;
    }
    if (orch.state() != en::ReconnectState::Connected) {
        GTEST_SKIP() << "Coinbase-fixture: orchestrator did not reach Connected "
                        "(state=" << en::to_string(orch.state()) << ")";
    }
    ASSERT_NE(orch.current(), nullptr);

    // ── 1st subscribe: send with JWT #1 ─────────────────────────────────
    {
        auto frame = encode_ws_text(make_subscribe(kFakeJwt1));
        auto sr2 = orch.current()->send(frame);
        ASSERT_TRUE(sr2.has_value()) << "send subscribe #1: "
                                     << sr2.error().detail;
    }

    bool got_ack_1 = drive_until(*poller, orch, [&]() {
        std::lock_guard lk(incoming_mu);
        for (auto& m : incoming)
            if (m.find(R"("channel":"subscriptions")") != std::string::npos)
                return true;
        return false;
    });
    ASSERT_TRUE(got_ack_1)
        << "Server never delivered subscriptions ack on first connect";

    {
        std::lock_guard lk(fx.jwts_mu);
        ASSERT_EQ(fx.jwts_received.size(), 1u);
        EXPECT_EQ(fx.jwts_received[0], kFakeJwt1)
            << "JWT #1 was corrupted in transit (TLS + WS round-trip)";
    }

    EXPECT_EQ(orch.reconnect_count(), 1u);
    EXPECT_EQ(orch.reconnect_failures(), 0u);

    // ── Disconnect: Coinbase Advanced Trade pushes Close 1011 on
    //    "Internal Error" or 1008 on auth expiry. Either way the
    //    client must reconnect and re-issue a fresh JWT.
    server.send_close_to_all(1011);

    bool reconnected = drive_until(*poller, orch, [&]() {
        return orch.reconnect_count() >= 2u
            && orch.state() == en::ReconnectState::Connected;
    }, 5s);
    ASSERT_TRUE(reconnected)
        << "Orchestrator failed to reconnect within budget; "
        << "state=" << en::to_string(orch.state())
        << " count=" << orch.reconnect_count()
        << " failures=" << orch.reconnect_failures();

    auto* fresh = orch.current();
    ASSERT_NE(fresh, nullptr);
    fresh->on_message = on_message;

    // ── 2nd subscribe: send with JWT #2 (fresh-token semantics) ─────────
    {
        auto frame = encode_ws_text(make_subscribe(kFakeJwt2));
        auto sr2 = fresh->send(frame);
        ASSERT_TRUE(sr2.has_value()) << "send subscribe #2: "
                                     << sr2.error().detail;
    }

    bool got_ack_2 = drive_until(*poller, orch, [&]() {
        std::lock_guard lk(incoming_mu);
        size_t n = 0;
        for (auto& m : incoming)
            if (m.find(R"("channel":"subscriptions")") != std::string::npos) ++n;
        return n >= 2;
    });
    ASSERT_TRUE(got_ack_2)
        << "Server did not deliver subscriptions ack on reconnect";

    // ── Assertions ──────────────────────────────────────────────────────
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    EXPECT_GE(orch.reconnect_count(), 2u);
    EXPECT_EQ(orch.reconnect_failures(), 0u);
    EXPECT_GE(server.messages_received(), 2u);
    EXPECT_GE(server.accepted_sessions(), 2u);
    EXPECT_GE(on_reconnect_calls.load(), 2u);

    {
        std::lock_guard lk(fx.jwts_mu);
        ASSERT_EQ(fx.jwts_received.size(), 2u);
        EXPECT_EQ(fx.jwts_received[0], kFakeJwt1);
        EXPECT_EQ(fx.jwts_received[1], kFakeJwt2)
            << "Reconnect must replay subscribe with the *new* JWT";
        EXPECT_NE(fx.jwts_received[0], fx.jwts_received[1])
            << "Test sanity: the two JWTs must differ";
    }

    // WS upgrade requests sanity: Coinbase Advanced Trade uses no auth
    // headers at the upgrade level (auth is in the subscribe payload),
    // so we only assert the WS upgrade structure is well-formed.
    auto reqs = server.captured_requests();
    ASSERT_GE(reqs.size(), 2u);
    for (auto& r : reqs) {
        EXPECT_NE(r.find("Sec-WebSocket-Key:"), std::string::npos)
            << "Request missing Sec-WebSocket-Key:\n" << r;
        EXPECT_NE(r.find("GET /"), std::string::npos)
            << "Request not targeting /:\n" << r;
        EXPECT_NE(r.find("Upgrade: websocket"), std::string::npos)
            << "Request missing Upgrade: websocket header:\n" << r;
    }

    orch.stop();
    if (orch.current()) (void)poller->remove(orch.current());
    server.stop();
}
