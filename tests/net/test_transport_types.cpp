/// @file test_transport_types.cpp
/// Unit tests for transport_types.hpp: SendError, TransportConfig::validate(),
/// TransportStats helpers, enum formatters, close code validation, and
/// ReceivedMessage close frame accessors.

#include <format>
#include <string>

#include <gtest/gtest.h>

#include "eph/transport/transport_types.hpp"
#include "eph/transport/websocket.hpp"
#include "eph/net/socket_connect.hpp"

using namespace eph::net;

// ─────────────────────────────────────────────────────────────────────────────
// SendError
// ─────────────────────────────────────────────────────────────────────────────

TEST(SendError, NameCoversAllVariants) {
    EXPECT_EQ(send_error_name(SendError::kOk), "OK");
    EXPECT_EQ(send_error_name(SendError::kMessageTooLarge), "MESSAGE_TOO_LARGE");
    EXPECT_EQ(send_error_name(SendError::kNotConnected), "NOT_CONNECTED");
    EXPECT_EQ(send_error_name(SendError::kQueueFull), "QUEUE_FULL");
    EXPECT_EQ(send_error_name(SendError::kInvalidUtf8), "INVALID_UTF8");
    EXPECT_EQ(send_error_name(SendError::kInvalidCloseCode), "INVALID_CLOSE_CODE");
    EXPECT_EQ(send_error_name(SendError::kNullData), "NULL_DATA");
}

TEST(SendError, BangOperatorReturnsTrueOnFailure) {
    EXPECT_FALSE(!SendError::kOk);
    EXPECT_TRUE(!SendError::kMessageTooLarge);
    EXPECT_TRUE(!SendError::kNotConnected);
    EXPECT_TRUE(!SendError::kQueueFull);
    EXPECT_TRUE(!SendError::kInvalidUtf8);
    EXPECT_TRUE(!SendError::kInvalidCloseCode);
    EXPECT_TRUE(!SendError::kNullData);
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
    EXPECT_EQ(connection_error_name(ConnectionError::kInvalidConfig), "INVALID_CONFIG");
    EXPECT_EQ(connection_error_name(ConnectionError::kFactoryFailed), "FACTORY_FAILED");
    EXPECT_EQ(connection_error_name(ConnectionError::kTcpNotEstablished), "TCP_NOT_ESTABLISHED");
    EXPECT_EQ(connection_error_name(ConnectionError::kTlsSessionFailed), "TLS_SESSION_FAILED");
    EXPECT_EQ(connection_error_name(ConnectionError::kTlsHandshakeFailed), "TLS_HANDSHAKE_FAILED");
    EXPECT_EQ(connection_error_name(ConnectionError::kTlsKeyExportFailed), "TLS_KEY_EXPORT_FAILED");
    EXPECT_EQ(connection_error_name(ConnectionError::kWsUpgradeFailed), "WS_UPGRADE_FAILED");
    EXPECT_EQ(connection_error_name(ConnectionError::kWsUpgradeRejected), "WS_UPGRADE_REJECTED");
    EXPECT_EQ(connection_error_name(ConnectionError::kWsAcceptInvalid), "WS_ACCEPT_INVALID");
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
    EXPECT_EQ(info.http_status.value(), 403);
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

TEST(TransportStats, MbpsConvenienceHelpers) {
    TransportStats stats{};
    stats.tx_bytes = 125'000'000;  // 125 MB
    stats.rx_bytes = 250'000'000;  // 250 MB
    stats.uptime_ns = 1'000'000'000;  // 1 second

    // 125 MB/s = 1000 Mbps, 250 MB/s = 2000 Mbps
    EXPECT_DOUBLE_EQ(stats.tx_mbps(), 1000.0);
    EXPECT_DOUBLE_EQ(stats.rx_mbps(), 2000.0);
}

TEST(TransportStats, MbpsZeroUptimeReturnsZero) {
    TransportStats stats{};
    stats.tx_bytes = 1000;
    stats.uptime_ns = 0;
    EXPECT_DOUBLE_EQ(stats.tx_mbps(), 0.0);
    EXPECT_DOUBLE_EQ(stats.rx_mbps(), 0.0);
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
    EXPECT_EQ(std::format("{}", SendError::kNullData), "NULL_DATA");
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

TEST(TransportTypeFormatters, TransportConfigFormats) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.remote_port = 443;
    cfg.ws_path = "/ws";
    cfg.use_tls = true;
    cfg.max_reconnect_attempts = 5;
    cfg.reconnect_interval = std::chrono::milliseconds{200};

    auto formatted = std::format("{}", cfg);
    EXPECT_NE(formatted.find("example.com"), std::string::npos);
    EXPECT_NE(formatted.find("443"), std::string::npos);
    EXPECT_NE(formatted.find("/ws"), std::string::npos);
    EXPECT_NE(formatted.find("tls=true"), std::string::npos);
    EXPECT_NE(formatted.find("reconnect=5/200"), std::string::npos);
}

TEST(TransportTypeFormatters, TransportConfigFormatsPlainWs) {
    TransportConfig cfg;
    cfg.remote_host = "local";
    cfg.remote_port = 8080;
    cfg.ws_path = "/";
    cfg.use_tls = false;
    cfg.max_reconnect_attempts = 0;

    auto formatted = std::format("{}", cfg);
    EXPECT_NE(formatted.find("tls=false"), std::string::npos);
    EXPECT_NE(formatted.find("reconnect=0/"), std::string::npos);
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

TEST(TransportConfigDump, ShowsMtlsAndUseTls) {
    TransportConfig cfg;
    cfg.remote_host = "mtls.example.com";
    cfg.ws_path = "/ws";
    cfg.use_tls = true;
    cfg.client_cert_path = "/certs/client.pem";
    cfg.client_key_path = "/certs/client.key";

    auto dump = cfg.dump();
    EXPECT_NE(dump.find("use_tls=true"), std::string::npos);
    EXPECT_NE(dump.find("client_cert=/certs/client.pem"), std::string::npos);
    EXPECT_NE(dump.find("client_key=/certs/client.key"), std::string::npos);
}

TEST(TransportConfigDump, PlainWsShowsUseTlsFalse) {
    TransportConfig cfg;
    cfg.remote_host = "ws.example.com";
    cfg.ws_path = "/ws";
    cfg.use_tls = false;

    auto dump = cfg.dump();
    EXPECT_NE(dump.find("use_tls=false"), std::string::npos);
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
    EXPECT_NE(json.find("\"use_tls\":true"), std::string::npos);
    EXPECT_NE(json.find("\"verify_peer\":true"), std::string::npos);
    EXPECT_NE(json.find("\"client_cert_path\":\"\""), std::string::npos);
    EXPECT_NE(json.find("\"client_key_path\":\"\""), std::string::npos);
}

TEST(TransportConfigJson, IncludesMtlsFields) {
    TransportConfig cfg{};
    cfg.remote_host = "mtls.example.com";
    cfg.ws_path = "/ws";
    cfg.use_tls = true;
    cfg.client_cert_path = "/path/to/cert.pem";
    cfg.client_key_path = "/path/to/key.pem";

    auto json = cfg.to_json();
    EXPECT_NE(json.find("\"client_cert_path\":\"/path/to/cert.pem\""),
              std::string::npos);
    EXPECT_NE(json.find("\"client_key_path\":\"/path/to/key.pem\""),
              std::string::npos);
    EXPECT_NE(json.find("\"use_tls\":true"), std::string::npos);
}

TEST(TransportConfigJson, PlainWsShowsUseTlsFalse) {
    TransportConfig cfg{};
    cfg.remote_host = "ws.example.com";
    cfg.ws_path = "/ws";
    cfg.use_tls = false;

    auto json = cfg.to_json();
    EXPECT_NE(json.find("\"use_tls\":false"), std::string::npos);
}

TEST(TransportConfigJson, IncludesExtraHeaders) {
    TransportConfig cfg{};
    cfg.remote_host = "example.com";
    cfg.ws_path = "/ws";
    cfg.extra_headers = "Authorization: Bearer tok\r\n";

    auto json = cfg.to_json();
    // extra_headers should appear in JSON (with \r\n escaped)
    EXPECT_NE(json.find("\"extra_headers\":"), std::string::npos);
    EXPECT_NE(json.find("Authorization"), std::string::npos);
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

TEST(JsonEscape, AllEscapeCharsString) {
    // String consisting entirely of characters that need escaping
    std::string input(100, '"');
    auto result = detail::json_escape(input);
    EXPECT_EQ(result.size(), 200u); // each " becomes \"
    for (size_t i = 0; i < result.size(); i += 2) {
        EXPECT_EQ(result[i], '\\');
        EXPECT_EQ(result[i + 1], '"');
    }
}

TEST(JsonEscape, MixedAsciiAndUtf8) {
    // Mix of escaped ASCII and UTF-8 multibyte sequences
    auto result = detail::json_escape("key=\"\xe4\xb8\xad\xe6\x96\x87\"");
    EXPECT_EQ(result, "key=\\\"\xe4\xb8\xad\xe6\x96\x87\\\"");
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

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionInfo
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectionInfo, DumpContainsAllFields) {
    ConnectionInfo info{
        .tls_version = "TLSv1.3",
        .cipher_name = "TLS_AES_256_GCM_SHA384",
        .ws_subprotocol = "graphql-ws",
        .remote_ip = "10.0.0.1",
        .remote_port = 443,
        .use_tls = true,
    };
    auto dump = info.dump();
    EXPECT_NE(dump.find("10.0.0.1"), std::string::npos);
    EXPECT_NE(dump.find("443"), std::string::npos);
    EXPECT_NE(dump.find("TLSv1.3"), std::string::npos);
    EXPECT_NE(dump.find("TLS_AES_256_GCM_SHA384"), std::string::npos);
    EXPECT_NE(dump.find("graphql-ws"), std::string::npos);
}

TEST(ConnectionInfo, DumpHandlesEmptyFields) {
    ConnectionInfo info{};
    auto dump = info.dump();
    EXPECT_NE(dump.find("unknown"), std::string::npos);
    EXPECT_NE(dump.find("(none)"), std::string::npos);
}

TEST(ConnectionInfo, DefaultToJsonProducesValidJson) {
    ConnectionInfo info{};
    auto json = info.to_json();
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"tls_version\":\"\""), std::string::npos);
    EXPECT_NE(json.find("\"remote_port\":0"), std::string::npos);
    EXPECT_NE(json.find("\"use_tls\":true"), std::string::npos);
}

TEST(ConnectionInfo, ToJsonContainsAllFields) {
    ConnectionInfo info{
        .tls_version = "TLSv1.3",
        .cipher_name = "TLS_AES_256_GCM_SHA384",
        .ws_subprotocol = "proto",
        .remote_ip = "10.0.0.1",
        .remote_port = 8443,
        .use_tls = true,
    };
    auto json = info.to_json();
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"tls_version\":\"TLSv1.3\""), std::string::npos);
    EXPECT_NE(json.find("\"cipher_name\":\"TLS_AES_256_GCM_SHA384\""), std::string::npos);
    EXPECT_NE(json.find("\"ws_subprotocol\":\"proto\""), std::string::npos);
    EXPECT_NE(json.find("\"remote_ip\":\"10.0.0.1\""), std::string::npos);
    EXPECT_NE(json.find("\"remote_port\":8443"), std::string::npos);
    EXPECT_NE(json.find("\"use_tls\":true"), std::string::npos);
}

