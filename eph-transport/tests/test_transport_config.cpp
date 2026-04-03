/// @file test_transport_config.cpp
/// Tests for TransportConfig validation, URL parsing, serialization, and warnings.

#include <gtest/gtest.h>

#include "eph/transport/transport_types.hpp"

using namespace eph::net;

// =======================================================================
// Helper: create a valid base config (all validation passes)
// =======================================================================

static TransportConfig valid_config() {
    TransportConfig cfg;
    cfg.remote_host = "example.com";
    cfg.remote_port = 443;
    cfg.ws_path = "/ws";
    cfg.skip_utf8_validation = false;  // avoid default warning
    return cfg;
}

// =======================================================================
// validate() — happy path
// =======================================================================

TEST(TransportConfigValidate, ValidConfigReturnsEmpty) {
    auto cfg = valid_config();
    EXPECT_TRUE(cfg.validate().empty());
}

// =======================================================================
// validate() — each validation branch
// =======================================================================

TEST(TransportConfigValidate, EmptyRemoteHostRejected) {
    auto cfg = valid_config();
    cfg.remote_host.clear();
    EXPECT_EQ(cfg.validate(), "remote_host must not be empty");
}

TEST(TransportConfigValidate, ControlCharsInHostRejected) {
    auto cfg = valid_config();
    cfg.remote_host = "evil\r\nhost";
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("control characters"), std::string_view::npos);
}

TEST(TransportConfigValidate, ZeroPortRejected) {
    auto cfg = valid_config();
    cfg.remote_port = 0;
    EXPECT_EQ(cfg.validate(), "remote_port must be > 0");
}

TEST(TransportConfigValidate, EmptyWsPathRejected) {
    auto cfg = valid_config();
    cfg.ws_path.clear();
    EXPECT_EQ(cfg.validate(), "ws_path must not be empty");
}

TEST(TransportConfigValidate, WsPathMissingLeadingSlashRejected) {
    auto cfg = valid_config();
    cfg.ws_path = "no-slash";
    EXPECT_EQ(cfg.validate(), "ws_path must start with '/'");
}

TEST(TransportConfigValidate, ControlCharsInWsPathRejected) {
    auto cfg = valid_config();
    cfg.ws_path = "/path\r\n/injection";
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("control characters"), std::string_view::npos);
}

TEST(TransportConfigValidate, ZeroTxBurstSizeRejected) {
    auto cfg = valid_config();
    cfg.tx_burst_size = 0;
    EXPECT_EQ(cfg.validate(), "tx_burst_size must be > 0");
}

TEST(TransportConfigValidate, ZeroRxBurstSizeRejected) {
    auto cfg = valid_config();
    cfg.rx_burst_size = 0;
    EXPECT_EQ(cfg.validate(), "rx_burst_size must be > 0");
}

TEST(TransportConfigValidate, NegativeMaxReconnectAttemptsRejected) {
    auto cfg = valid_config();
    cfg.max_reconnect_attempts = -1;
    EXPECT_EQ(cfg.validate(), "max_reconnect_attempts must be >= 0");
}

TEST(TransportConfigValidate, ClientCertWithoutKeyRejected) {
    auto cfg = valid_config();
    cfg.client_cert_path = "/cert.pem";
    // client_key_path left empty
    auto err = cfg.validate();
    EXPECT_NE(err.find("both be set or both empty"), std::string_view::npos);
}

TEST(TransportConfigValidate, ClientKeyWithoutCertRejected) {
    auto cfg = valid_config();
    cfg.client_key_path = "/key.pem";
    auto err = cfg.validate();
    EXPECT_NE(err.find("both be set or both empty"), std::string_view::npos);
}

TEST(TransportConfigValidate, ClientCertWithoutTlsRejected) {
    auto cfg = valid_config();
    cfg.use_tls = false;
    cfg.client_cert_path = "/cert.pem";
    cfg.client_key_path = "/key.pem";
    EXPECT_EQ(cfg.validate(), "client certificates require use_tls=true");
}

TEST(TransportConfigValidate, ZeroTcpTimeoutRejected) {
    auto cfg = valid_config();
    cfg.tcp_timeout = std::chrono::milliseconds{0};
    EXPECT_EQ(cfg.validate(), "tcp_timeout must be positive");
}

