/// @file test_tcp_concept.cpp
/// Unit tests for TcpTransport concept and TcpState enum from eph-net.
/// Validates concept constraints at compile time and mock behavior at runtime.

#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "eph/net/tcp_concept.hpp"
#include "eph/net/socket_transport.hpp"

using namespace eph::net;

// ---------------------------------------------------------------------------
// MockTcpSession — satisfies TcpTransport concept
// ---------------------------------------------------------------------------

struct MockTcpSession {
    TcpState current_state = TcpState::Closed;
    std::vector<uint8_t> sent_data;
    std::vector<uint8_t> rx_buffer; // pre-stored data for poll_rx

    auto connect(std::chrono::milliseconds /*timeout*/)
        -> std::expected<void, std::string>
    {
        current_state = TcpState::Established;
        return {};
    }

    auto send(const void* data, size_t len)
        -> std::expected<size_t, std::string>
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        sent_data.insert(sent_data.end(), bytes, bytes + len);
        return len;
    }

    template <typename Callback>
    auto poll_rx(Callback&& cb)
        -> std::expected<uint16_t, std::string>
    {
        if (rx_buffer.empty()) {
            return uint16_t{0};
        }
        auto sz = static_cast<uint16_t>(rx_buffer.size());
        cb(rx_buffer.data(), sz);
        rx_buffer.clear();
        return sz;
    }

    auto close() -> std::expected<void, std::string> {
        current_state = TcpState::Closed;
        return {};
    }

    void reset() noexcept {
        current_state = TcpState::Closed;
        sent_data.clear();
        rx_buffer.clear();
    }

    auto mss() const -> uint16_t { return 1460; }

    auto state() const -> TcpState { return current_state; }

    auto is_established() const -> bool {
        return current_state == TcpState::Established;
    }
};

// ---------------------------------------------------------------------------
// BadTcpSession — intentionally missing poll_rx, must NOT satisfy concept
// ---------------------------------------------------------------------------

struct BadTcpSession {
    auto connect(std::chrono::milliseconds) -> std::expected<void, std::string> { return {}; }
    auto send(const void*, size_t len) -> std::expected<size_t, std::string> { return len; }
    // poll_rx intentionally omitted
    auto close() -> std::expected<void, std::string> { return {}; }
    void reset() noexcept {}
    auto mss() const -> uint16_t { return 1460; }
    auto state() const -> TcpState { return TcpState::Closed; }
    auto is_established() const -> bool { return false; }
};

// ---------------------------------------------------------------------------
// Compile-time concept validation
// ---------------------------------------------------------------------------

static_assert(TcpTransport<MockTcpSession>,
    "MockTcpSession must satisfy TcpTransport");
static_assert(!TcpTransport<BadTcpSession>,
    "BadTcpSession must NOT satisfy TcpTransport (missing poll_rx)");

// ---------------------------------------------------------------------------
// TcpState name tests — exhaustive over all 8 states
// ---------------------------------------------------------------------------

TEST(TcpState, NameClosed)      { EXPECT_STREQ(tcp_state_name(TcpState::Closed),      "CLOSED"); }
TEST(TcpState, NameSynSent)     { EXPECT_STREQ(tcp_state_name(TcpState::SynSent),     "SYN_SENT"); }
TEST(TcpState, NameEstablished) { EXPECT_STREQ(tcp_state_name(TcpState::Established), "ESTABLISHED"); }
TEST(TcpState, NameFinWait1)    { EXPECT_STREQ(tcp_state_name(TcpState::FinWait1),    "FIN_WAIT_1"); }
TEST(TcpState, NameFinWait2)    { EXPECT_STREQ(tcp_state_name(TcpState::FinWait2),    "FIN_WAIT_2"); }
TEST(TcpState, NameTimeWait)    { EXPECT_STREQ(tcp_state_name(TcpState::TimeWait),    "TIME_WAIT"); }
TEST(TcpState, NameCloseWait)   { EXPECT_STREQ(tcp_state_name(TcpState::CloseWait),   "CLOSE_WAIT"); }
TEST(TcpState, NameLastAck)     { EXPECT_STREQ(tcp_state_name(TcpState::LastAck),     "LAST_ACK"); }

// ---------------------------------------------------------------------------
// MockTcpSession runtime behavior tests
// ---------------------------------------------------------------------------

TEST(MockTcpSession, SendCapturesData) {
    MockTcpSession session;
    session.current_state = TcpState::Established;

    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto result = session.send(payload, sizeof(payload));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, sizeof(payload));
    ASSERT_EQ(session.sent_data.size(), sizeof(payload));
    EXPECT_EQ(session.sent_data[0], 0xDE);
    EXPECT_EQ(session.sent_data[1], 0xAD);
    EXPECT_EQ(session.sent_data[2], 0xBE);
    EXPECT_EQ(session.sent_data[3], 0xEF);
}

