/// @file test_transport_types.cpp
/// Unit tests for transport_types.hpp: SendError, TransportConfig::validate(),
/// TransportStats helpers, enum formatters, close code validation, and
/// ReceivedMessage close frame accessors.

#include <format>
#include <string>

#include <gtest/gtest.h>

#include "eph/net/transport_types.hpp"
#include "eph/net/websocket.hpp"
#include "eph/net/socket_transport.hpp"

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

TEST_F(TransportConfigValidateTest, SubprotocolWithCRLFFailsHeaderInjection) {
    auto cfg = valid_config();
    cfg.ws_subprotocol = "valid-proto";
    EXPECT_TRUE(cfg.validate().empty());

    // CR injection
    cfg.ws_subprotocol = "proto\r\nX-Injected: evil";
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("header injection"), std::string_view::npos);

    // LF only
    cfg.ws_subprotocol = "proto\nevil";
    EXPECT_FALSE(cfg.validate().empty());

    // CR only
    cfg.ws_subprotocol = "proto\revil";
    EXPECT_FALSE(cfg.validate().empty());

    // Empty subprotocol is fine
    cfg.ws_subprotocol = "";
    EXPECT_TRUE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, MtlsBothPathsMustBeSetTogether) {
    auto cfg = valid_config();

    // Both set = valid
    cfg.client_cert_path = "/path/to/cert.pem";
    cfg.client_key_path = "/path/to/key.pem";
    EXPECT_TRUE(cfg.validate().empty());

    // Only cert = invalid
    cfg.client_key_path = "";
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("client_cert_path"), std::string_view::npos);

    // Only key = invalid
    cfg.client_cert_path = "";
    cfg.client_key_path = "/path/to/key.pem";
    EXPECT_FALSE(cfg.validate().empty());

    // Both empty = valid (no mTLS)
    cfg.client_cert_path = "";
    cfg.client_key_path = "";
    EXPECT_TRUE(cfg.validate().empty());
}

TEST_F(TransportConfigValidateTest, MtlsRequiresTls) {
    auto cfg = valid_config();
    cfg.use_tls = false;
    cfg.client_cert_path = "/path/to/cert.pem";
    cfg.client_key_path = "/path/to/key.pem";
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("use_tls"), std::string_view::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionError enum
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectionErrorTest, NameFormatting) {
    EXPECT_STREQ(connection_error_name(ConnectionError::kInvalidConfig), "INVALID_CONFIG");
    EXPECT_STREQ(connection_error_name(ConnectionError::kFactoryFailed), "FACTORY_FAILED");
    EXPECT_STREQ(connection_error_name(ConnectionError::kTcpNotEstablished), "TCP_NOT_ESTABLISHED");
    EXPECT_STREQ(connection_error_name(ConnectionError::kTlsSessionFailed), "TLS_SESSION_FAILED");
    EXPECT_STREQ(connection_error_name(ConnectionError::kTlsHandshakeFailed), "TLS_HANDSHAKE_FAILED");
    EXPECT_STREQ(connection_error_name(ConnectionError::kTlsKeyExportFailed), "TLS_KEY_EXPORT_FAILED");
    EXPECT_STREQ(connection_error_name(ConnectionError::kWsUpgradeFailed), "WS_UPGRADE_FAILED");
    EXPECT_STREQ(connection_error_name(ConnectionError::kWsUpgradeRejected), "WS_UPGRADE_REJECTED");
    EXPECT_STREQ(connection_error_name(ConnectionError::kWsAcceptInvalid), "WS_ACCEPT_INVALID");
}

TEST(ConnectionErrorTest, InfoMessage) {
    ConnectionErrorInfo info{
        .code = ConnectionError::kWsUpgradeRejected,
        .detail = "HTTP 403 Forbidden",
        .http_status = 403,
    };
    auto msg = info.message();
    EXPECT_NE(msg.find("WS_UPGRADE_REJECTED"), std::string::npos);
    EXPECT_NE(msg.find("403"), std::string::npos);
    EXPECT_EQ(info.http_status, 403);
}

TEST(ConnectionErrorTest, FormatConnectionError) {
    EXPECT_EQ(std::format("{}", ConnectionError::kFactoryFailed), "FACTORY_FAILED");
}

