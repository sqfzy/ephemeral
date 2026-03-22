#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/connector.hpp"

using namespace eph::dpdk;

// ─────────────────────────────────────────────────────────────────────────────
// resolve_hostname — dotted-decimal fast path (no DNS needed)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ResolveHostname, DottedDecimalParsedDirectly) {
    auto result = resolve_hostname("192.168.1.1");
    ASSERT_TRUE(result.has_value()) << result.error();
    // 192.168.1.1 in host byte order = 0xC0A80101
    EXPECT_EQ(*result, 0xC0A80101u);
}

TEST(ResolveHostname, LoopbackAddress) {
    auto result = resolve_hostname("127.0.0.1");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, 0x7F000001u);
}

TEST(ResolveHostname, MaxOctets) {
    auto result = resolve_hostname("255.255.255.255");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, 0xFFFFFFFFu);
}

TEST(ResolveHostname, MinNonZeroAddress) {
    auto result = resolve_hostname("0.0.0.1");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// resolve_hostname — DNS resolution (uses kernel stack, works with net_null)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ResolveHostname, LocalhostResolvesViaKernelDns) {
    auto result = resolve_hostname("localhost");
    ASSERT_TRUE(result.has_value()) << result.error();
    // localhost typically resolves to 127.0.0.1
    EXPECT_EQ(*result, 0x7F000001u);
}

// ─────────────────────────────────────────────────────────────────────────────
// resolve_hostname — error paths
// ─────────────────────────────────────────────────────────────────────────────

TEST(ResolveHostname, NonExistentHostReturnsErrorWithNicHint) {
    auto result = resolve_hostname("this.host.definitely.does.not.exist.invalid");
    ASSERT_FALSE(result.has_value());
    // Error message should contain the hostname
    EXPECT_NE(result.error().find("this.host.definitely.does.not.exist.invalid"),
              std::string::npos);
    // Error message should contain the exclusive-mode PMD hint
    EXPECT_NE(result.error().find("exclusive-mode PMD"), std::string::npos);
}

TEST(ResolveHostname, EmptyStringReturnsError) {
    auto result = resolve_hostname("");
    ASSERT_FALSE(result.has_value());
}

TEST(ResolveHostname, InvalidFormatReturnsError) {
    // Not a valid IP and not a resolvable hostname
    auto result = resolve_hostname("999.999.999.999");
    // parse_ipv4 should return 0 for this, then DNS will fail
    ASSERT_FALSE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// ConnectorConfig — default values
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectorConfig, DefaultValues) {
    ConnectorConfig cfg{};
    EXPECT_EQ(cfg.platform.port_id, 0);
    EXPECT_EQ(cfg.platform.nb_rx_queues, 1);
    EXPECT_EQ(cfg.platform.nb_tx_queues, 1);
    EXPECT_EQ(cfg.local_port, 0);
    EXPECT_EQ(cfg.tx_queue_id, 0);
    EXPECT_EQ(cfg.rx_queue_id, 0);
    EXPECT_EQ(cfg.arp_timeout, std::chrono::milliseconds{3000});
    EXPECT_EQ(cfg.connect_timeout, std::chrono::milliseconds{5000});
    EXPECT_TRUE(cfg.local_ip.empty());
    EXPECT_TRUE(cfg.gateway_ip.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// connect(cfg, tp_cfg, server_ip) — validation error paths
//
// These tests exercise the early validation checks in connect() without
// needing a real network.  They use net_null (via dpdk_test_env), which
// provides EAL but the port is consumed by the platform test suite, so
// we focus on the checks that fire before Platform::create().
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectValidation, EmptyLocalIpReturnsError) {
    ConnectorConfig cfg{.local_ip = "", .gateway_ip = "10.0.0.1"};
    eph::net::TransportConfig tp_cfg{.remote_host = "example.com"};
    auto result = connect(cfg, tp_cfg, 0x0A000002u);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("local_ip"), std::string::npos);
}

TEST(ConnectValidation, EmptyGatewayIpReturnsError) {
    ConnectorConfig cfg{.local_ip = "10.0.0.2", .gateway_ip = ""};
    eph::net::TransportConfig tp_cfg{.remote_host = "example.com"};
    auto result = connect(cfg, tp_cfg, 0x0A000002u);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("gateway_ip"), std::string::npos);
}

TEST(ConnectValidation, ZeroServerIpReturnsError) {
    ConnectorConfig cfg{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    eph::net::TransportConfig tp_cfg{.remote_host = "example.com"};
    auto result = connect(cfg, tp_cfg, 0u);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("server_ip"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// connect(cfg, tp_cfg) — hostname overload validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectHostnameOverload, EmptyRemoteHostReturnsError) {
    ConnectorConfig cfg{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    eph::net::TransportConfig tp_cfg{.remote_host = ""};
    auto result = connect(cfg, tp_cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("remote_host"), std::string::npos);
}

TEST(ConnectHostnameOverload, UnresolvableHostReturnsError) {
    ConnectorConfig cfg{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    eph::net::TransportConfig tp_cfg{
        .remote_host = "this.host.definitely.does.not.exist.invalid",
    };
    auto result = connect(cfg, tp_cfg);
    ASSERT_FALSE(result.has_value());
    // Should contain the DNS error with exclusive-mode hint
    EXPECT_NE(result.error().find("DNS resolution failed"), std::string::npos);
}

TEST(ConnectHostnameOverload, InvalidLocalIpStillCaughtAfterDns) {
    // DNS succeeds (dotted-decimal) but local_ip is empty → validation error
    ConnectorConfig cfg{.local_ip = "", .gateway_ip = "10.0.0.1"};
    eph::net::TransportConfig tp_cfg{.remote_host = "10.0.0.99"};
    auto result = connect(cfg, tp_cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("local_ip"), std::string::npos);
}