TEST(MockTcpSession, PollRxDeliversStoredData) {
    MockTcpSession session;
    session.rx_buffer = {0x01, 0x02, 0x03};

    std::vector<uint8_t> received;
    auto result = session.poll_rx([&](const uint8_t* data, uint16_t len) {
        received.assign(data, data + len);
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 3);
    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0], 0x01);
    EXPECT_EQ(received[1], 0x02);
    EXPECT_EQ(received[2], 0x03);
    // Buffer should be cleared after delivery
    EXPECT_TRUE(session.rx_buffer.empty());
}

TEST(MockTcpSession, PollRxEmptyReturnsZero) {
    MockTcpSession session;

    bool callback_invoked = false;
    auto result = session.poll_rx([&](const uint8_t*, uint16_t) {
        callback_invoked = true;
    });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);
    EXPECT_FALSE(callback_invoked);
}

TEST(MockTcpSession, ConnectTransitionsFromClosedToEstablished) {
    MockTcpSession session;
    ASSERT_EQ(session.state(), TcpState::Closed);
    ASSERT_FALSE(session.is_established());

    auto result = session.connect(std::chrono::milliseconds{1000});
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(session.state(), TcpState::Established);
    EXPECT_TRUE(session.is_established());
}

TEST(MockTcpSession, CloseReturnsToClosedState) {
    MockTcpSession session;
    session.current_state = TcpState::Established;

    auto result = session.close();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(session.state(), TcpState::Closed);
}

TEST(MockTcpSession, ResetIsNoexceptAndClears) {
    static_assert(noexcept(std::declval<MockTcpSession&>().reset()),
        "reset() must be noexcept");

    MockTcpSession session;
    session.current_state = TcpState::Established;
    session.sent_data = {0x01};
    session.rx_buffer = {0x02};

    session.reset();

    EXPECT_EQ(session.state(), TcpState::Closed);
    EXPECT_TRUE(session.sent_data.empty());
    EXPECT_TRUE(session.rx_buffer.empty());
}

TEST(MockTcpSession, MssReturns1460) {
    MockTcpSession session;
    EXPECT_EQ(session.mss(), 1460);
}

// ---------------------------------------------------------------------------
// TransportConfig validation tests
// ---------------------------------------------------------------------------

#include "eph/net/transport.hpp"

using eph::net::TransportConfig;

TEST(TransportConfigValidate, DefaultWithHostIsValid) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TransportConfigValidate, EmptyHostIsInvalid) {
    TransportConfig cfg;
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("remote_host"), std::string_view::npos);
}

TEST(TransportConfigValidate, ZeroPortIsInvalid) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.remote_port = 0;
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("remote_port"), std::string_view::npos);
}

TEST(TransportConfigValidate, EmptyWsPathIsInvalid) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.ws_path = "";
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("ws_path"), std::string_view::npos);
}

TEST(TransportConfigValidate, ZeroBurstSizeIsInvalid) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.tx_burst_size = 0;
    EXPECT_FALSE(cfg.validate().empty());

    cfg.tx_burst_size = 32;
    cfg.rx_burst_size = 0;
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(TransportConfigValidate, NegativeReconnectAttemptsIsInvalid) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.max_reconnect_attempts = -1;
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(TransportConfigValidate, MaxReconnectBackoffDefaultsToZero) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    // max_reconnect_backoff defaults to 0 (meaning 16x base)
    EXPECT_EQ(cfg.max_reconnect_backoff.count(), 0);
    // Config should still be valid
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TransportConfigValidate, ExplicitMaxReconnectBackoffIsValid) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.max_reconnect_backoff = std::chrono::milliseconds{30000};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TransportConfigValidate, ZeroTimeoutsAreInvalid) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";

    cfg.tcp_timeout = std::chrono::milliseconds{0};
    EXPECT_FALSE(cfg.validate().empty());

    cfg.tcp_timeout = std::chrono::milliseconds{3000};
    cfg.tls_timeout = std::chrono::milliseconds{0};
    EXPECT_FALSE(cfg.validate().empty());

    cfg.tls_timeout = std::chrono::milliseconds{5000};
    cfg.ws_timeout = std::chrono::milliseconds{0};
    EXPECT_FALSE(cfg.validate().empty());
}

// ---------------------------------------------------------------------------
// std::formatter specializations
// ---------------------------------------------------------------------------