TEST(TransportConfigValidate, ZeroTlsTimeoutRejected) {
    auto cfg = valid_config();
    cfg.tls_timeout = std::chrono::milliseconds{0};
    EXPECT_EQ(cfg.validate(), "tls_timeout must be positive");
}

TEST(TransportConfigValidate, ZeroWsTimeoutRejected) {
    auto cfg = valid_config();
    cfg.ws_timeout = std::chrono::milliseconds{0};
    EXPECT_EQ(cfg.validate(), "ws_timeout must be positive");
}

TEST(TransportConfigValidate, ReconnectEnabledWithZeroIntervalRejected) {
    auto cfg = valid_config();
    cfg.max_reconnect_attempts = 5;
    cfg.reconnect_interval = std::chrono::milliseconds{0};
    auto err = cfg.validate();
    EXPECT_NE(err.find("reconnect_interval must be positive"), std::string_view::npos);
}

TEST(TransportConfigValidate, DisabledReconnectAllowsZeroInterval) {
    auto cfg = valid_config();
    cfg.max_reconnect_attempts = 0;
    cfg.reconnect_interval = std::chrono::milliseconds{0};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TransportConfigValidate, PongTimeoutWithoutPingRejected) {
    auto cfg = valid_config();
    cfg.ping_interval = std::chrono::seconds{0};
    cfg.pong_timeout = std::chrono::seconds{5};
    auto err = cfg.validate();
    EXPECT_NE(err.find("pong_timeout requires ping_interval"), std::string_view::npos);
}

TEST(TransportConfigValidate, PongTimeoutGreaterThanPingRejected) {
    auto cfg = valid_config();
    cfg.ping_interval = std::chrono::seconds{10};
    cfg.pong_timeout = std::chrono::seconds{10};  // equal is also rejected
    auto err = cfg.validate();
    EXPECT_NE(err.find("pong_timeout must be less than ping_interval"), std::string_view::npos);
}

TEST(TransportConfigValidate, PongTimeoutLessThanPingAccepted) {
    auto cfg = valid_config();
    cfg.ping_interval = std::chrono::seconds{10};
    cfg.pong_timeout = std::chrono::seconds{5};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TransportConfigValidate, ExtraHeadersWithoutCrLfRejected) {
    auto cfg = valid_config();
    cfg.extra_headers = "X-Custom: value";  // missing \r\n
    auto err = cfg.validate();
    EXPECT_NE(err.find("extra_headers must end with"), std::string_view::npos);
}

TEST(TransportConfigValidate, ExtraHeadersWithCrLfAccepted) {
    auto cfg = valid_config();
    cfg.extra_headers = "X-Custom: value\r\n";
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TransportConfigValidate, SubprotocolWithCrRejected) {
    auto cfg = valid_config();
    cfg.ws_subprotocol = "proto\rinjection";
    auto err = cfg.validate();
    EXPECT_NE(err.find("must not contain CR or LF"), std::string_view::npos);
}

TEST(TransportConfigValidate, SubprotocolWithLfRejected) {
    auto cfg = valid_config();
    cfg.ws_subprotocol = "proto\ninjection";
    auto err = cfg.validate();
    EXPECT_NE(err.find("must not contain CR or LF"), std::string_view::npos);
}

TEST(TransportConfigValidate, TxCpuBelowMinusOneRejected) {
    auto cfg = valid_config();
    cfg.tx_cpu = -2;
    EXPECT_EQ(cfg.validate(), "tx_cpu must be >= -1 (-1 = no pinning)");
}

TEST(TransportConfigValidate, RxCpuBelowMinusOneRejected) {
    auto cfg = valid_config();
    cfg.rx_cpu = -2;
    EXPECT_EQ(cfg.validate(), "rx_cpu must be >= -1 (-1 = no pinning)");
}

// =======================================================================
// from_url() — happy paths
// =======================================================================

TEST(TransportConfigFromUrl, WssSimplePath) {
    auto result = TransportConfig::from_url("wss://host.example/ws/v2");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_host, "host.example");
    EXPECT_EQ(result->remote_port, 443);
    EXPECT_EQ(result->ws_path, "/ws/v2");
    EXPECT_TRUE(result->use_tls);
}