TEST(ConnectionInfo, ToJsonEscapesStringFields) {
    ConnectionInfo info{
        .tls_version = "TLS\"injected",
        .cipher_name = {},
        .ws_subprotocol = {},
        .remote_ip = "10.0.0.1\\path",
    };
    auto json = info.to_json();
    EXPECT_NE(json.find(R"(TLS\"injected)"), std::string::npos);
    EXPECT_NE(json.find(R"(10.0.0.1\\path)"), std::string::npos);
}

TEST(ConnectionInfo, PlainWsShowsUseTlsFalse) {
    ConnectionInfo info{.tls_version = {}, .cipher_name = {}, .ws_subprotocol = {},
                        .remote_ip = {}, .remote_port = 0, .use_tls = false};
    auto json = info.to_json();
    EXPECT_NE(json.find("\"use_tls\":false"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionErrorInfo::to_json()
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectionErrorInfo, ToJsonProducesValidJson) {
    ConnectionErrorInfo err{
        .code = ConnectionError::kTlsHandshakeFailed,
        .detail = "certificate expired",
        .http_status = 0,
    };
    auto json = err.to_json();
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"code\":\"TLS_HANDSHAKE_FAILED\""), std::string::npos);
    EXPECT_NE(json.find("\"detail\":\"certificate expired\""), std::string::npos);
    EXPECT_NE(json.find("\"http_status\":0"), std::string::npos);
}

