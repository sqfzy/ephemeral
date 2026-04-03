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
// ConnectorOptions — default values
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectorOptions, DefaultValues) {
    ConnectorOptions opts{};
    EXPECT_EQ(opts.platform.port_id, 0);
    EXPECT_EQ(opts.platform.nb_rx_queues, 1);
    EXPECT_EQ(opts.platform.nb_tx_queues, 1);
    EXPECT_EQ(opts.local_port, 0);
    EXPECT_EQ(opts.tx_queue_id, 0);
    EXPECT_EQ(opts.rx_queue_id, 0);
    EXPECT_EQ(opts.arp_timeout, std::chrono::milliseconds{3000});
    EXPECT_EQ(opts.connect_timeout, std::chrono::milliseconds{5000});
}

TEST(DpdkEndpoint, RequiredFieldsMustBeProvided) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    EXPECT_EQ(ep.local_ip, "10.0.0.2");
    EXPECT_EQ(ep.gateway_ip, "10.0.0.1");
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
    DpdkEndpoint ep{.local_ip = "", .gateway_ip = "10.0.0.1"};
    eph::net::TransportConfig tp_cfg{.remote_host = "example.com"};
    auto result = connect(ep, tp_cfg, 0x0A000002u);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("local_ip"), std::string::npos);
}

TEST(ConnectValidation, EmptyGatewayIpReturnsError) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = ""};
    eph::net::TransportConfig tp_cfg{.remote_host = "example.com"};
    auto result = connect(ep, tp_cfg, 0x0A000002u);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("gateway_ip"), std::string::npos);
}

TEST(ConnectValidation, ZeroServerIpReturnsError) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    eph::net::TransportConfig tp_cfg{.remote_host = "example.com"};
    auto result = connect(ep, tp_cfg, 0u);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("server_ip"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// connect(cfg, tp_cfg) — hostname overload validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectHostnameOverload, EmptyRemoteHostReturnsError) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    eph::net::TransportConfig tp_cfg{.remote_host = ""};
    auto result = connect(ep, tp_cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("remote_host"), std::string::npos);
}

TEST(ConnectHostnameOverload, UnresolvableHostReturnsError) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    eph::net::TransportConfig tp_cfg{
        .remote_host = "this.host.definitely.does.not.exist.invalid",
    };
    auto result = connect(ep, tp_cfg);
    ASSERT_FALSE(result.has_value());
    // Error may be "DNS resolution failed" (kernel DNS) or contain "DNS fallback"
    // (DPDK DNS path tried but Platform/ARP failed on test hardware)
    auto& err = result.error();
    EXPECT_TRUE(err.find("DNS") != std::string::npos ||
                err.find("resolution") != std::string::npos)
        << "Expected DNS-related error, got: " << err;
}

TEST(ConnectHostnameOverload, InvalidLocalIpStillCaughtAfterDns) {
    // DNS succeeds (dotted-decimal) but local_ip is empty → validation error
    DpdkEndpoint ep{.local_ip = "", .gateway_ip = "10.0.0.1"};
    eph::net::TransportConfig tp_cfg{.remote_host = "10.0.0.99"};
    auto result = connect(ep, tp_cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("local_ip"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// DpdkEndpoint observability
// ─────────────────────────────────────────────────────────────────────────────

TEST(DpdkEndpoint, ValidateEmptyLocalIpFails) {
    DpdkEndpoint ep{.local_ip = "", .gateway_ip = "10.0.0.1"};
    EXPECT_FALSE(ep.validate().empty());
}

TEST(DpdkEndpoint, ValidateEmptyGatewayFails) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = ""};
    EXPECT_FALSE(ep.validate().empty());
}

TEST(DpdkEndpoint, ValidateValidEndpointPasses) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    EXPECT_TRUE(ep.validate().empty());
}

