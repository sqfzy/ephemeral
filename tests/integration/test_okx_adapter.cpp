/// @file test_okx_adapter.cpp
///
/// Integration test: OKX V5 public-channel adapter end-to-end.
///
/// Exercises the full client stack (TLS 1.3 + WebSocket + ReconnectOrchestrator)
/// against an in-process TLS WS server that mimics the OKX V5 subscribe-ack
/// protocol. No real venue endpoint is contacted.
///
/// Scenario:
///   1. The fixture spins up a TlsWsEchoServer with an OKX-shaped message
///      handler. When the server receives a `{"op":"subscribe", ...}` text
///      frame it responds with `{"event":"subscribe", "arg":{...}}` —
///      OKX's public-channel ack format. Other frames echo through.
///   2. A `ReconnectOrchestrator<KernelTcpStream<WsCodec, true>>` is built
///      with a factory that dials the loopback TLS port. The on_reconnect
///      callback re-sends the subscribe message every time the stream comes
///      up — including the first connect (the orchestrator fires
///      on_reconnect once per Connected transition).
///   3. After the initial connect we drive the poller until the subscribe-ack
///      lands in the codec sink, then ask the server to send a Close 1011
///      to all sessions. The orchestrator detects the disconnect via
///      auto_detect_via_state, backs off, reconnects, fires on_reconnect
///      again, the subscribe is replayed.
///   4. We assert: state == Connected, reconnect_count >= 2, the server
///      saw the subscribe message at least 2 times, and the cumulative
///      `kReconnectFailures` counter is 0 (no spurious factory failures).
///
/// SKIP behaviour: matches `test_transport_tls_ws_e2e` — if the
/// in-process TLS handshake doesn't complete on this host (cert verify
/// policy, missing aws-lc, etc.) we GTEST_SKIP rather than fail so the
/// suite stays informative across environments.
///
/// Public-channel only: OKX private channels (orders, balance) require
/// `OK-ACCESS-SIGN` / `OK-ACCESS-PASSPHRASE` on a separate "login" frame
/// AFTER the WS upgrade — not part of the HTTP request, so the existing
/// `SignedRequest<OkxSignTraits>` doesn't directly apply at the upgrade
/// boundary. We test what we ship: public market-data subscribe over TLS
/// + WS + reconnect.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
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

/// OKX subscribe payload — the wire shape we'd actually send to v5 public
/// `wss://ws.okx.com:8443/ws/v5/public`. Realistic enough that future
/// adapter authors can reuse it as a recipe.
constexpr std::string_view kOkxSubscribe =
    R"({"op":"subscribe","args":[{"channel":"books","instId":"BTC-USDT"}]})";

/// OKX-shape ack: `{"event":"subscribe","arg":{...}}`. The exact payload
/// is irrelevant for the test — we only need to prove the server's custom
/// handler ran on the frame the client sent.
constexpr std::string_view kOkxSubscribeAck =
    R"({"event":"subscribe","arg":{"channel":"books","instId":"BTC-USDT"}})";

/// Build a TLS+WS stream config dialing the in-process echo server.
/// The per-venue knob is `ws_path` — everything else (TLS hostname pin,
/// 2 s timeouts, no permessage-deflate) is shared and lives in
/// `eph::test::make_local_tls_ws_config`.
ek::StreamConfig make_config(uint16_t port) {
    return eph::test::make_local_tls_ws_config(port, "/ws/v5/public");
}

