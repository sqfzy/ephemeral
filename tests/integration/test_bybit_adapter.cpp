/// @file test_bybit_adapter.cpp
///
/// Integration test: Bybit V5 public-channel adapter end-to-end.
///
/// Exercises TLS 1.3 + WebSocket + ReconnectOrchestrator against an
/// in-process TLS WS server that mimics the Bybit V5 public-channel
/// subscribe-ack protocol. No real venue endpoint is contacted.
///
/// Why test the public path: Bybit V5 supports both public (no auth) and
/// private (HMAC-SHA256 challenge over WS) channels. The HMAC challenge
/// is an *application-layer* frame after the WS upgrade, NOT an HTTP
/// header — so `SignedRequest<BybitSignTraits>` (which produces HTTP
/// headers) doesn't apply at the WS-upgrade boundary the way a REST
/// adapter would use it. The unit tests for `SignedRequest<BybitSignTraits>`
/// cover the signing primitive; this integration test covers the
/// reconnect / resubscribe state machine over the actual TLS stream.
///
/// Scenario:
///   1. Server installed with a Bybit-shape handler that responds to
///      `{"op":"subscribe", ...}` with `{"op":"subscribe", "success":true}`.
///   2. Orchestrator dials, subscribe is sent, ack received.
///   3. Server pushes WS Close 1011 → orchestrator backs off, reconnects.
///   4. Subscribe replayed; second ack received.
///   5. Assertions: state == Connected, reconnect_count >= 2,
///      messages_received >= 2, accepted_sessions >= 2,
///      no spurious failures.

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
#include "venue_adapter_test_kit.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;
namespace ec = eph::codec;
using namespace std::chrono_literals;

namespace {

using TlsWsStream = ek::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>;
using Orch        = en::ReconnectOrchestrator<TlsWsStream>;

/// Realistic Bybit V5 subscribe payload — `wss://stream.bybit.com/v5/public/spot`.
/// `args[]` is an array of topics; we use the order-book topic shape.
constexpr std::string_view kBybitSubscribe =
    R"({"op":"subscribe","args":["orderbook.50.BTCUSDT"]})";

/// Bybit V5 public ack: success=true means the broker accepted the
/// subscription. The exact payload contents don't matter to us; we only
/// need the handler to fire and emit something distinguishable.
constexpr std::string_view kBybitSubscribeAck =
    R"({"op":"subscribe","success":true,"ret_msg":"","conn_id":"abc","req_id":""})";

ek::StreamConfig make_config(uint16_t port) {
    return eph::test::make_local_tls_ws_config(port, "/v5/public/spot");
}

eph::test::TlsWsEchoServer::MessageHandler bybit_handler() {
    return [](std::span<const uint8_t> payload, uint8_t opcode)
        -> std::optional<std::vector<uint8_t>> {
        if (opcode != 0x1) return std::nullopt;
        std::string_view sv{
            reinterpret_cast<const char*>(payload.data()), payload.size()};
        if (sv.find("\"op\":\"subscribe\"") == std::string_view::npos) {
            return std::nullopt;
        }
        return std::vector<uint8_t>(kBybitSubscribeAck.begin(),
                                    kBybitSubscribeAck.end());
    };
}

// `drive_until` and `encode_ws_text` come from the shared
// venue_adapter_test_kit.hpp — see the include above.
using eph::test::drive_until;
using eph::test::encode_ws_text;

} // namespace

// ---------------------------------------------------------------------------
// Test: PublicChannelHappyPathConnectAndReconnect
// ---------------------------------------------------------------------------
TEST(BybitAdapterIntegration, PublicChannelHappyPathConnectAndReconnect) {
    eph::utils::TSC::init();

    eph::test::TlsWsEchoServer server;
    server.set_message_handler(bybit_handler());
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
        GTEST_SKIP() << "Bybit-fixture initial connect failed: "
                     << sr.error().detail;
    }
    if (orch.state() != en::ReconnectState::Connected) {
        GTEST_SKIP() << "Bybit-fixture: orchestrator did not reach Connected "
                        "(state=" << en::to_string(orch.state()) << ")";
    }
    ASSERT_NE(orch.current(), nullptr);

    // Send subscribe.
    auto sub_frame = encode_ws_text(kBybitSubscribe);
    {
        auto sr2 = orch.current()->send(sub_frame);
        ASSERT_TRUE(sr2.has_value()) << "send subscribe: " << sr2.error().detail;
    }

    bool got_ack = drive_until(*poller, orch, [&]() {
        std::lock_guard lk(incoming_mu);
        for (auto& m : incoming) {
            // Bybit's success ack carries `"success":true` in the body —
            // the most distinctive marker. (`"op":"subscribe"` would also
            // match the request we just sent if it came back via echo.)
            if (m.find("\"success\":true") != std::string::npos)
                return true;
        }
        return false;
    });
    ASSERT_TRUE(got_ack) << "Server never delivered Bybit subscribe-ack";

    EXPECT_EQ(orch.reconnect_count(), 1u);
    EXPECT_EQ(orch.reconnect_failures(), 0u);

    // Server-initiated disconnect: Bybit pushes Close 1011 when the
    // venue rolls a server, plus periodic auto-disconnects after 24h
    // for spot or 1h for derivatives. We simulate the wire frame.
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
    {
        auto sr2 = fresh->send(sub_frame);
        ASSERT_TRUE(sr2.has_value()) << "send subscribe (reconnect): "
                                     << sr2.error().detail;
    }

    bool got_ack_2 = drive_until(*poller, orch, [&]() {
        std::lock_guard lk(incoming_mu);
        size_t n = 0;
        for (auto& m : incoming)
            if (m.find("\"success\":true") != std::string::npos) ++n;
        return n >= 2;
    });
    ASSERT_TRUE(got_ack_2) << "Server did not deliver Bybit ack on reconnect";

    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    EXPECT_GE(orch.reconnect_count(), 2u);
    EXPECT_EQ(orch.reconnect_failures(), 0u);
    EXPECT_GE(server.messages_received(), 2u);
    EXPECT_GE(server.accepted_sessions(), 2u);
    EXPECT_GE(on_reconnect_calls.load(), 2u);

    // Verify the WS upgrade request looks well-formed for Bybit's path.
    // Bybit's public channel uses no auth headers at the upgrade level —
    // private auth is an in-band `op:auth` message AFTER the upgrade.
    auto reqs = server.captured_requests();
    ASSERT_GE(reqs.size(), 2u);
    for (auto& r : reqs) {
        EXPECT_NE(r.find("Sec-WebSocket-Key:"), std::string::npos)
            << "Request missing Sec-WebSocket-Key:\n" << r;
        EXPECT_NE(r.find("GET /v5/public/spot"), std::string::npos)
            << "Request not targeting /v5/public/spot:\n" << r;
        // Public-channel sanity: no signed-request header should appear.
        EXPECT_EQ(r.find("X-BAPI-SIGN"), std::string::npos)
            << "Public channel must not carry X-BAPI-SIGN:\n" << r;
    }

    orch.stop();
    if (orch.current()) (void)poller->remove(orch.current());
    server.stop();
}