TEST(TransportConfigFromUrl, WsSimplePath) {
    auto result = TransportConfig::from_url("ws://localhost/stream");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_host, "localhost");
    EXPECT_EQ(result->remote_port, 80);
    EXPECT_EQ(result->ws_path, "/stream");
    EXPECT_FALSE(result->use_tls);
}

TEST(TransportConfigFromUrl, ExplicitPort) {
    auto result = TransportConfig::from_url("wss://api.example:8443/v1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_host, "api.example");
    EXPECT_EQ(result->remote_port, 8443);
    EXPECT_EQ(result->ws_path, "/v1");
}

TEST(TransportConfigFromUrl, HostOnlyDefaultsPathToSlash) {
    auto result = TransportConfig::from_url("wss://host.example");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_host, "host.example");
    EXPECT_EQ(result->ws_path, "/");
}

TEST(TransportConfigFromUrl, HostWithPortNoPath) {
    auto result = TransportConfig::from_url("wss://host.example:9090");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_port, 9090);
    EXPECT_EQ(result->ws_path, "/");
}

TEST(TransportConfigFromUrl, QueryStringIncludedInPath) {
    auto result = TransportConfig::from_url("wss://host.example/path?key=val&a=b");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->ws_path, "/path?key=val&a=b");
}

TEST(TransportConfigFromUrl, Ipv6Address) {
    auto result = TransportConfig::from_url("wss://[::1]:8443/ws");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_host, "::1");
    EXPECT_EQ(result->remote_port, 8443);
    EXPECT_EQ(result->ws_path, "/ws");
}

TEST(TransportConfigFromUrl, Ipv6NoPort) {
    auto result = TransportConfig::from_url("wss://[fe80::1]/ws");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_host, "fe80::1");
    EXPECT_EQ(result->remote_port, 443);
}

TEST(TransportConfigFromUrl, LeadingTrailingWhitespaceStripped) {
    auto result = TransportConfig::from_url("  wss://host.example/ws  ");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_host, "host.example");
}

// =======================================================================
// from_url() — error paths
// =======================================================================

TEST(TransportConfigFromUrl, InvalidSchemeRejected) {
    auto result = TransportConfig::from_url("http://host/path");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("ws:// or wss://"), std::string::npos);
}

TEST(TransportConfigFromUrl, MissingHostRejected) {
    auto result = TransportConfig::from_url("wss://");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("host"), std::string::npos);
}

TEST(TransportConfigFromUrl, EmptyHostBeforePortRejected) {
    auto result = TransportConfig::from_url("wss://:8080/path");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("empty host"), std::string::npos);
}

TEST(TransportConfigFromUrl, InvalidPortRejected) {
    auto result = TransportConfig::from_url("wss://host:abc/path");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("invalid port"), std::string::npos);
}

TEST(TransportConfigFromUrl, PortZeroRejected) {
    auto result = TransportConfig::from_url("wss://host:0/path");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("port out of range"), std::string::npos);
}

TEST(TransportConfigFromUrl, PortOverflowRejected) {
    auto result = TransportConfig::from_url("wss://host:99999/path");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("port out of range"), std::string::npos);
}

TEST(TransportConfigFromUrl, Ipv6MissingCloseBracketRejected) {
    auto result = TransportConfig::from_url("wss://[::1/path");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("closing ']'"), std::string::npos);
}

TEST(TransportConfigFromUrl, EmptyIpv6AddressRejected) {
    auto result = TransportConfig::from_url("wss://[]/path");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("empty IPv6"), std::string::npos);
}

TEST(TransportConfigFromUrl, ControlCharsInHostRejected) {
    auto result = TransportConfig::from_url("wss://evil\x01host/path");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("control characters"), std::string::npos);
}

TEST(TransportConfigFromUrl, EmptyPortAfterColonRejected) {
    auto result = TransportConfig::from_url("wss://host:/path");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("empty port"), std::string::npos);
}

// =======================================================================
// to_url() — roundtrip
// =======================================================================

