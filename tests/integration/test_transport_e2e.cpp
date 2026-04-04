/// @file test_transport_e2e.cpp
/// E2E integration tests for Transport<FakeTcpTransport, RawFramer>.
///
/// Tests the Transport layer with a mock TCP backend (no real network I/O).
/// WebSocket and TLS scenarios are intentionally excluded — this exercises
/// the raw TCP-level transport path only.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/fake_tcp_transport.hpp"
#include "eph/core/transport_errors.hpp"
#include "eph/transport/raw_framer.hpp"
#include "eph/transport/transport.hpp"
#include "eph/transport/transport_types.hpp"

namespace {

using namespace eph::net;
using eph::net::testing::FakeTcpTransport;

/// Concrete Transport type under test: FakeTcp, RawFramer, 512B payload, 64-depth queue.
using TestTransport = Transport<FakeTcpTransport, RawFramer, 512, 64>;

/// Build a minimal TransportConfig suitable for FakeTcpTransport (no TLS, no WS).
TransportConfig make_test_config() {
    TransportConfig cfg;
    cfg.remote_host = "fake.test.local";
    cfg.remote_port = 9999;
    cfg.use_tls     = false;
    cfg.verify_peer = false;
    // Disable auto-reconnect so tests don't hang on disconnect
    cfg.max_reconnect_attempts = 0;
    return cfg;
}

/// Build a TcpFactory that creates a connected FakeTcpTransport.
/// Optionally stores a raw pointer to the underlying fake for test inspection.
TestTransport::TcpFactory make_factory(FakeTcpTransport** out_raw = nullptr) {
    return [out_raw]() -> std::expected<std::unique_ptr<FakeTcpTransport>, std::string> {
        auto tcp = std::make_unique<FakeTcpTransport>();
        auto result = tcp->connect(std::chrono::milliseconds{100});
        if (!result) return std::unexpected(result.error());
        if (out_raw) *out_raw = tcp.get();
        return tcp;
    };
}

// ---------------------------------------------------------------------------
// Test: HappyPath — create transport, verify it's running
// ---------------------------------------------------------------------------
TEST(TransportE2E, HappyPath) {
    auto cfg = make_test_config();
    auto result = TestTransport::create(make_factory(), cfg);

    ASSERT_TRUE(result.has_value())
        << "create() failed: " << result.error().message();

    auto& transport = *result;
    EXPECT_TRUE(transport->is_running());
    EXPECT_EQ(transport->state(), TransportState::kConnected);

    transport->stop();
    EXPECT_FALSE(transport->is_running());
    EXPECT_EQ(transport->state(), TransportState::kStopped);
}

// ---------------------------------------------------------------------------
// Test: FactoryConnectFails — connect() error propagates through create()
// ---------------------------------------------------------------------------
TEST(TransportE2E, FactoryConnectFails) {
    auto cfg = make_test_config();

    // Factory that always fails at connect()
    auto factory = []() -> std::expected<std::unique_ptr<FakeTcpTransport>, std::string> {
        auto tcp = std::make_unique<FakeTcpTransport>();
        tcp->set_connect_error("simulated connect failure");
        auto result = tcp->connect(std::chrono::milliseconds{100});
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    auto result = TestTransport::create(std::move(factory), cfg);
    ASSERT_FALSE(result.has_value());

    const auto& err = result.error();
    EXPECT_EQ(err.code, ConnectionError::kFactoryFailed);
    EXPECT_NE(err.detail.find("simulated connect failure"), std::string::npos)
        << "Expected error detail to contain factory error, got: " << err.detail;
}

// ---------------------------------------------------------------------------
// Test: FactoryReturnsNonEstablished — TCP not established after factory
// ---------------------------------------------------------------------------
TEST(TransportE2E, FactoryReturnsNonEstablished) {
    auto cfg = make_test_config();

    // Factory returns a FakeTcpTransport that is NOT connected
    auto factory = []() -> std::expected<std::unique_ptr<FakeTcpTransport>, std::string> {
        auto tcp = std::make_unique<FakeTcpTransport>();
        // Don't call connect() — state remains Closed
        return tcp;
    };

    auto result = TestTransport::create(std::move(factory), cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ConnectionError::kTcpNotEstablished);
}

// ---------------------------------------------------------------------------
// Test: SendWhenNotConnected — send after stop() returns kNotConnected
// ---------------------------------------------------------------------------
TEST(TransportE2E, SendWhenNotConnected) {
    auto cfg = make_test_config();
    auto result = TestTransport::create(make_factory(), cfg);
    ASSERT_TRUE(result.has_value())
        << "create() failed: " << result.error().message();

    auto& transport = *result;
    transport->stop();

    const uint8_t payload[] = {0x01, 0x02, 0x03};
    EXPECT_EQ(transport->send(payload, sizeof(payload)), SendError::kNotConnected);
    EXPECT_EQ(transport->send_binary(payload, sizeof(payload)), SendError::kNotConnected);
    EXPECT_EQ(transport->send_text("hello"), SendError::kNotConnected);
}

// ---------------------------------------------------------------------------
// Test: SendData — verify data reaches FakeTcpTransport::sent_data()
// ---------------------------------------------------------------------------
TEST(TransportE2E, SendData) {
    FakeTcpTransport* raw_tcp = nullptr;
    auto cfg = make_test_config();
    auto result = TestTransport::create(make_factory(&raw_tcp), cfg);
    ASSERT_TRUE(result.has_value())
        << "create() failed: " << result.error().message();
    ASSERT_NE(raw_tcp, nullptr);

    auto& transport = *result;

    // Send binary data
    const uint8_t binary_payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto send_err = transport->send_binary(binary_payload, sizeof(binary_payload));
    EXPECT_EQ(send_err, SendError::kOk);

    // Send text data
    std::string_view text_payload = "hello world";
    send_err = transport->send_text(text_payload);
    EXPECT_EQ(send_err, SendError::kOk);

    // Give the TX thread time to drain the queue and send data to the fake TCP
    // The TX worker polls the SPSC queue and writes to TCP asynchronously.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    // Verify data arrived at the fake TCP layer.
    // RawFramer passes data through unchanged, so sent_data() should contain
    // the exact payloads (RawFramer::encode is a memcpy).
    const auto& sent = raw_tcp->sent_data();
    ASSERT_GE(sent.size(), 2u)
        << "Expected at least 2 sent packets, got " << sent.size();

    // First packet: binary payload
    EXPECT_EQ(sent[0].size(), sizeof(binary_payload));
    EXPECT_EQ(std::memcmp(sent[0].data(), binary_payload, sizeof(binary_payload)), 0);

    // Second packet: text payload
    EXPECT_EQ(sent[1].size(), text_payload.size());
    EXPECT_EQ(std::memcmp(sent[1].data(), text_payload.data(), text_payload.size()), 0);

    transport->stop();
}

// ---------------------------------------------------------------------------
// Test: SendMessageTooLarge — payload exceeding MaxPayload is rejected
// ---------------------------------------------------------------------------
TEST(TransportE2E, SendMessageTooLarge) {
    auto cfg = make_test_config();
    auto result = TestTransport::create(make_factory(), cfg);
    ASSERT_TRUE(result.has_value())
        << "create() failed: " << result.error().message();

    auto& transport = *result;

    // MaxPayload is 512 — send 513 bytes
    std::vector<uint8_t> oversized(513, 0xAA);
    auto send_err = transport->send(oversized.data(), oversized.size());
    EXPECT_EQ(send_err, SendError::kMessageTooLarge);

    transport->stop();
}

// ---------------------------------------------------------------------------
// Note on recv testing:
// Testing recv() with FakeTcpTransport is non-trivial because the RX worker
// thread busy-polls FakeTcpTransport::poll_rx() in a tight loop. Injecting
// data via inject_rx() would require careful synchronization with the RX
// thread's polling cycle, and the data must pass through the framer's decode
// path. Additionally, the RX thread may spin-consume injected data before the
// test can call recv(). This level of thread coordination complexity is better
// suited for dedicated RX worker unit tests rather than E2E integration tests.
// ---------------------------------------------------------------------------

} // namespace