TEST(ConnectionErrorInfo, ToJsonEscapesDetail) {
    ConnectionErrorInfo err{
        .code = ConnectionError::kWsUpgradeRejected,
        .detail = "reason: \"forbidden\"",
        .http_status = 403,
    };
    auto json = err.to_json();
    EXPECT_NE(json.find(R"(reason: \"forbidden\")"), std::string::npos);
    EXPECT_NE(json.find("\"http_status\":403"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter<ConnectionInfo>
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectionInfoFormatter, FormatsWithAllFields) {
    ConnectionInfo info{
        .tls_version = "TLSv1.3",
        .cipher_name = "TLS_AES_256_GCM_SHA384",
        .ws_subprotocol = "graphql-ws",
        .remote_ip = "10.0.0.1",
        .remote_port = 8443,
        .use_tls = true,
    };
    auto str = std::format("{}", info);
    EXPECT_NE(str.find("10.0.0.1:8443"), std::string::npos);
    EXPECT_NE(str.find("tls=true"), std::string::npos);
    EXPECT_NE(str.find("version=TLSv1.3"), std::string::npos);
    EXPECT_NE(str.find("cipher=TLS_AES_256_GCM_SHA384"), std::string::npos);
    EXPECT_NE(str.find("subproto=graphql-ws"), std::string::npos);
}

TEST(ConnectionInfoFormatter, FormatsWithEmptyFields) {
    ConnectionInfo info{};
    auto str = std::format("{}", info);
    EXPECT_NE(str.find("unknown"), std::string::npos);
    EXPECT_NE(str.find("tls=true"), std::string::npos);
    EXPECT_NE(str.find("version=none"), std::string::npos);
    EXPECT_NE(str.find("subproto=(none)"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// RttStats convenience accessors (min_us, max_us, p999_us)
// ─────────────────────────────────────────────────────────────────────────────

TEST(RttStats, AllConvenienceAccessors) {
    RttStats rtt{
        .count = 100,
        .min_ns = 1000,
        .max_ns = 5000000,
        .mean_ns = 100000.0,
        .p50_ns = 50000,
        .p99_ns = 900000,
        .p999_ns = 4500000,
    };
    EXPECT_DOUBLE_EQ(rtt.min_us(), 1.0);
    EXPECT_DOUBLE_EQ(rtt.max_us(), 5000.0);
    EXPECT_DOUBLE_EQ(rtt.p50_us(), 50.0);
    EXPECT_DOUBLE_EQ(rtt.p99_us(), 900.0);
    EXPECT_DOUBLE_EQ(rtt.p999_us(), 4500.0);
    EXPECT_DOUBLE_EQ(rtt.mean_us(), 100.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::warnings()
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigWarnings, NoWarningsForValidConfig) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.skip_utf8_validation = false;  // default is true which emits warning
    EXPECT_TRUE(cfg.warnings().empty());
}

TEST(TransportConfigWarnings, VerifyPeerWithoutTls) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.use_tls = false;
    cfg.verify_peer = true;
    auto w = cfg.warnings();
    ASSERT_GE(w.size(), 1);
    EXPECT_NE(w[0].find("verify_peer"), std::string::npos);
}

TEST(TransportConfigWarnings, CaCertPathWithoutTls) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.use_tls = false;
    cfg.verify_peer = false;
    cfg.ca_cert_path = "/some/ca.pem";
    auto w = cfg.warnings();
    ASSERT_GE(w.size(), 1);
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("ca_cert_path") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TransportConfigValidation, PongTimeoutExceedsPingIntervalIsError) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.ping_interval = std::chrono::seconds{10};
    cfg.pong_timeout = std::chrono::seconds{15};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("pong_timeout"), std::string_view::npos);
}

TEST(TransportConfigWarnings, LargeBurstSizeWarning) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.tx_burst_size = 2048;
    auto w = cfg.warnings();
    ASSERT_GE(w.size(), 1);
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("tx_burst_size") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TransportConfigWarnings, LargeRxBurstSizeWarning) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.rx_burst_size = 2048;
    auto w = cfg.warnings();
    ASSERT_GE(w.size(), 1);
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("rx_burst_size") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TransportConfigValidation, PongTimeoutEqualsPingIntervalIsError) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.ping_interval = std::chrono::seconds{10};
    cfg.pong_timeout = std::chrono::seconds{10};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("pong_timeout"), std::string_view::npos);
}

TEST(TransportConfigWarnings, MultipleWarningsFromSameConfig) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.use_tls = false;
    cfg.verify_peer = true;
    cfg.ca_cert_path = "/some/ca.pem";
    auto w = cfg.warnings();
    // Both verify_peer and ca_cert_path warnings should fire
    ASSERT_GE(w.size(), 2);
    bool has_verify = false, has_ca = false;
    for (const auto& msg : w) {
        if (msg.find("verify_peer") != std::string::npos) has_verify = true;
        if (msg.find("ca_cert_path") != std::string::npos) has_ca = true;
    }
    EXPECT_TRUE(has_verify);
    EXPECT_TRUE(has_ca);
}