TEST(TransportConfigToUrl, RoundtripWss) {
    auto original = TransportConfig::from_url("wss://host.example:8443/ws/v2");
    ASSERT_TRUE(original.has_value());
    auto url = original->to_url();
    auto roundtrip = TransportConfig::from_url(url);
    ASSERT_TRUE(roundtrip.has_value());
    EXPECT_EQ(original->remote_host, roundtrip->remote_host);
    EXPECT_EQ(original->remote_port, roundtrip->remote_port);
    EXPECT_EQ(original->ws_path, roundtrip->ws_path);
    EXPECT_EQ(original->use_tls, roundtrip->use_tls);
}

TEST(TransportConfigToUrl, DefaultPortOmitted) {
    auto cfg = TransportConfig::from_url("wss://host.example/ws");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->to_url(), "wss://host.example/ws");
}

TEST(TransportConfigToUrl, NonDefaultPortIncluded) {
    auto cfg = TransportConfig::from_url("wss://host.example:9090/ws");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->to_url(), "wss://host.example:9090/ws");
}

TEST(TransportConfigToUrl, Ipv6BracketEnclosed) {
    TransportConfig cfg;
    cfg.remote_host = "::1";
    cfg.remote_port = 8443;
    cfg.ws_path = "/ws";
    cfg.use_tls = true;
    EXPECT_EQ(cfg.to_url(), "wss://[::1]:8443/ws");
}

// =======================================================================
// warnings()
// =======================================================================

TEST(TransportConfigWarnings, NoWarningsForCleanConfig) {
    auto cfg = valid_config();
    cfg.skip_utf8_validation = false;  // avoid the utf8 warning
    EXPECT_TRUE(cfg.warnings().empty());
}

TEST(TransportConfigWarnings, VerifyPeerWithoutTls) {
    auto cfg = valid_config();
    cfg.use_tls = false;
    cfg.verify_peer = true;
    cfg.skip_utf8_validation = false;
    auto w = cfg.warnings();
    EXPECT_GE(w.size(), 1u);
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("verify_peer") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TransportConfigWarnings, CaCertWithoutTls) {
    auto cfg = valid_config();
    cfg.use_tls = false;
    cfg.verify_peer = false;
    cfg.ca_cert_path = "/some/ca.pem";
    cfg.skip_utf8_validation = false;
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("ca_cert_path") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TransportConfigWarnings, LargeTxBurstSize) {
    auto cfg = valid_config();
    cfg.tx_burst_size = 2000;
    cfg.skip_utf8_validation = false;
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("tx_burst_size") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(TransportConfigWarnings, SkipUtf8ValidationWarns) {
    auto cfg = valid_config();
    cfg.skip_utf8_validation = true;
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("skip_utf8_validation") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// =======================================================================
// dump() and to_json()
// =======================================================================

TEST(TransportConfigDump, ContainsHostAndPort) {
    auto cfg = valid_config();
    auto d = cfg.dump();
    EXPECT_NE(d.find("example.com"), std::string::npos);
    EXPECT_NE(d.find("443"), std::string::npos);
}

TEST(TransportConfigJson, ContainsRequiredFields) {
    auto cfg = valid_config();
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"remote_host\""), std::string::npos);
    EXPECT_NE(j.find("\"remote_port\""), std::string::npos);
    EXPECT_NE(j.find("\"use_tls\""), std::string::npos);
    EXPECT_NE(j.find("\"ws_path\""), std::string::npos);
}

// =======================================================================
// Equality operator
// =======================================================================

TEST(TransportConfigEquality, IdenticalConfigsAreEqual) {
    auto a = valid_config();
    auto b = valid_config();
    EXPECT_EQ(a, b);
}

TEST(TransportConfigEquality, DifferentHostsAreNotEqual) {
    auto a = valid_config();
    auto b = valid_config();
    b.remote_host = "other.example";
    EXPECT_NE(a, b);
}

TEST(TransportConfigEquality, DifferentPortsAreNotEqual) {
    auto a = valid_config();
    auto b = valid_config();
    b.remote_port = 8080;
    EXPECT_NE(a, b);
}

// =======================================================================
// validate() — additional edge cases
// =======================================================================

