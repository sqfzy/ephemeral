/// @file test_transport_types.cpp
/// Unit tests for transport_types.hpp: SendError, TransportConfig::validate(),
/// TransportStats helpers, enum formatters, and close code validation.

#include <format>
#include <string>

#include <gtest/gtest.h>

#include "eph/net/transport_types.hpp"
#include "eph/net/websocket.hpp"

using namespace eph::net;

// ─────────────────────────────────────────────────────────────────────────────
// SendError
// ─────────────────────────────────────────────────────────────────────────────

TEST(SendError, NameCoversAllVariants) {
    EXPECT_STREQ(send_error_name(SendError::kOk), "OK");
    EXPECT_STREQ(send_error_name(SendError::kMessageTooLarge), "MESSAGE_TOO_LARGE");
    EXPECT_STREQ(send_error_name(SendError::kNotConnected), "NOT_CONNECTED");
    EXPECT_STREQ(send_error_name(SendError::kQueueFull), "QUEUE_FULL");
    EXPECT_STREQ(send_error_name(SendError::kInvalidUtf8), "INVALID_UTF8");
    EXPECT_STREQ(send_error_name(SendError::kInvalidCloseCode), "INVALID_CLOSE_CODE");
}

TEST(SendError, BangOperatorReturnsTrueOnFailure) {
    EXPECT_FALSE(!SendError::kOk);
    EXPECT_TRUE(!SendError::kMessageTooLarge);
    EXPECT_TRUE(!SendError::kNotConnected);
    EXPECT_TRUE(!SendError::kQueueFull);
    EXPECT_TRUE(!SendError::kInvalidUtf8);
    EXPECT_TRUE(!SendError::kInvalidCloseCode);
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::validate()
// ─────────────────────────────────────────────────────────────────────────────

class TransportConfigValidateTest : public ::testing::Test {
protected:
    TransportConfig valid_config() {
        TransportConfig cfg;
        cfg.remote_host = "example.com";
        cfg.remote_port = 443;
        cfg.ws_path = "/ws";
        return cfg;
    }
};

TEST_F(TransportConfigValidateTest, ValidConfigPassesValidation) {
    auto cfg = valid_config();
    EXPECT_TRUE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, EmptyHostFails) {
    auto cfg = valid_config();
    cfg.remote_host = "";
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("remote_host"), std::string_view::npos);
}