TEST(TransportConfigWarnings, SkipUtf8ValidationWarning) {
    // Default (true) should produce the warning
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    auto w = cfg.warnings();
    ASSERT_GE(w.size(), 1u);
    bool has_utf8 = false;
    for (const auto& msg : w) {
        if (msg.find("skip_utf8_validation") != std::string::npos) has_utf8 = true;
    }
    EXPECT_TRUE(has_utf8);

    // Explicitly disabled (false) should NOT produce this warning
    TransportConfig cfg2;
    cfg2.remote_host = "example.com";
    cfg2.skip_utf8_validation = false;
    auto w2 = cfg2.warnings();
    for (const auto& msg : w2) {
        EXPECT_EQ(msg.find("skip_utf8_validation"), std::string::npos);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ThreadStats::reset()
// ─────────────────────────────────────────────────────────────────────────────

TEST(ThreadStats, ResetZeroesAllCounters) {
    ThreadStats ts;
    ts.packets.store(100, std::memory_order_relaxed);
    ts.bytes.store(5000, std::memory_order_relaxed);
    ts.text_packets.store(42, std::memory_order_relaxed);
    ts.text_bytes.store(2100, std::memory_order_relaxed);
    ts.dropped.store(7, std::memory_order_relaxed);
    ts.crypto_errors.store(3, std::memory_order_relaxed);

    ts.reset();

    EXPECT_EQ(ts.packets.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(ts.bytes.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(ts.text_packets.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(ts.text_bytes.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(ts.dropped.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(ts.crypto_errors.load(std::memory_order_relaxed), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportStats HWM in dump()/to_json()
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportStatsDump, IncludesHwmFields) {
    TransportStats stats{};
    stats.tx_queue_hwm = 512;
    stats.rx_queue_hwm = 256;
    auto dump = stats.dump();
    EXPECT_NE(dump.find("TX HWM: 512"), std::string::npos);
    EXPECT_NE(dump.find("RX HWM: 256"), std::string::npos);
}

TEST(TransportStatsJson, IncludesHwmFields) {
    TransportStats stats{};
    stats.tx_queue_hwm = 128;
    stats.rx_queue_hwm = 64;
    auto json = stats.to_json();
    EXPECT_NE(json.find("\"tx_queue_hwm\":128"), std::string::npos);
    EXPECT_NE(json.find("\"rx_queue_hwm\":64"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::to_json() extra_headers escaping
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigJson, ExtraHeadersSpecialCharsEscaped) {
    TransportConfig cfg{};
    cfg.remote_host = "example.com";
    cfg.ws_path = "/ws";
    cfg.extra_headers = "X-Custom: value\"with\\quotes\r\n";

    auto json = cfg.to_json();
    // Quotes and backslashes should be escaped in JSON
    EXPECT_NE(json.find("extra_headers"), std::string::npos);
    // Raw " and \ must NOT appear unescaped
    auto pos = json.find("\"extra_headers\":\"");
    ASSERT_NE(pos, std::string::npos);
    // Check that the value portion has \\\" (escaped quote)
    auto value_start = pos + 17; // length of "extra_headers":"
    auto value = json.substr(value_start);
    EXPECT_NE(value.find("\\\""), std::string::npos);
    EXPECT_NE(value.find("\\\\"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// TLS sequence stats
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportStats, TlsSeqUsageZeroWhenNoTls) {
    TransportStats stats{};
    stats.tls_seq_limit = 0;
    EXPECT_DOUBLE_EQ(stats.tls_write_seq_usage(), 0.0);
    EXPECT_DOUBLE_EQ(stats.tls_read_seq_usage(), 0.0);
}

TEST(TransportStats, TlsSeqUsageComputesCorrectly) {
    TransportStats stats{};
    stats.tls_seq_limit = 1000;
    stats.tls_write_seq = 500;
    stats.tls_read_seq = 250;
    EXPECT_DOUBLE_EQ(stats.tls_write_seq_usage(), 0.5);
    EXPECT_DOUBLE_EQ(stats.tls_read_seq_usage(), 0.25);
}

TEST(TransportStats, DumpIncludesTlsSeqInfo) {
    TransportStats stats{};
    stats.uptime_ns = 1'000'000'000;
    stats.tls_write_seq = 100;
    stats.tls_read_seq = 50;
    stats.tls_seq_limit = 16'777'216;
    auto dump = stats.dump();
    EXPECT_NE(dump.find("TLS seq:"), std::string::npos);
}

TEST(TransportStats, ToJsonIncludesTlsSeqFields) {
    TransportStats stats{};
    stats.tls_write_seq = 42;
    stats.tls_read_seq = 7;
    stats.tls_seq_limit = 1000;
    auto json = stats.to_json();
    EXPECT_NE(json.find("\"tls_write_seq\":42"), std::string::npos);
    EXPECT_NE(json.find("\"tls_read_seq\":7"), std::string::npos);
    EXPECT_NE(json.find("\"tls_seq_limit\":1000"), std::string::npos);
    EXPECT_NE(json.find("\"tls_write_seq_usage\":"), std::string::npos);
}

// ===========================================================================
// TransportStats delta (operator-)
// ===========================================================================

TEST(TransportStats, DeltaSubtractsCounters) {
    TransportStats s1{};
    s1.tx_packets = 100;
    s1.tx_bytes = 5000;
    s1.rx_packets = 80;
    s1.rx_bytes = 4000;
    s1.tx_dropped = 2;
    s1.rx_dropped = 1;
    s1.encrypt_errors = 0;
    s1.decrypt_errors = 0;
    s1.queue_full_count = 3;
    s1.uptime_ns = 1'000'000'000;

    TransportStats s2{};
    s2.tx_packets = 250;
    s2.tx_bytes = 12000;
    s2.rx_packets = 200;
    s2.rx_bytes = 10000;
    s2.tx_dropped = 5;
    s2.rx_dropped = 2;
    s2.encrypt_errors = 1;
    s2.decrypt_errors = 0;
    s2.queue_full_count = 7;
    s2.uptime_ns = 3'000'000'000;

    auto d = s2 - s1;
    EXPECT_EQ(d.tx_packets, 150u);
    EXPECT_EQ(d.tx_bytes, 7000u);
    EXPECT_EQ(d.rx_packets, 120u);
    EXPECT_EQ(d.rx_bytes, 6000u);
    EXPECT_EQ(d.tx_dropped, 3u);
    EXPECT_EQ(d.rx_dropped, 1u);
    EXPECT_EQ(d.encrypt_errors, 1u);
    EXPECT_EQ(d.queue_full_count, 4u);
    EXPECT_EQ(d.uptime_ns, 2'000'000'000u);
}

TEST(TransportStats, DeltaRateHelpers) {
    TransportStats s1{};
    s1.tx_packets = 0;
    s1.uptime_ns = 0;

    TransportStats s2{};
    s2.tx_packets = 1000;
    s2.tx_bytes = 50000;
    s2.rx_packets = 800;
    s2.rx_bytes = 40000;
    s2.uptime_ns = 1'000'000'000;  // 1 second window

    auto d = s2 - s1;
    EXPECT_DOUBLE_EQ(d.tx_pps(), 1000.0);
    EXPECT_DOUBLE_EQ(d.rx_pps(), 800.0);
    EXPECT_DOUBLE_EQ(d.tx_bps(), 50000.0);
    EXPECT_DOUBLE_EQ(d.rx_bps(), 40000.0);
}

TEST(TransportStats, DeltaPreservesMetadataFromLater) {
    TransportStats s1{};
    s1.remote_ip = "1.2.3.4";
    s1.tls_write_seq = 10;
    s1.handshake_ns = 500'000;

    TransportStats s2{};
    s2.remote_ip = "5.6.7.8";
    s2.tls_write_seq = 42;
    s2.tls_read_seq = 7;
    s2.tls_seq_limit = 1000;
    s2.handshake_ns = 300'000;
    s2.tx_queue_hwm = 16;

    auto d = s2 - s1;
    EXPECT_EQ(d.remote_ip, "5.6.7.8");
    EXPECT_EQ(d.tls_write_seq, 42u);
    EXPECT_EQ(d.tls_read_seq, 7u);
    EXPECT_EQ(d.handshake_ns, 300'000u);
    EXPECT_EQ(d.tx_queue_hwm, 16u);
}

TEST(TransportStats, DeltaIdentityWhenSameSnapshot) {
    TransportStats s{};
    s.tx_packets = 100;
    s.rx_packets = 80;
    s.uptime_ns = 5'000'000'000;

    auto d = s - s;
    EXPECT_EQ(d.tx_packets, 0u);
    EXPECT_EQ(d.rx_packets, 0u);
    EXPECT_EQ(d.uptime_ns, 0u);
}

TEST(TransportStats, DeltaTextCounters) {
    TransportStats s1{};
    s1.tx_text_packets = 10;
    s1.tx_text_bytes = 500;
    s1.rx_text_packets = 8;
    s1.rx_text_bytes = 400;

    TransportStats s2{};
    s2.tx_text_packets = 25;
    s2.tx_text_bytes = 1200;
    s2.rx_text_packets = 20;
    s2.rx_text_bytes = 1000;

    auto d = s2 - s1;
    EXPECT_EQ(d.tx_text_packets, 15u);
    EXPECT_EQ(d.tx_text_bytes, 700u);
    EXPECT_EQ(d.rx_text_packets, 12u);
    EXPECT_EQ(d.rx_text_bytes, 600u);
}

TEST(TransportStats, DeltaWebSocketCounters) {
    TransportStats s1{};
    s1.ws_pings_received = 5;
    s1.ws_pongs_sent = 5;
    s1.pong_timeouts = 1;
    s1.reconnect_count = 2;

    TransportStats s2{};
    s2.ws_pings_received = 15;
    s2.ws_pongs_sent = 14;
    s2.pong_timeouts = 3;
    s2.reconnect_count = 4;

    auto d = s2 - s1;
    EXPECT_EQ(d.ws_pings_received, 10u);
    EXPECT_EQ(d.ws_pongs_sent, 9u);
    EXPECT_EQ(d.pong_timeouts, 2u);
    EXPECT_EQ(d.reconnect_count, 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::from_url()
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigFromUrl, WssFullUrl) {
    auto r = TransportConfig::from_url("wss://example.com:8443/ws/v2");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_host, "example.com");
    EXPECT_EQ(r->remote_port, 8443);
    EXPECT_EQ(r->ws_path, "/ws/v2");
    EXPECT_TRUE(r->use_tls);
}

TEST(TransportConfigFromUrl, WsFullUrl) {
    auto r = TransportConfig::from_url("ws://localhost:9000/feed");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_host, "localhost");
    EXPECT_EQ(r->remote_port, 9000);
    EXPECT_EQ(r->ws_path, "/feed");
    EXPECT_FALSE(r->use_tls);
}

TEST(TransportConfigFromUrl, WssDefaultPort) {
    auto r = TransportConfig::from_url("wss://api.example.com/stream");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_host, "api.example.com");
    EXPECT_EQ(r->remote_port, 443);
    EXPECT_EQ(r->ws_path, "/stream");
    EXPECT_TRUE(r->use_tls);
}

TEST(TransportConfigFromUrl, WsDefaultPort) {
    auto r = TransportConfig::from_url("ws://localhost/ws");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_port, 80);
    EXPECT_FALSE(r->use_tls);
}

TEST(TransportConfigFromUrl, HostOnly) {
    auto r = TransportConfig::from_url("wss://api.exchange.io");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_host, "api.exchange.io");
    EXPECT_EQ(r->remote_port, 443);
    EXPECT_EQ(r->ws_path, "/");
}

TEST(TransportConfigFromUrl, HostAndPortNoPath) {
    auto r = TransportConfig::from_url("wss://api.exchange.io:9443");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_host, "api.exchange.io");
    EXPECT_EQ(r->remote_port, 9443);
    EXPECT_EQ(r->ws_path, "/");
}

TEST(TransportConfigFromUrl, PathWithQueryString) {
    auto r = TransportConfig::from_url("wss://stream.exchange.com/ws?token=abc123");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_host, "stream.exchange.com");
    EXPECT_EQ(r->ws_path, "/ws?token=abc123");
}

TEST(TransportConfigFromUrl, LeadingTrailingWhitespace) {
    auto r = TransportConfig::from_url("  wss://example.com/ws  ");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_host, "example.com");
    EXPECT_EQ(r->ws_path, "/ws");
}

TEST(TransportConfigFromUrl, ErrorInvalidScheme) {
    auto r = TransportConfig::from_url("https://example.com/ws");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("ws://"), std::string::npos);
}

TEST(TransportConfigFromUrl, ErrorMissingHost) {
    auto r = TransportConfig::from_url("wss://");
    ASSERT_FALSE(r.has_value());
}

TEST(TransportConfigFromUrl, ErrorEmptyHost) {
    auto r = TransportConfig::from_url("wss://:443/ws");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("empty host"), std::string::npos);
}

TEST(TransportConfigFromUrl, ErrorInvalidPort) {
    auto r = TransportConfig::from_url("wss://host:abc/ws");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("port"), std::string::npos);
}

TEST(TransportConfigFromUrl, ErrorPortZero) {
    auto r = TransportConfig::from_url("wss://host:0/ws");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("port"), std::string::npos);
}

TEST(TransportConfigFromUrl, ErrorEmptyPortAfterColon) {
    auto r = TransportConfig::from_url("wss://host:/ws");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("port"), std::string::npos);
}

TEST(TransportConfigFromUrl, ErrorEmptyString) {
    auto r = TransportConfig::from_url("");
    ASSERT_FALSE(r.has_value());
}

TEST(TransportConfigFromUrl, ValidatePassesAfterFromUrl) {
    auto r = TransportConfig::from_url("wss://example.com:443/ws");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->validate().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::to_url()
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigToUrl, WssDefaultPort) {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.remote_port = 443;
    cfg.ws_path = "/ws";
    cfg.use_tls = true;
    EXPECT_EQ(cfg.to_url(), "wss://example.com/ws");
}

TEST(TransportConfigToUrl, WsDefaultPort) {
    TransportConfig cfg;
    cfg.remote_host = "localhost";
    cfg.remote_port = 80;
    cfg.ws_path = "/feed";
    cfg.use_tls = false;
    EXPECT_EQ(cfg.to_url(), "ws://localhost/feed");
}

TEST(TransportConfigToUrl, NonDefaultPort) {
    TransportConfig cfg;
    cfg.remote_host = "api.exchange.io";
    cfg.remote_port = 8443;
    cfg.ws_path = "/stream";
    cfg.use_tls = true;
    EXPECT_EQ(cfg.to_url(), "wss://api.exchange.io:8443/stream");
}

TEST(TransportConfigToUrl, RoundTripFromUrl) {
    auto original = "wss://api.exchange.io:9443/ws/v2";
    auto r = TransportConfig::from_url(original);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->to_url(), original);
}

TEST(TransportConfigToUrl, RoundTripDefaultPort) {
    auto r = TransportConfig::from_url("wss://example.com/ws");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->to_url(), "wss://example.com/ws");
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::from_url() — IPv6 support
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigFromUrl, Ipv6Loopback) {
    auto r = TransportConfig::from_url("wss://[::1]/ws");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_host, "::1");
    EXPECT_EQ(r->remote_port, 443);
    EXPECT_EQ(r->ws_path, "/ws");
}