TEST(TransportConfigValidate, NegativePingIntervalRejected) {
    auto cfg = valid_config();
    cfg.ping_interval = std::chrono::seconds{-1};
    auto err = cfg.validate();
    EXPECT_NE(err.find("ping_interval must be >= 0"), std::string_view::npos);
}

TEST(TransportConfigValidate, NegativePongTimeoutRejected) {
    auto cfg = valid_config();
    cfg.pong_timeout = std::chrono::seconds{-1};
    auto err = cfg.validate();
    EXPECT_NE(err.find("pong_timeout must be >= 0"), std::string_view::npos);
}

TEST(TransportConfigValidate, ValidMtlsConfig) {
    auto cfg = valid_config();
    cfg.use_tls = true;
    cfg.client_cert_path = "/cert.pem";
    cfg.client_key_path = "/key.pem";
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TransportConfigValidate, NegativeTcpTimeoutRejected) {
    auto cfg = valid_config();
    cfg.tcp_timeout = std::chrono::milliseconds{-1};
    auto err = cfg.validate();
    EXPECT_EQ(err, "tcp_timeout must be positive");
}

// =======================================================================
// warnings() — additional coverage
// =======================================================================

TEST(TransportConfigWarnings, LargeRxBurstSizeWarns) {
    auto cfg = valid_config();
    cfg.rx_burst_size = 2000;
    cfg.skip_utf8_validation = false;
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("rx_burst_size") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// =======================================================================
// from_url() — additional edge cases
// =======================================================================

TEST(TransportConfigFromUrl, WsPlainRoundtrip) {
    auto result = TransportConfig::from_url("ws://localhost:8080/stream");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_host, "localhost");
    EXPECT_EQ(result->remote_port, 8080);
    EXPECT_EQ(result->ws_path, "/stream");
    EXPECT_FALSE(result->use_tls);
    // Roundtrip
    auto url = result->to_url();
    EXPECT_EQ(url, "ws://localhost:8080/stream");
}

TEST(TransportConfigFromUrl, WsDefaultPort80OmittedInToUrl) {
    auto result = TransportConfig::from_url("ws://host.example/ws");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_port, 80);
    EXPECT_EQ(result->to_url(), "ws://host.example/ws");
}

TEST(TransportConfigFromUrl, Ipv6WithDefaultPort) {
    TransportConfig cfg;
    cfg.remote_host = "::1";
    cfg.remote_port = 443;
    cfg.ws_path = "/ws";
    cfg.use_tls = true;
    // Default port omitted in URL
    EXPECT_EQ(cfg.to_url(), "wss://[::1]/ws");
}

TEST(TransportConfigFromUrl, FragmentInPathPreserved) {
    auto result = TransportConfig::from_url("wss://host.example/path#fragment");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->ws_path, "/path#fragment");
}

TEST(TransportConfigFromUrl, PortMax65535Accepted) {
    auto result = TransportConfig::from_url("wss://host:65535/ws");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->remote_port, 65535);
}

TEST(TransportConfigFromUrl, Port65536Rejected) {
    auto result = TransportConfig::from_url("wss://host:65536/ws");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("port out of range"), std::string::npos);
}

// =======================================================================
// to_json() / dump() — roundtrip and format checks
// =======================================================================

TEST(TransportConfigJson, JsonContainsAllFields) {
    auto cfg = valid_config();
    cfg.ws_subprotocol = "graphql-ws";
    cfg.extra_headers = "X-Custom: val\r\n";
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"ws_subprotocol\":\"graphql-ws\""), std::string::npos);
    EXPECT_NE(j.find("\"extra_headers\""), std::string::npos);
    EXPECT_NE(j.find("\"tx_burst_size\""), std::string::npos);
    EXPECT_NE(j.find("\"rx_burst_size\""), std::string::npos);
}

TEST(TransportConfigDump, DumpContainsSubprotocol) {
    auto cfg = valid_config();
    cfg.ws_subprotocol = "graphql-ws";
    auto d = cfg.dump();
    EXPECT_NE(d.find("graphql-ws"), std::string::npos);
}

TEST(TransportConfigDump, DumpNoSubprotocolShowsNone) {
    auto cfg = valid_config();
    auto d = cfg.dump();
    EXPECT_NE(d.find("(none)"), std::string::npos);
}