TEST(ConnectionErrorTest, FormatConnectionErrorInfo) {
    ConnectionErrorInfo info{
        .code = ConnectionError::kTlsHandshakeFailed,
        .detail = "cert expired",
    };
    auto formatted = std::format("{}", info);
    EXPECT_NE(formatted.find("TLS_HANDSHAKE_FAILED"), std::string::npos);
    EXPECT_NE(formatted.find("cert expired"), std::string::npos);
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

TEST(TransportStats, DumpIncludesRemoteIp) {
    TransportStats stats{};
    stats.uptime_ns = 1'000'000'000;
    stats.remote_ip = "10.0.0.1";
    auto dump = stats.dump();
    EXPECT_NE(dump.find("10.0.0.1"), std::string::npos);
}

TEST(TransportStats, DumpShowsUnknownForEmptyRemoteIp) {
    TransportStats stats{};
    stats.uptime_ns = 1'000'000'000;
    auto dump = stats.dump();
    EXPECT_NE(dump.find("unknown"), std::string::npos);
}

TEST(TransportStats, ToJsonProducesValidFormat) {
    TransportStats stats{};
    stats.tx_packets = 42;
    stats.rx_bytes = 1024;
    stats.uptime_ns = 2'000'000'000;
    stats.handshake_ns = 5'000'000;
    stats.remote_ip = "192.168.1.1";

    auto json = stats.to_json();
    // Basic JSON structure checks
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"tx_packets\":42"), std::string::npos);
    EXPECT_NE(json.find("\"rx_bytes\":1024"), std::string::npos);
    EXPECT_NE(json.find("\"remote_ip\":\"192.168.1.1\""), std::string::npos);
    EXPECT_NE(json.find("\"handshake_ms\":"), std::string::npos);
    EXPECT_NE(json.find("\"tx_pps\":"), std::string::npos);
}

TEST(TransportStats, ToJsonHandlesEmptyRemoteIp) {
    TransportStats stats{};
    auto json = stats.to_json();
    EXPECT_NE(json.find("\"remote_ip\":\"\""), std::string::npos);
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
// on_reconnect_attempt callback
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigCallbacks, OnReconnectAttemptCallableSignature) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.ws_path = "/ws";

    int captured_attempt = 0;
    std::string captured_error;

    cfg.on_reconnect_attempt = [&](int attempt, int max_attempts,
                                    std::string_view error) -> bool {
        captured_attempt = attempt;
        captured_error = std::string(error);
        // Abort on TLS-related errors
        if (error.find("TLS") != std::string_view::npos) return false;
        return attempt < max_attempts;
    };

    // Simulate a transient error — should continue
    EXPECT_TRUE(cfg.on_reconnect_attempt(1, 5, "Connection timeout"));
    EXPECT_EQ(captured_attempt, 1);
    EXPECT_EQ(captured_error, "Connection timeout");

    // Simulate a TLS error — should abort
    EXPECT_FALSE(cfg.on_reconnect_attempt(2, 5, "TLS handshake failed"));
    EXPECT_EQ(captured_attempt, 2);
}