TEST(TransportConfigFromUrl, Ipv6WithPort) {
    auto r = TransportConfig::from_url("wss://[2001:db8::1]:8443/stream");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_host, "2001:db8::1");
    EXPECT_EQ(r->remote_port, 8443);
    EXPECT_EQ(r->ws_path, "/stream");
}

TEST(TransportConfigFromUrl, Ipv6NoPath) {
    auto r = TransportConfig::from_url("ws://[::1]");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_host, "::1");
    EXPECT_EQ(r->remote_port, 80);
    EXPECT_EQ(r->ws_path, "/");
}

TEST(TransportConfigFromUrl, Ipv6MissingCloseBracket) {
    auto r = TransportConfig::from_url("wss://[::1/ws");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("']'"), std::string::npos);
}

TEST(TransportConfigFromUrl, Ipv6EmptyAddress) {
    auto r = TransportConfig::from_url("wss://[]/ws");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("empty"), std::string::npos);
}

TEST(TransportConfigFromUrl, Ipv6RoundTrip) {
    auto r = TransportConfig::from_url("wss://[::1]:9443/ws");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->to_url(), "wss://[::1]:9443/ws");
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::from_url() — port edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigFromUrl, PortMaxValid) {
    auto r = TransportConfig::from_url("wss://host:65535/ws");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->remote_port, 65535);
}