TEST(Formatter, TcpStateFormatsCorrectly) {
    EXPECT_EQ(std::format("{}", TcpState::Closed), "CLOSED");
    EXPECT_EQ(std::format("{}", TcpState::SynSent), "SYN_SENT");
    EXPECT_EQ(std::format("{}", TcpState::Established), "ESTABLISHED");
    EXPECT_EQ(std::format("{}", TcpState::FinWait1), "FIN_WAIT_1");
    EXPECT_EQ(std::format("{}", TcpState::FinWait2), "FIN_WAIT_2");
    EXPECT_EQ(std::format("{}", TcpState::TimeWait), "TIME_WAIT");
    EXPECT_EQ(std::format("{}", TcpState::CloseWait), "CLOSE_WAIT");
    EXPECT_EQ(std::format("{}", TcpState::LastAck), "LAST_ACK");
}

TEST(Formatter, TransportEventFormatsCorrectly) {
    EXPECT_EQ(std::format("{}", TransportEvent::kConnected), "CONNECTED");
    EXPECT_EQ(std::format("{}", TransportEvent::kDisconnected), "DISCONNECTED");
    EXPECT_EQ(std::format("{}", TransportEvent::kReconnecting), "RECONNECTING");
    EXPECT_EQ(std::format("{}", TransportEvent::kStopped), "STOPPED");
}

TEST(Formatter, TransportStateFormatsCorrectly) {
    EXPECT_EQ(std::format("{}", TransportState::kConnected), "CONNECTED");
    EXPECT_EQ(std::format("{}", TransportState::kReconnecting), "RECONNECTING");
    EXPECT_EQ(std::format("{}", TransportState::kStopped), "STOPPED");
}

// Verify Transport exposes tls_version() and cipher_name() APIs.
// Full integration test requires a live TLS server; here we verify
// the API compiles and the default Transport type aliases exist.
TEST(TransportAPI, ConnectionMetadataAccessorsExist) {
    // SocketWssTransport is Transport<SocketTransport, 512, 1024>
    using T = eph::net::SocketWssTransport;
    // Verify the methods exist on the type (via member pointer)
    [[maybe_unused]] auto fn1 = &T::tls_version;
    [[maybe_unused]] auto fn2 = &T::cipher_name;
}

TEST(TransportStatsDump, MultiLineOutput) {
    TransportStats stats{};
    stats.tx_packets = 50;
    stats.tx_bytes = 2500;
    stats.rx_packets = 40;
    stats.rx_bytes = 2000;
    stats.tx_dropped = 3;
    stats.encrypt_errors = 1;
    stats.decrypt_errors = 2;
    stats.queue_full_count = 5;
    stats.ws_pings_received = 10;
    stats.ws_pongs_sent = 10;
    stats.reconnect_count = 1;

    auto d = stats.dump();
    EXPECT_NE(d.find("TransportStats:"), std::string::npos);
    EXPECT_NE(d.find("50 packets"), std::string::npos);
    EXPECT_NE(d.find("2500 bytes"), std::string::npos);
    EXPECT_NE(d.find("3 dropped"), std::string::npos);
    EXPECT_NE(d.find("Queue full: 5"), std::string::npos);
    EXPECT_NE(d.find("10 pings received"), std::string::npos);
    EXPECT_NE(d.find("Reconnections: 1"), std::string::npos);
}

TEST(Formatter, TransportStatsFormatsCorrectly) {
    TransportStats stats{};
    stats.tx_packets = 100;
    stats.tx_bytes = 5000;
    stats.rx_packets = 80;
    stats.rx_bytes = 4000;
    stats.reconnect_count = 1;

    auto s = std::format("{}", stats);
    EXPECT_NE(s.find("100"), std::string::npos);   // tx_packets
    EXPECT_NE(s.find("5000"), std::string::npos);   // tx_bytes
    EXPECT_NE(s.find("80"), std::string::npos);     // rx_packets
    EXPECT_NE(s.find("4000"), std::string::npos);   // rx_bytes
    EXPECT_NE(s.find("reconnect:1"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::validate()
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigValidation, ValidConfigPasses) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.remote_port = 443;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TransportConfigValidation, EmptyHostFails) {
    TransportConfig cfg;
    cfg.remote_port = 443;
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(TransportConfigValidation, ZeroReconnectIntervalWithAutoReconnectFails) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.max_reconnect_attempts = 5;
    cfg.reconnect_interval = std::chrono::milliseconds{0};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("reconnect_interval"), std::string_view::npos);
}

TEST(TransportConfigValidation, ZeroReconnectIntervalWithDisabledAutoReconnectPasses) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.max_reconnect_attempts = 0; // auto-reconnect disabled
    cfg.reconnect_interval = std::chrono::milliseconds{0};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TransportConfigValidation, NegativePingIntervalFails) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.ping_interval = std::chrono::seconds{-1};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("ping_interval"), std::string_view::npos);
}

TEST(TransportConfigValidation, ZeroPingIntervalPasses) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.ping_interval = std::chrono::seconds{0}; // disables ping
    EXPECT_TRUE(cfg.validate().empty());
}