TEST(TransportConfigCallbacks, OnReconnectAttemptDefaultIsNull) {
    TransportConfig cfg;
    EXPECT_FALSE(cfg.on_reconnect_attempt);
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

// ─────────────────────────────────────────────────────────────────────────────
// RttStats
// ─────────────────────────────────────────────────────────────────────────────

TEST(RttStats, DefaultIsZero) {
    RttStats rtt{};
    EXPECT_EQ(rtt.count, 0u);
    EXPECT_EQ(rtt.min_ns, 0u);
    EXPECT_EQ(rtt.max_ns, 0u);
    EXPECT_DOUBLE_EQ(rtt.mean_ns, 0.0);
    EXPECT_EQ(rtt.p50_ns, 0u);
    EXPECT_EQ(rtt.p99_ns, 0u);
    EXPECT_EQ(rtt.p999_ns, 0u);
}

TEST(RttStats, ConvenienceAccessorsConvertToMicroseconds) {
    RttStats rtt{
        .count = 100,
        .min_ns = 1000,
        .max_ns = 5000000,
        .mean_ns = 500000.0,
        .p50_ns = 400000,
        .p99_ns = 4000000,
        .p999_ns = 4800000,
    };
    EXPECT_DOUBLE_EQ(rtt.p50_us(), 400.0);
    EXPECT_DOUBLE_EQ(rtt.p99_us(), 4000.0);
    EXPECT_DOUBLE_EQ(rtt.mean_us(), 500.0);
}

TEST(RttStats, DumpReturnsNoSamplesWhenEmpty) {
    RttStats rtt{};
    EXPECT_EQ(rtt.dump(), "RttStats: no samples");
}

TEST(RttStats, DumpReturnsFormattedStringWhenPopulated) {
    RttStats rtt{
        .count = 42,
        .min_ns = 1000,
        .max_ns = 5000000,
        .mean_ns = 500000.0,
        .p50_ns = 400000,
        .p99_ns = 4000000,
        .p999_ns = 4800000,
    };
    auto dump = rtt.dump();
    EXPECT_NE(dump.find("42 samples"), std::string::npos);
    EXPECT_NE(dump.find("p50:"), std::string::npos);
    EXPECT_NE(dump.find("p99:"), std::string::npos);
}

TEST(RttStats, FormatterProducesOutput) {
    RttStats rtt{};
    auto s = std::format("{}", rtt);
    EXPECT_NE(s.find("no samples"), std::string::npos);

    rtt.count = 10;
    rtt.p50_ns = 500000;
    rtt.p99_ns = 2000000;
    rtt.max_ns = 5000000;
    auto s2 = std::format("{}", rtt);
    EXPECT_NE(s2.find("p50="), std::string::npos);
    EXPECT_NE(s2.find("p99="), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// ReceivedMessage close frame accessors
// ─────────────────────────────────────────────────────────────────────────────

using RecvMsg = eph::net::SocketWssTransport::ReceivedMessage;

TEST(ReceivedMessage, CloseCodeExtractsStatusCode) {
    RecvMsg msg;
    msg.opcode = ws::opcode::kClose;
    // Close payload: 2-byte big-endian status code + reason
    msg.data = {0x03, 0xE8};  // 1000 = Normal Closure
    EXPECT_TRUE(msg.is_close());
    EXPECT_EQ(msg.close_code(), 1000);
}

TEST(ReceivedMessage, CloseCodeReturnsZeroForNonCloseFrame) {
    RecvMsg msg;
    msg.opcode = ws::opcode::kBinary;
    msg.data = {0x03, 0xE8};
    EXPECT_FALSE(msg.is_close());
    EXPECT_EQ(msg.close_code(), 0);
}

TEST(ReceivedMessage, CloseCodeReturnsZeroForShortPayload) {
    RecvMsg msg;
    msg.opcode = ws::opcode::kClose;
    msg.data = {0x03};  // Only 1 byte — too short
    EXPECT_EQ(msg.close_code(), 0);
}

TEST(ReceivedMessage, CloseCodeReturnsZeroForEmptyPayload) {
    RecvMsg msg;
    msg.opcode = ws::opcode::kClose;
    EXPECT_EQ(msg.close_code(), 0);
}

TEST(ReceivedMessage, CloseReasonExtractsString) {
    RecvMsg msg;
    msg.opcode = ws::opcode::kClose;
    // 1001 = Going Away, reason = "server shutdown"
    msg.data = {0x03, 0xE9, 's', 'e', 'r', 'v', 'e', 'r', ' ',
                's', 'h', 'u', 't', 'd', 'o', 'w', 'n'};
    EXPECT_EQ(msg.close_code(), 1001);
    EXPECT_EQ(msg.close_reason(), "server shutdown");
}

TEST(ReceivedMessage, CloseReasonEmptyWhenNoReasonPresent) {
    RecvMsg msg;
    msg.opcode = ws::opcode::kClose;
    msg.data = {0x03, 0xE8};  // Code only, no reason
    EXPECT_EQ(msg.close_reason(), "");
}

TEST(ReceivedMessage, CloseReasonEmptyForNonCloseFrame) {
    RecvMsg msg;
    msg.opcode = ws::opcode::kText;
    msg.data = {0x03, 0xE8, 'h', 'i'};
    EXPECT_EQ(msg.close_reason(), "");
}

TEST(ReceivedMessage, TextAndBinaryTypeChecks) {
    RecvMsg text_msg;
    text_msg.opcode = ws::opcode::kText;
    text_msg.data = {'h', 'e', 'l', 'l', 'o'};
    EXPECT_TRUE(text_msg.is_text());
    EXPECT_FALSE(text_msg.is_binary());
    EXPECT_FALSE(text_msg.is_close());
    EXPECT_EQ(text_msg.text(), "hello");

    RecvMsg bin_msg;
    bin_msg.opcode = ws::opcode::kBinary;
    EXPECT_FALSE(bin_msg.is_text());
    EXPECT_TRUE(bin_msg.is_binary());
    EXPECT_FALSE(bin_msg.is_close());
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::dump() and to_json()
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigDump, ContainsKeyFields) {
    TransportConfig cfg;
    cfg.remote_host = "ws.example.com";
    cfg.remote_port = 8443;
    cfg.ws_path = "/v2/stream";
    cfg.ws_subprotocol = "graphql-ws";
    cfg.verify_peer = false;
    cfg.tcp_timeout = std::chrono::milliseconds{5000};

    auto dump = cfg.dump();
    EXPECT_NE(dump.find("ws.example.com"), std::string::npos);
    EXPECT_NE(dump.find("8443"), std::string::npos);
    EXPECT_NE(dump.find("/v2/stream"), std::string::npos);
    EXPECT_NE(dump.find("graphql-ws"), std::string::npos);
    EXPECT_NE(dump.find("verify_peer=false"), std::string::npos);
    EXPECT_NE(dump.find("tcp=5000ms"), std::string::npos);
}

TEST(TransportConfigDump, ShowsCallbackStatus) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.ws_path = "/ws";
    cfg.on_message = [](const uint8_t*, uint16_t, uint8_t) {};
    cfg.on_close = [](uint16_t, std::string_view) {};

    auto dump = cfg.dump();
    EXPECT_NE(dump.find("on_message=true"), std::string::npos);
    EXPECT_NE(dump.find("on_close=true"), std::string::npos);
    EXPECT_NE(dump.find("on_state_change=false"), std::string::npos);
}

TEST(TransportConfigDump, DefaultSubprotocolShowsNone) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.ws_path = "/ws";

    auto dump = cfg.dump();
    EXPECT_NE(dump.find("(none)"), std::string::npos);
}

TEST(TransportConfigToJson, ProducesValidJsonStructure) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.remote_port = 443;
    cfg.ws_path = "/ws";
    cfg.max_reconnect_attempts = 5;
    cfg.ping_interval = std::chrono::seconds{15};

    auto json = cfg.to_json();
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"remote_host\":\"example.com\""), std::string::npos);
    EXPECT_NE(json.find("\"remote_port\":443"), std::string::npos);
    EXPECT_NE(json.find("\"ws_path\":\"/ws\""), std::string::npos);
    EXPECT_NE(json.find("\"max_reconnect_attempts\":5"), std::string::npos);
    EXPECT_NE(json.find("\"ping_interval_s\":15"), std::string::npos);
    EXPECT_NE(json.find("\"verify_peer\":true"), std::string::npos);
}