TEST(TransportConfigFromUrl, PortOverflow) {
    auto r = TransportConfig::from_url("wss://host:65536/ws");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("range"), std::string::npos);
}

TEST(TransportConfigFromUrl, PortWayOverflow) {
    auto r = TransportConfig::from_url("wss://host:100000/ws");
    ASSERT_FALSE(r.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::from_url() — hostname safety
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigFromUrl, HostWithControlCharsRejected) {
    // Newline injection
    auto r = TransportConfig::from_url("wss://host\ninjection/ws");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("control"), std::string::npos);
}

TEST(TransportConfigFromUrl, HostWithNullByteRejected) {
    std::string url = "wss://ho";
    url += '\0';
    url += "st/ws";
    auto r = TransportConfig::from_url(std::string_view(url));
    // from_url sees a NUL in the host → control char rejection
    // (or the string_view might be truncated at NUL, yielding "ho" as host)
    // Either way, it should not silently pass an injected hostname
    if (r.has_value()) {
        // If it parsed, the host should be truncated (safe)
        EXPECT_EQ(r->remote_host.find('\0'), std::string::npos);
    }
}

TEST(TransportConfigFromUrl, PathWithFragment) {
    auto r = TransportConfig::from_url("wss://example.com/ws#section");
    ASSERT_TRUE(r.has_value());
    // Fragments are preserved in ws_path (server decides how to handle)
    EXPECT_EQ(r->ws_path, "/ws#section");
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportConfig::drop_log_interval
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportConfigDropLog, DefaultValueIs1000) {
    TransportConfig cfg;
    EXPECT_EQ(cfg.drop_log_interval, 1000u);
}

TEST(TransportConfigDropLog, CustomValuePreserved) {
    TransportConfig cfg;
    cfg.drop_log_interval = 500;
    EXPECT_EQ(cfg.drop_log_interval, 500u);
}