TEST(DpdkEndpoint, EqualityDefaultBehavior) {
    DpdkEndpoint a{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    DpdkEndpoint b{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    EXPECT_EQ(a, b);
}

TEST(DpdkEndpoint, EqualityDetectsDifference) {
    DpdkEndpoint a{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    DpdkEndpoint b{.local_ip = "10.0.0.3", .gateway_ip = "10.0.0.1"};
    EXPECT_NE(a, b);
}

TEST(DpdkEndpoint, ToJsonContainsFields) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    auto json = ep.to_json();
    EXPECT_NE(json.find("\"local_ip\":\"10.0.0.2\""), std::string::npos);
    EXPECT_NE(json.find("\"gateway_ip\":\"10.0.0.1\""), std::string::npos);
}

TEST(DpdkEndpoint, FormatterProducesNonEmpty) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    auto s = std::format("{}", ep);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("10.0.0.2"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// ConnectorOptions observability
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectorOptions, ValidateDefaultPasses) {
    ConnectorOptions opts{};
    EXPECT_TRUE(opts.validate().empty());
}

TEST(ConnectorOptions, ValidateZeroArpTimeoutFails) {
    ConnectorOptions opts{.arp_timeout = std::chrono::milliseconds{0}};
    EXPECT_FALSE(opts.validate().empty());
}

TEST(ConnectorOptions, ValidateTxQueueOutOfRangeFails) {
    ConnectorOptions opts{};
    opts.tx_queue_id = 5;  // default nb_tx_queues is 1
    auto err = opts.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("tx_queue_id"), std::string_view::npos);
}

TEST(ConnectorOptions, ValidateRxQueueOutOfRangeFails) {
    ConnectorOptions opts{};
    opts.rx_queue_id = 5;  // default nb_rx_queues is 1
    auto err = opts.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("rx_queue_id"), std::string_view::npos);
}

TEST(ConnectorOptions, ValidateQueueIdAtBoundaryPasses) {
    ConnectorOptions opts{};
    opts.platform.nb_tx_queues = 4;
    opts.platform.nb_rx_queues = 4;
    opts.tx_queue_id = 3;  // 3 < 4 = valid
    opts.rx_queue_id = 3;
    EXPECT_TRUE(opts.validate().empty());
}

TEST(ConnectorOptions, ValidateZeroConnectTimeoutFails) {
    ConnectorOptions opts{.connect_timeout = std::chrono::milliseconds{0}};
    EXPECT_FALSE(opts.validate().empty());
}

TEST(ConnectorOptions, EqualityDefaultBehavior) {
    ConnectorOptions a{};
    ConnectorOptions b{};
    EXPECT_EQ(a, b);
}

TEST(ConnectorOptions, ToJsonContainsNestedPlatform) {
    ConnectorOptions opts{.local_port = 5000};
    auto json = opts.to_json();
    EXPECT_NE(json.find("\"local_port\":5000"), std::string::npos);
    EXPECT_NE(json.find("\"port_id\":"), std::string::npos);  // nested platform
    EXPECT_NE(json.find("\"dns\":"), std::string::npos);       // nested dns
    EXPECT_NE(json.find("\"nameserver_ip\":"), std::string::npos);
}

TEST(ConnectorOptions, FormatterProducesNonEmpty) {
    ConnectorOptions opts{};
    auto s = std::format("{}", opts);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(s.find("ConnectorOptions"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// DpdkEndpoint validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(DpdkEndpoint, ValidEndpointPasses) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    EXPECT_TRUE(ep.validate().empty());
}

TEST(DpdkEndpoint, EmptyLocalIpFails) {
    DpdkEndpoint ep{.local_ip = "", .gateway_ip = "10.0.0.1"};
    auto err = ep.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("local_ip"), std::string_view::npos);
}

TEST(DpdkEndpoint, EmptyGatewayIpFails) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = ""};
    auto err = ep.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("gateway_ip"), std::string_view::npos);
}

TEST(DpdkEndpoint, LocalIpWithControlCharsRejected) {
    DpdkEndpoint ep{.local_ip = "10.0\n.0.2", .gateway_ip = "10.0.0.1"};
    auto err = ep.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("control characters"), std::string_view::npos);
}

TEST(DpdkEndpoint, GatewayIpWithControlCharsRejected) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0\r.0.1"};
    auto err = ep.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("control characters"), std::string_view::npos);
}

TEST(DpdkEndpoint, DumpProducesOutput) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    auto s = ep.dump();
    EXPECT_NE(s.find("10.0.0.2"), std::string::npos);
    EXPECT_NE(s.find("10.0.0.1"), std::string::npos);
}

TEST(DpdkEndpoint, ToJsonProducesValidStructure) {
    DpdkEndpoint ep{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    auto json = ep.to_json();
    EXPECT_NE(json.find("\"local_ip\":\"10.0.0.2\""), std::string::npos);
    EXPECT_NE(json.find("\"gateway_ip\":\"10.0.0.1\""), std::string::npos);
}

TEST(DpdkEndpoint, EqualityOperator) {
    DpdkEndpoint a{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    DpdkEndpoint b{.local_ip = "10.0.0.2", .gateway_ip = "10.0.0.1"};
    DpdkEndpoint c{.local_ip = "10.0.0.3", .gateway_ip = "10.0.0.1"};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}