TEST(ReceivedMessage, AllStandardCloseCodesRoundtrip) {
    // Verify various standard close codes encode/decode correctly
    for (uint16_t code : {1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011,
                          3000, 3999, 4000, 4999}) {
        RecvMsg msg;
        msg.opcode = ws::opcode::kClose;
        msg.data = {static_cast<uint8_t>(code >> 8),
                    static_cast<uint8_t>(code & 0xFF)};
        EXPECT_EQ(msg.close_code(), code) << "Failed for code " << code;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON string escaping (detail::json_escape)
// ─────────────────────────────────────────────────────────────────────────────

TEST(JsonEscape, PlainStringUnchanged) {
    auto result = detail::json_escape("hello world");
    EXPECT_EQ(result, "hello world");
}

TEST(JsonEscape, EmptyStringUnchanged) {
    auto result = detail::json_escape("");
    EXPECT_EQ(result, "");
}

TEST(JsonEscape, EscapesDoubleQuotes) {
    auto result = detail::json_escape(R"(say "hello")");
    EXPECT_EQ(result, R"(say \"hello\")");
}

TEST(JsonEscape, EscapesBackslashes) {
    auto result = detail::json_escape(R"(C:\path\to\file)");
    EXPECT_EQ(result, R"(C:\\path\\to\\file)");
}

TEST(JsonEscape, EscapesControlCharacters) {
    auto result = detail::json_escape("line1\nline2\ttab");
    EXPECT_EQ(result, R"(line1\nline2\ttab)");
}

TEST(JsonEscape, EscapesAllSpecialChars) {
    std::string input;
    input += '"';   // double quote
    input += '\\';  // backslash
    input += '\b';  // backspace
    input += '\f';  // form feed
    input += '\n';  // newline
    input += '\r';  // carriage return
    input += '\t';  // tab
    auto result = detail::json_escape(input);
    EXPECT_EQ(result, R"(\"\\\b\f\n\r\t)");
}

TEST(JsonEscape, EscapesLowControlCharsAsUnicode) {
    // NUL (0x00) and SOH (0x01) should become \u0000 and \u0001
    std::string input;
    input += '\x00'; // NUL
    input += '\x01'; // SOH
    input += '\x1F'; // US (last control char)
    auto result = detail::json_escape(std::string_view(input.data(), input.size()));
    EXPECT_NE(result.find("\\u0000"), std::string::npos);
    EXPECT_NE(result.find("\\u0001"), std::string::npos);
    EXPECT_NE(result.find("\\u001f"), std::string::npos);
}

TEST(JsonEscape, PassthroughUtf8) {
    // UTF-8 multibyte characters should pass through unchanged
    auto result = detail::json_escape("Hello \xe4\xb8\x96\xe7\x95\x8c");
    EXPECT_EQ(result, "Hello \xe4\xb8\x96\xe7\x95\x8c");
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::to_json() escaping
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigJson, EscapesSpecialCharsInStringFields) {
    TransportConfig cfg;
    cfg.remote_host = "host\"with\"quotes";
    cfg.remote_port = 443;
    cfg.ws_path = "/path\\with\\backslash";
    cfg.ws_subprotocol = "proto\nnewline";

    auto json = cfg.to_json();

    // Escaped values should appear in JSON
    EXPECT_NE(json.find(R"(host\"with\"quotes)"), std::string::npos);
    EXPECT_NE(json.find(R"(/path\\with\\backslash)"), std::string::npos);
    EXPECT_NE(json.find(R"(proto\nnewline)"), std::string::npos);

    // Raw unescaped values should NOT appear
    EXPECT_EQ(json.find("host\"with"), std::string::npos)
        << "Unescaped double quote found in JSON output";
}

// ─────────────────────────────────────────────────────────────────────────────
// RttStats::to_json()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RttStatsJson, EmptyStatsProducesValidJson) {
    RttStats rtt{};
    auto json = rtt.to_json();
    EXPECT_NE(json.find("\"count\":0"), std::string::npos);
    EXPECT_NE(json.find("\"min_ns\":0"), std::string::npos);
}

TEST(RttStatsJson, PopulatedStatsIncludesAllFields) {
    RttStats rtt{
        .count = 100,
        .min_ns = 1000,
        .max_ns = 50000,
        .mean_ns = 10000.0,
        .p50_ns = 8000,
        .p99_ns = 45000,
        .p999_ns = 49000,
    };
    auto json = rtt.to_json();
    EXPECT_NE(json.find("\"count\":100"), std::string::npos);
    EXPECT_NE(json.find("\"min_ns\":1000"), std::string::npos);
    EXPECT_NE(json.find("\"max_ns\":50000"), std::string::npos);
    EXPECT_NE(json.find("\"p50_ns\":8000"), std::string::npos);
    EXPECT_NE(json.find("\"p99_ns\":45000"), std::string::npos);
    EXPECT_NE(json.find("\"p999_ns\":49000"), std::string::npos);
    EXPECT_NE(json.find("\"p50_us\":"), std::string::npos);
    EXPECT_NE(json.find("\"p99_us\":"), std::string::npos);
    EXPECT_NE(json.find("\"mean_us\":"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportStats RTT integration
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportStatsJson, IncludesRttObject) {
    TransportStats stats{};
    stats.rtt = RttStats{.count = 42, .min_ns = 500};
    auto json = stats.to_json();
    EXPECT_NE(json.find("\"rtt\":{"), std::string::npos);
    EXPECT_NE(json.find("\"count\":42"), std::string::npos);
}

TEST(TransportStatsDump, IncludesRttLine) {
    TransportStats stats{};
    stats.rtt = RttStats{.count = 10, .min_ns = 100, .p50_ns = 200};
    auto dump = stats.dump();
    EXPECT_NE(dump.find("RttStats"), std::string::npos);
}

TEST(TransportStatsJson, EscapesRemoteIp) {
    TransportStats stats{};
    stats.remote_ip = "10.0.0.1\"injected";
    auto json = stats.to_json();
    // Should have escaped quote
    EXPECT_NE(json.find(R"(10.0.0.1\"injected)"), std::string::npos);
}