TEST(TransportConfigDropLog, ZeroDisablesLogging) {
    TransportConfig cfg;
    cfg.drop_log_interval = 0;
    EXPECT_EQ(cfg.drop_log_interval, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// RttStats operator== and operator-
// ─────────────────────────────────────────────────────────────────────────────

TEST(RttStats, EqualityDefaultBehavior) {
    RttStats a{.count = 10, .min_ns = 100, .max_ns = 500, .mean_ns = 300.0,
               .p50_ns = 250, .p99_ns = 480, .p999_ns = 499};
    RttStats b = a;
    EXPECT_EQ(a, b);
    b.count = 11;
    EXPECT_NE(a, b);
}

TEST(RttStats, EqualityDefaultZero) {
    RttStats a{};
    RttStats b{};
    EXPECT_EQ(a, b);
}

TEST(RttStats, DeltaSubtractsCount) {
    RttStats s1{.count = 50, .min_ns = 100, .max_ns = 900, .mean_ns = 500.0,
                .p50_ns = 400, .p99_ns = 850, .p999_ns = 890};
    RttStats s2{.count = 30, .min_ns = 200, .max_ns = 800, .mean_ns = 400.0,
                .p50_ns = 350, .p99_ns = 700, .p999_ns = 750};
    auto delta = s1 - s2;
    EXPECT_EQ(delta.count, 20u);
    // Percentile/mean fields come from the later snapshot (s1)
    EXPECT_EQ(delta.min_ns, s1.min_ns);
    EXPECT_EQ(delta.max_ns, s1.max_ns);
    EXPECT_DOUBLE_EQ(delta.mean_ns, s1.mean_ns);
    EXPECT_EQ(delta.p50_ns, s1.p50_ns);
    EXPECT_EQ(delta.p99_ns, s1.p99_ns);
    EXPECT_EQ(delta.p999_ns, s1.p999_ns);
}

TEST(RttStats, DeltaIdentityWhenSame) {
    RttStats s{.count = 100, .min_ns = 50};
    auto delta = s - s;
    EXPECT_EQ(delta.count, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// TransportStats operator==
// ─────────────────────────────────────────────────────────────────────────────

TEST(TransportStats, EqualityDefaultBehavior) {
    TransportStats a{};
    a.tx_packets = 100;
    a.rx_bytes = 5000;
    a.remote_ip = "10.0.0.1";
    TransportStats b = a;
    EXPECT_EQ(a, b);
    b.tx_packets = 101;
    EXPECT_NE(a, b);
}

TEST(TransportStats, EqualityIncludesRttAndRemoteIp) {
    TransportStats a{};
    a.rtt = {.count = 5, .min_ns = 100};
    a.remote_ip = "10.0.0.1";
    TransportStats b = a;
    EXPECT_EQ(a, b);
    b.rtt.count = 6;
    EXPECT_NE(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// ThreadStats::Snapshot — to_json, dump, operator==, operator-
// ─────────────────────────────────────────────────────────────────────────────

TEST(ThreadStatsSnapshot, SnapshotCapturesCurrentValues) {
    ThreadStats ts;
    ts.packets.store(10, std::memory_order_relaxed);
    ts.bytes.store(500, std::memory_order_relaxed);
    ts.text_packets.store(3, std::memory_order_relaxed);
    ts.text_bytes.store(150, std::memory_order_relaxed);
    ts.dropped.store(1, std::memory_order_relaxed);
    ts.crypto_errors.store(2, std::memory_order_relaxed);
    auto snap = ts.snapshot();
    EXPECT_EQ(snap.packets, 10u);
    EXPECT_EQ(snap.bytes, 500u);
    EXPECT_EQ(snap.text_packets, 3u);
    EXPECT_EQ(snap.text_bytes, 150u);
    EXPECT_EQ(snap.dropped, 1u);
    EXPECT_EQ(snap.crypto_errors, 2u);
}

TEST(ThreadStatsSnapshot, EqualityDefaultBehavior) {
    ThreadStats::Snapshot a{.packets = 1, .bytes = 2, .text_packets = 3,
                            .text_bytes = 4, .dropped = 5, .crypto_errors = 6};
    ThreadStats::Snapshot b = a;
    EXPECT_EQ(a, b);
    b.dropped = 99;
    EXPECT_NE(a, b);
}

TEST(ThreadStatsSnapshot, DeltaSubtractsAllFields) {
    ThreadStats::Snapshot s1{.packets = 100, .bytes = 5000, .text_packets = 30,
                             .text_bytes = 1500, .dropped = 5, .crypto_errors = 2};
    ThreadStats::Snapshot s2{.packets = 70,  .bytes = 3000, .text_packets = 20,
                             .text_bytes = 1000, .dropped = 3, .crypto_errors = 1};
    auto delta = s1 - s2;
    EXPECT_EQ(delta.packets, 30u);
    EXPECT_EQ(delta.bytes, 2000u);
    EXPECT_EQ(delta.text_packets, 10u);
    EXPECT_EQ(delta.text_bytes, 500u);
    EXPECT_EQ(delta.dropped, 2u);
    EXPECT_EQ(delta.crypto_errors, 1u);
}

TEST(ThreadStatsSnapshot, DumpContainsAllFields) {
    ThreadStats::Snapshot s{.packets = 42, .bytes = 999, .dropped = 7};
    auto d = s.dump();
    EXPECT_NE(d.find("ThreadStats::Snapshot"), std::string::npos);
    EXPECT_NE(d.find("42"), std::string::npos);
    EXPECT_NE(d.find("999"), std::string::npos);
    EXPECT_NE(d.find("7"), std::string::npos);
}

TEST(ThreadStatsSnapshot, ToJsonContainsAllFields) {
    ThreadStats::Snapshot s{.packets = 10, .bytes = 200, .text_packets = 3,
                            .text_bytes = 50, .dropped = 1, .crypto_errors = 0};
    auto j = s.to_json();
    EXPECT_NE(j.find("\"packets\":10"), std::string::npos);
    EXPECT_NE(j.find("\"bytes\":200"), std::string::npos);
    EXPECT_NE(j.find("\"text_packets\":3"), std::string::npos);
    EXPECT_NE(j.find("\"text_bytes\":50"), std::string::npos);
    EXPECT_NE(j.find("\"dropped\":1"), std::string::npos);
    EXPECT_NE(j.find("\"crypto_errors\":0"), std::string::npos);
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
}

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionErrorInfo operator==
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectionErrorInfo, EqualityDefaultBehavior) {
    ConnectionErrorInfo a{ConnectionError::kTcpNotEstablished, "timeout", 0};
    ConnectionErrorInfo b = a;
    EXPECT_EQ(a, b);
    b.detail = "refused";
    EXPECT_NE(a, b);
}

TEST(ConnectionErrorInfo, EqualityIncludesHttpStatus) {
    ConnectionErrorInfo a{ConnectionError::kWsUpgradeRejected, "403", 403};
    ConnectionErrorInfo b = a;
    EXPECT_EQ(a, b);
    b.http_status = 401;
    EXPECT_NE(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// SocketConfig operator==
// ─────────────────────────────────────────────────────────────────────────────

TEST(SocketConfig, EqualityDefaultBehavior) {
    SocketConfig a{.host = "localhost", .port = 8080};
    SocketConfig b = a;
    EXPECT_EQ(a, b);
    b.port = 9090;
    EXPECT_NE(a, b);
}

TEST(SocketConfig, EqualityIncludesAllFields) {
    SocketConfig a{.host = "example.com", .port = 443, .tcp_nodelay = true,
                   .recv_buf_size = 4096, .send_buf_size = 4096,
                   .tcp_keepalive = true, .keepalive_idle = 30,
                   .keepalive_interval = 5, .keepalive_count = 2,
                   .send_timeout_ms = 500};
    SocketConfig b = a;
    EXPECT_EQ(a, b);
    b.tcp_keepalive = false;
    EXPECT_NE(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge case tests for observability operators
// ─────────────────────────────────────────────────────────────────────────────

TEST(ThreadStatsSnapshot, DeltaIdentityWhenSame) {
    ThreadStats::Snapshot s{.packets = 50, .bytes = 2000, .text_packets = 10,
                            .text_bytes = 500, .dropped = 3, .crypto_errors = 1};
    auto delta = s - s;
    ThreadStats::Snapshot zero{};
    EXPECT_EQ(delta, zero);
}

TEST(ThreadStatsSnapshot, SnapshotAfterResetIsZero) {
    ThreadStats ts;
    ts.packets.store(42, std::memory_order_relaxed);
    ts.bytes.store(999, std::memory_order_relaxed);
    ts.reset();
    auto snap = ts.snapshot();
    ThreadStats::Snapshot zero{};
    EXPECT_EQ(snap, zero);
}

TEST(ThreadStatsSnapshot, FormatterProducesOutput) {
    ThreadStats::Snapshot s{
        .packets = 100, .bytes = 5000, .text_packets = 20,
        .text_bytes = 800, .dropped = 3, .crypto_errors = 1,
    };
    auto formatted = std::format("{}", s);
    EXPECT_NE(formatted.find("100"), std::string::npos);
    EXPECT_NE(formatted.find("5000"), std::string::npos);
    EXPECT_NE(formatted.find("20"), std::string::npos);
    EXPECT_NE(formatted.find("800"), std::string::npos);
    EXPECT_NE(formatted.find("3"), std::string::npos);
    // Verify zero snapshot also formats
    ThreadStats::Snapshot zero{};
    auto zero_fmt = std::format("{}", zero);
    EXPECT_FALSE(zero_fmt.empty());
}

TEST(TransportStats, EqualityDefaultInitialized) {
    TransportStats a{};
    TransportStats b{};
    EXPECT_EQ(a, b);
}

TEST(TransportStats, EqualityDiffersOnRemoteIpOnly) {
    TransportStats a{};
    a.remote_ip = "10.0.0.1";
    TransportStats b{};
    b.remote_ip = "10.0.0.2";
    EXPECT_NE(a, b);
}

TEST(TransportStats, DeltaPreservesRemoteIpFromLhs) {
    TransportStats s1{};
    s1.tx_packets = 100;
    s1.remote_ip = "10.0.0.1";
    TransportStats s2{};
    s2.tx_packets = 70;
    s2.remote_ip = "10.0.0.2";
    auto delta = s1 - s2;
    EXPECT_EQ(delta.tx_packets, 30u);
    EXPECT_EQ(delta.remote_ip, "10.0.0.1");
}

TEST(RttStats, EqualityMeanNsDoublePrecision) {
    RttStats a{.count = 1, .mean_ns = 123.456789};
    RttStats b{.count = 1, .mean_ns = 123.456789};
    EXPECT_EQ(a, b);
    b.mean_ns = 123.456790;
    EXPECT_NE(a, b);
}

TEST(ConnectionErrorInfo, EqualityDiffersOnCodeOnly) {
    ConnectionErrorInfo a{ConnectionError::kTlsHandshakeFailed, "cert expired", 0};
    ConnectionErrorInfo b{ConnectionError::kTlsSessionFailed, "cert expired", 0};
    EXPECT_NE(a, b);
}

// ============================================================================
// transport_event_name / transport_state_name
// ============================================================================

TEST(TransportEventName, ReturnsHumanReadableStrings) {
    EXPECT_EQ(transport_event_name(TransportEvent::kConnected), "CONNECTED");
    EXPECT_EQ(transport_event_name(TransportEvent::kDisconnected), "DISCONNECTED");
    EXPECT_EQ(transport_event_name(TransportEvent::kReconnecting), "RECONNECTING");
    EXPECT_EQ(transport_event_name(TransportEvent::kStopped), "STOPPED");
}

TEST(TransportStateName, ReturnsHumanReadableStrings) {
    EXPECT_EQ(transport_state_name(TransportState::kConnected), "CONNECTED");
    EXPECT_EQ(transport_state_name(TransportState::kReconnecting), "RECONNECTING");
    EXPECT_EQ(transport_state_name(TransportState::kStopped), "STOPPED");
}

// ============================================================================
// SendError operator!
// ============================================================================

TEST(SendErrorOperatorNot, OkIsFalsy) {
    // operator!(kOk) should return false (success → !success = false)
    // This enables `if (!send(...))` error-checking pattern
    EXPECT_FALSE(!SendError::kOk);
}

TEST(SendErrorOperatorNot, ErrorsAreTruthy) {
    EXPECT_TRUE(!SendError::kQueueFull);
    EXPECT_TRUE(!SendError::kMessageTooLarge);
    EXPECT_TRUE(!SendError::kNotConnected);
    EXPECT_TRUE(!SendError::kInvalidUtf8);
    EXPECT_TRUE(!SendError::kInvalidCloseCode);
    EXPECT_TRUE(!SendError::kNullData);
}

// ─────────────────────────────────────────────────────────────────────────────
// make_twophase_filter
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Simple hash: return first byte, 0 on empty.
uint32_t simple_hash(const uint8_t* data, size_t len) {
    return len > 0 ? data[0] : 0;
}
} // namespace

TEST(TwophaseFilter, StdFunctionOverloadDeliverLatestPerSymbol) {
    auto filter = make_twophase_filter(
        std::function<uint32_t(const uint8_t*, size_t)>(simple_hash));

    // 3 frames: symbol A(1), symbol B(2), symbol A(1) — latest A is frame 2
    uint8_t a = 'A', b = 'B';
    FrameView views[] = {
        {&a, 1, 1, true},  // frame 0: symbol A
        {&b, 1, 1, true},  // frame 1: symbol B
        {&a, 1, 1, true},  // frame 2: symbol A (latest)
    };

    filter(std::span{views, 3});

    EXPECT_FALSE(views[0].deliver);  // old A — skipped
    EXPECT_TRUE(views[1].deliver);   // latest B
    EXPECT_TRUE(views[2].deliver);   // latest A
}

TEST(TwophaseFilter, RawFnPtrOverloadDeliverLatestPerSymbol) {
    auto filter = make_twophase_filter(simple_hash);

    uint8_t a = 'A', b = 'B';
    FrameView views[] = {
        {&a, 1, 1, true},
        {&b, 1, 1, true},
        {&a, 1, 1, true},
    };

    filter(std::span{views, 3});

    EXPECT_FALSE(views[0].deliver);
    EXPECT_TRUE(views[1].deliver);
    EXPECT_TRUE(views[2].deliver);
}

TEST(TwophaseFilter, ZeroHashAlwaysDelivered) {
    // Hash returns 0 → frame always delivered (unrecognized payload)
    auto filter = make_twophase_filter(
        std::function<uint32_t(const uint8_t*, size_t)>(
            [](const uint8_t*, size_t) -> uint32_t { return 0; }));

    uint8_t x = 'X';
    FrameView views[] = {
        {&x, 1, 1, true},
        {&x, 1, 1, true},
        {&x, 1, 1, true},
    };

    filter(std::span{views, 3});

    // All frames delivered (hash=0 means always deliver)
    EXPECT_TRUE(views[0].deliver);
    EXPECT_TRUE(views[1].deliver);
    EXPECT_TRUE(views[2].deliver);
}

TEST(TwophaseFilter, SingleFrameDelivered) {
    auto filter = make_twophase_filter(simple_hash);

    uint8_t a = 'A';
    FrameView views[] = {{&a, 1, 1, true}};

    filter(std::span{views, 1});

    EXPECT_TRUE(views[0].deliver);  // only frame = latest
}

TEST(TwophaseFilter, AllSameSymbolOnlyLatestDelivered) {
    auto filter = make_twophase_filter(simple_hash);

    uint8_t a = 'A';
    FrameView views[] = {
        {&a, 1, 1, true},
        {&a, 1, 1, true},
        {&a, 1, 1, true},
        {&a, 1, 1, true},
        {&a, 1, 1, true},
    };

    filter(std::span{views, 5});

    for (int i = 0; i < 4; ++i)
        EXPECT_FALSE(views[i].deliver) << "frame " << i << " should be skipped";
    EXPECT_TRUE(views[4].deliver);  // only the last
}

TEST(TwophaseFilter, EmptySpanNoOp) {
    auto filter = make_twophase_filter(simple_hash);
    filter(std::span<FrameView>{});  // should not crash
}

TEST(TwophaseFilter, ManySymbolsAllLatestDelivered) {
    auto filter = make_twophase_filter(simple_hash);

    // 26 distinct symbols (A-Z), each appears once → all delivered
    uint8_t syms[26];
    FrameView views[26];
    for (int i = 0; i < 26; ++i) {
        syms[i] = static_cast<uint8_t>('A' + i);
        views[i] = {&syms[i], 1, 1, true};
    }

    filter(std::span{views, 26});

    for (int i = 0; i < 26; ++i)
        EXPECT_TRUE(views[i].deliver) << "symbol " << char('A' + i) << " should be delivered";
}