TEST_F(TransportConfigValidateTest, ZeroPortFails) {
    auto cfg = valid_config();
    cfg.remote_port = 0;
    EXPECT_FALSE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, EmptyWsPathFails) {
    auto cfg = valid_config();
    cfg.ws_path = "";
    EXPECT_FALSE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, WsPathMustStartWithSlash) {
    auto cfg = valid_config();
    cfg.ws_path = "ws";
    EXPECT_FALSE(cfg.validate().empty());

    cfg.ws_path = "/ws";
    EXPECT_TRUE(cfg.validate().empty());

    cfg.ws_path = "/";
    EXPECT_TRUE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, ZeroBurstSizeFails) {
    auto cfg = valid_config();
    cfg.tx_burst_size = 0;
    EXPECT_FALSE(cfg.validate().empty());

    cfg = valid_config();
    cfg.rx_burst_size = 0;
    EXPECT_FALSE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, NegativeReconnectAttemptsFails) {
    auto cfg = valid_config();
    cfg.max_reconnect_attempts = -1;
    EXPECT_FALSE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, ZeroTimeoutFails) {
    auto cfg = valid_config();
    cfg.tcp_timeout = std::chrono::milliseconds{0};
    EXPECT_FALSE(cfg.validate().empty());

    cfg = valid_config();
    cfg.tls_timeout = std::chrono::milliseconds{0};
    EXPECT_FALSE(cfg.validate().empty());

    cfg = valid_config();
    cfg.ws_timeout = std::chrono::milliseconds{0};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, ReconnectIntervalMustBePositiveWhenEnabled) {
    auto cfg = valid_config();
    cfg.max_reconnect_attempts = 5;
    cfg.reconnect_interval = std::chrono::milliseconds{0};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, DisabledReconnectAllowsZeroInterval) {
    auto cfg = valid_config();
    cfg.max_reconnect_attempts = 0;
    cfg.reconnect_interval = std::chrono::milliseconds{0};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, NegativePingIntervalFails) {
    auto cfg = valid_config();
    cfg.ping_interval = std::chrono::seconds{-1};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, PongTimeoutRequiresPingInterval) {
    auto cfg = valid_config();
    cfg.pong_timeout = std::chrono::seconds{5};
    cfg.ping_interval = std::chrono::seconds{0};
    EXPECT_FALSE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, PongTimeoutWithPingIntervalPasses) {
    auto cfg = valid_config();
    cfg.pong_timeout = std::chrono::seconds{5};
    cfg.ping_interval = std::chrono::seconds{30};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, ExtraHeadersMustEndWithCRLF) {
    auto cfg = valid_config();
    cfg.extra_headers = "X-Custom: value";
    EXPECT_FALSE(cfg.validate().empty());

    cfg.extra_headers = "X-Custom: value\r\n";
    EXPECT_TRUE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, EmptyExtraHeadersPasses) {
    auto cfg = valid_config();
    cfg.extra_headers = "";
    EXPECT_TRUE(cfg.validate().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportStats helpers
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportStats, RateHelpersReturnZeroOnZeroUptime) {
    TransportStats stats{};
    stats.tx_packets = 100;
    stats.rx_packets = 200;
    stats.tx_bytes = 1000;
    stats.rx_bytes = 2000;
    stats.uptime_ns = 0;

    EXPECT_DOUBLE_EQ(stats.tx_pps(), 0.0);
    EXPECT_DOUBLE_EQ(stats.rx_pps(), 0.0);
    EXPECT_DOUBLE_EQ(stats.tx_bps(), 0.0);
    EXPECT_DOUBLE_EQ(stats.rx_bps(), 0.0);
}

TEST(TransportStats, RateHelpersComputeCorrectly) {
    TransportStats stats{};
    stats.tx_packets = 100;
    stats.rx_packets = 200;
    stats.tx_bytes = 1000;
    stats.rx_bytes = 2000;
    stats.uptime_ns = 1'000'000'000;  // 1 second

    EXPECT_DOUBLE_EQ(stats.tx_pps(), 100.0);
    EXPECT_DOUBLE_EQ(stats.rx_pps(), 200.0);
    EXPECT_DOUBLE_EQ(stats.tx_bps(), 1000.0);
    EXPECT_DOUBLE_EQ(stats.rx_bps(), 2000.0);
}

TEST(TransportStats, HandshakeMsConversion) {
    TransportStats stats{};
    stats.handshake_ns = 5'000'000;  // 5ms
    EXPECT_NEAR(stats.handshake_ms(), 5.0, 0.001);
}

TEST(TransportStats, UptimeDuration) {
    TransportStats stats{};
    stats.uptime_ns = 1'500'000'000;
    EXPECT_EQ(stats.uptime(), std::chrono::nanoseconds{1'500'000'000});
}

TEST(TransportStats, DumpProducesNonEmptyString) {
    TransportStats stats{};
    stats.tx_packets = 10;
    stats.uptime_ns = 1'000'000'000;
    auto dump = stats.dump();
    EXPECT_FALSE(dump.empty());
    EXPECT_NE(dump.find("TransportStats"), std::string::npos);
    EXPECT_NE(dump.find("TX:"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportTypeFormatters, SendErrorFormats) {
    EXPECT_EQ(std::format("{}", SendError::kOk), "OK");
    EXPECT_EQ(std::format("{}", SendError::kMessageTooLarge), "MESSAGE_TOO_LARGE");
    EXPECT_EQ(std::format("{}", SendError::kNotConnected), "NOT_CONNECTED");
    EXPECT_EQ(std::format("{}", SendError::kQueueFull), "QUEUE_FULL");
    EXPECT_EQ(std::format("{}", SendError::kInvalidUtf8), "INVALID_UTF8");
    EXPECT_EQ(std::format("{}", SendError::kInvalidCloseCode), "INVALID_CLOSE_CODE");
}

TEST(TransportTypeFormatters, TransportEventFormats) {
    EXPECT_EQ(std::format("{}", TransportEvent::kConnected), "CONNECTED");
    EXPECT_EQ(std::format("{}", TransportEvent::kDisconnected), "DISCONNECTED");
    EXPECT_EQ(std::format("{}", TransportEvent::kReconnecting), "RECONNECTING");
    EXPECT_EQ(std::format("{}", TransportEvent::kStopped), "STOPPED");
}

TEST(TransportTypeFormatters, TransportStateFormats) {
    EXPECT_EQ(std::format("{}", TransportState::kConnected), "CONNECTED");
    EXPECT_EQ(std::format("{}", TransportState::kReconnecting), "RECONNECTING");
    EXPECT_EQ(std::format("{}", TransportState::kStopped), "STOPPED");
}

TEST(TransportTypeFormatters, TransportStatsFormats) {
    TransportStats stats{};
    stats.tx_packets = 42;
    stats.rx_bytes = 1024;
    auto formatted = std::format("{}", stats);
    EXPECT_NE(formatted.find("42"), std::string::npos);
    EXPECT_NE(formatted.find("1024"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket close code validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(WebSocketCloseCode, StandardCodesAreValid) {
    EXPECT_TRUE(ws::is_valid_close_code(1000));
    EXPECT_TRUE(ws::is_valid_close_code(1001));
    EXPECT_TRUE(ws::is_valid_close_code(1002));
    EXPECT_TRUE(ws::is_valid_close_code(1003));
    EXPECT_TRUE(ws::is_valid_close_code(1007));
    EXPECT_TRUE(ws::is_valid_close_code(1008));
    EXPECT_TRUE(ws::is_valid_close_code(1009));
    EXPECT_TRUE(ws::is_valid_close_code(1010));
    EXPECT_TRUE(ws::is_valid_close_code(1011));
}

TEST(WebSocketCloseCode, ReservedCodesAreInvalid) {
    // 1004, 1005, 1006 are reserved and must not be sent
    EXPECT_FALSE(ws::is_valid_close_code(1004));
    EXPECT_FALSE(ws::is_valid_close_code(1005));
    EXPECT_FALSE(ws::is_valid_close_code(1006));
    // 1015 is reserved (TLS handshake failure)
    EXPECT_FALSE(ws::is_valid_close_code(1015));
}

TEST(WebSocketCloseCode, OutOfRangeCodesAreInvalid) {
    EXPECT_FALSE(ws::is_valid_close_code(0));
    EXPECT_FALSE(ws::is_valid_close_code(999));
    EXPECT_FALSE(ws::is_valid_close_code(1012));
    EXPECT_FALSE(ws::is_valid_close_code(2999));
    EXPECT_FALSE(ws::is_valid_close_code(5000));
    EXPECT_FALSE(ws::is_valid_close_code(65535));
}

TEST(WebSocketCloseCode, RegisteredAndPrivateRangesAreValid) {
    EXPECT_TRUE(ws::is_valid_close_code(3000));
    EXPECT_TRUE(ws::is_valid_close_code(3999));
    EXPECT_TRUE(ws::is_valid_close_code(4000));
    EXPECT_TRUE(ws::is_valid_close_code(4999));
}