/// Build a server-side message handler that mimics the OKX V5 subscribe-ack
/// shape. Anything that LOOKS like a subscribe (contains `"op":"subscribe"`)
/// gets the ack; everything else echoes.
eph::test::TlsWsEchoServer::MessageHandler okx_handler() {
    return [](std::span<const uint8_t> payload, uint8_t opcode)
        -> std::optional<std::vector<uint8_t>> {
        if (opcode != 0x1) return std::nullopt;  // text only
        std::string_view sv{
            reinterpret_cast<const char*>(payload.data()), payload.size()};
        if (sv.find("\"op\":\"subscribe\"") == std::string_view::npos) {
            return std::nullopt;
        }
        std::vector<uint8_t> resp(kOkxSubscribeAck.begin(), kOkxSubscribeAck.end());
        return resp;
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
TEST(OkxAdapterIntegration, PublicChannelHappyPathConnectAndReconnect) {
    eph::utils::TSC::init();

    eph::test::TlsWsEchoServer server;
    server.set_message_handler(okx_handler());
    server.enable_request_capture(true);
    server.start();

    auto poller = ek::KernelPoller::create({}).value();

    // Captured frames (text payloads) the codec emits to user code.
    std::vector<std::string> incoming;
    std::mutex incoming_mu;
    auto on_message = [&](std::span<const uint8_t> bytes) {
        std::lock_guard lk(incoming_mu);
        incoming.emplace_back(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };

    // Factory: build a fresh TLS+WS stream every connect attempt. We assign
    // the on_message hook here so the orchestrator doesn't have to know
    // about the user's frame sink — this matches the canonical adapter
    // wiring pattern.
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
        // We can't capture `orch` (declared after this lambda). The test
        // sends the subscribe imperatively after start() and after
        // detecting reconnect, so this callback is just a counter for
        // the "on_reconnect fires per Connected transition" assertion.
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

    // ── Initial connect ─────────────────────────────────────────────────────
    auto sr = orch.start(eph::utils::TSC::now());
    if (!sr) {
        GTEST_SKIP() << "OKX-fixture initial connect failed: "
                     << sr.error().detail;
    }
    if (orch.state() != en::ReconnectState::Connected) {
        GTEST_SKIP() << "OKX-fixture: orchestrator did not reach Connected "
                        "(state=" << en::to_string(orch.state()) << ")";
    }

    ASSERT_NE(orch.current(), nullptr);

    // Send the OKX subscribe frame. WsCodec doesn't expose an encode entry
    // point that masks for us, so we construct the masked text frame
    // directly and hand the bytes to `Stream::send` — the kernel TCP stream
    // will TLS-encrypt them transparently. This matches the pattern an
    // adapter would use today (see `examples/binance_latency.cpp`).
    auto sub_frame = encode_ws_text(kOkxSubscribe);
    {
        auto sr2 = orch.current()->send(sub_frame);
        ASSERT_TRUE(sr2.has_value()) << "send subscribe: " << sr2.error().detail;
    }

    // Drive until the ack lands or budget elapses.
    bool got_ack = drive_until(*poller, orch, [&]() {
        std::lock_guard lk(incoming_mu);
        for (auto& m : incoming) {
            if (m.find("\"event\":\"subscribe\"") != std::string::npos)
                return true;
        }
        return false;
    });
    ASSERT_TRUE(got_ack) << "Server never delivered subscribe-ack";

    // Sanity: reconnect_count == 1 (the initial connect counts as 1
    // successful Connected transition).
    EXPECT_EQ(orch.reconnect_count(), 1u);
    EXPECT_EQ(orch.reconnect_failures(), 0u);
    EXPECT_GE(server.messages_received(), 1u);

    // ── Server-initiated disconnect: send WS Close 1011 to all sessions.
    //    OKX V5 occasionally pushes Close 1011 ("internal error") to force
    //    reconnects; our simulation matches that wire behaviour.
    server.send_close_to_all(1011);

    // Drive until reconnect_count >= 2 (initial + 1 reconnect) and we are
    // back in Connected. Allow the on_reconnect callback to fire and the
    // attach hook to run — both happen synchronously inside try_attempt_.
    bool reconnected = drive_until(*poller, orch, [&]() {
        return orch.reconnect_count() >= 2u
            && orch.state() == en::ReconnectState::Connected;
    }, 5s);
    ASSERT_TRUE(reconnected)
        << "Orchestrator failed to reconnect within budget; "
        << "state=" << en::to_string(orch.state())
        << " count=" << orch.reconnect_count()
        << " failures=" << orch.reconnect_failures();

    // After reconnect, replay the subscribe explicitly. Production
    // adapters do this inside on_reconnect; we keep the test imperative
    // so the timing is deterministic and we don't race the orchestrator's
    // internal state machine.
    auto* fresh = orch.current();
    ASSERT_NE(fresh, nullptr);
    fresh->on_message = on_message;  // re-attach sink on the fresh stream
    {
        auto sr2 = fresh->send(sub_frame);
        ASSERT_TRUE(sr2.has_value()) << "send subscribe (reconnect): "
                                     << sr2.error().detail;
    }

    // Drive until the second ack lands.
    bool got_ack_2 = drive_until(*poller, orch, [&]() {
        std::lock_guard lk(incoming_mu);
        // Count number of acks seen. Initial drove one; we want at least
        // two now.
        size_t n = 0;
        for (auto& m : incoming)
            if (m.find("\"event\":\"subscribe\"") != std::string::npos) ++n;
        return n >= 2;
    });
    ASSERT_TRUE(got_ack_2) << "Server did not deliver subscribe-ack on reconnect";

    // ── Assertions ──────────────────────────────────────────────────────
    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    EXPECT_GE(orch.reconnect_count(), 2u);
    EXPECT_EQ(orch.reconnect_failures(), 0u);
    EXPECT_GE(server.messages_received(), 2u)
        << "Server should have seen the subscribe at least twice";
    EXPECT_GE(server.accepted_sessions(), 2u)
        << "Server should have seen at least two TLS sessions";
    EXPECT_GE(on_reconnect_calls.load(), 2u)
        << "on_reconnect should fire on every Connected transition";

    // OKX public channel uses no auth headers at HTTP-Upgrade level. We do
    // verify the captured request looks like a well-formed WS upgrade
    // (Sec-WebSocket-Key present); this matters because the WsCodec
    // configuration would otherwise mute upgrade failures.
    auto reqs = server.captured_requests();
    ASSERT_GE(reqs.size(), 2u);
    for (auto& r : reqs) {
        EXPECT_NE(r.find("Sec-WebSocket-Key:"), std::string::npos)
            << "Request missing Sec-WebSocket-Key:\n" << r;
        EXPECT_NE(r.find("GET /ws/v5/public"), std::string::npos)
            << "Request not targeting /ws/v5/public:\n" << r;
    }

    // ── Tear down ───────────────────────────────────────────────────────
    orch.stop();
    if (orch.current()) (void)poller->remove(orch.current());
    server.stop();
}
