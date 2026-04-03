#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/tcp.hpp"

using namespace eph::dpdk;

// ─────────────────────────────────────────────────────────────────────────────
// Config used by all tests in this file.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr PlatformConfig test_config{
    .port_id         = 0,
    .nb_rx_queues    = 1,
    .nb_tx_queues    = 1,
    .nb_rx_desc      = 256,
    .nb_tx_desc      = 512,
    .mbuf_pool_size  = 1023,
    .mbuf_cache_size = 32,
    .link_timeout_ms = 0,
};
static_assert(config_ok(test_config), "Test config must be valid");

// ─────────────────────────────────────────────────────────────────────────────
// Tests that don't need a running Platform (config validation, error paths).
// These either reject config before touching DPDK or request a non-existent
// port, so they don't consume the one-shot net_null port.
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformCreate, BadPoolSizeReturnsError) {
    PlatformConfig cfg = test_config;
    cfg.mbuf_pool_size = 1000;
    auto result = Platform::create(cfg);
    EXPECT_FALSE(result.has_value());
}

TEST(PlatformCreate, PortIdOutOfRangeReturnsError) {
    PlatformConfig cfg = test_config;
    cfg.port_id = 99;
    auto result = Platform::create(cfg);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("out of range"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests that need a running Platform.
//
// net_null closes permanently after rte_eth_dev_close(), so we create the
// Platform once per suite and share it.  Tests must NOT destroy the platform.
// ─────────────────────────────────────────────────────────────────────────────

class PlatformTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        auto result = Platform::create(test_config);
        if (!result.has_value()) {
            // On single-port NICs, the port may already be consumed by
            // another test binary in the same run. Skip instead of fail.
            skip_reason_ = result.error();
            return;
        }
        platform_ = new Platform(std::move(*result));
    }

    static void TearDownTestSuite() {
        delete platform_;
        platform_ = nullptr;
    }

    void SetUp() override {
        if (!platform_) {
            GTEST_SKIP() << "Platform unavailable: " << skip_reason_;
        }
    }

    static Platform* platform_;
    static std::string skip_reason_;
};

Platform* PlatformTest::platform_ = nullptr;
std::string PlatformTest::skip_reason_;

TEST_F(PlatformTest, HappyPath) {
    EXPECT_TRUE(platform_->is_running());
    EXPECT_EQ(platform_->port_id(), 0u);
    EXPECT_NE(platform_->mempool(), nullptr);
}

TEST_F(PlatformTest, CollectReturnsZeroedStats) {
    auto stats = platform_->collect_stats();
    EXPECT_EQ(stats.rx_packets, 0u);
    EXPECT_EQ(stats.tx_packets, 0u);
    EXPECT_EQ(stats.rx_errors, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Platform::Stats observability
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformStats, to_json_contains_all_fields) {
    Platform::Stats s{
        .rx_packets = 100, .tx_packets = 200,
        .rx_bytes = 3000, .tx_bytes = 4000,
        .rx_missed = 5, .rx_errors = 6, .tx_errors = 7,
    };
    auto j = s.to_json();
    EXPECT_NE(j.find("\"rx_packets\":100"), std::string::npos);
    EXPECT_NE(j.find("\"tx_packets\":200"), std::string::npos);
    EXPECT_NE(j.find("\"rx_bytes\":3000"), std::string::npos);
    EXPECT_NE(j.find("\"tx_bytes\":4000"), std::string::npos);
    EXPECT_NE(j.find("\"rx_missed\":5"), std::string::npos);
    EXPECT_NE(j.find("\"rx_errors\":6"), std::string::npos);
    EXPECT_NE(j.find("\"tx_errors\":7"), std::string::npos);
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
}

TEST(PlatformStats, dump_contains_all_fields) {
    Platform::Stats s{.rx_packets = 42, .tx_packets = 99};
    auto d = s.dump();
    EXPECT_NE(d.find("Platform::Stats"), std::string::npos);
    EXPECT_NE(d.find("rx_packets: 42"), std::string::npos);
    EXPECT_NE(d.find("tx_packets: 99"), std::string::npos);
}

TEST(PlatformStats, operator_minus_diffs_counters) {
    Platform::Stats s1{.rx_packets = 100, .tx_packets = 50, .rx_bytes = 3000};
    Platform::Stats s2{.rx_packets =  80, .tx_packets = 30, .rx_bytes = 1000};
    auto delta = s1 - s2;
    EXPECT_EQ(delta.rx_packets, 20u);
    EXPECT_EQ(delta.tx_packets, 20u);
    EXPECT_EQ(delta.rx_bytes, 2000u);
}

TEST(PlatformStats, equality_compares_all_fields) {
    Platform::Stats a{.rx_packets = 1, .tx_packets = 2};
    Platform::Stats b{.rx_packets = 1, .tx_packets = 2};
    EXPECT_EQ(a, b);
    b.tx_packets = 3;
    EXPECT_NE(a, b);
}

TEST(PlatformStats, formatter_produces_dump_output) {
    Platform::Stats s{.rx_packets = 7};
    auto formatted = std::format("{}", s);
    EXPECT_NE(formatted.find("rx_packets: 7"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpSession::Stats observability
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpSessionStats, to_json_contains_all_fields) {
    TcpSession<>::Stats s{
        .tx_packets = 10, .rx_packets = 20,
        .tx_bytes = 300, .rx_bytes = 400,
        .acks_sent = 5, .out_of_order = 1, .resets_received = 2,
    };
    auto j = s.to_json();
    EXPECT_NE(j.find("\"tx_packets\":10"), std::string::npos);
    EXPECT_NE(j.find("\"rx_packets\":20"), std::string::npos);
    EXPECT_NE(j.find("\"tx_bytes\":300"), std::string::npos);
    EXPECT_NE(j.find("\"rx_bytes\":400"), std::string::npos);
    EXPECT_NE(j.find("\"acks_sent\":5"), std::string::npos);
    EXPECT_NE(j.find("\"out_of_order\":1"), std::string::npos);
    EXPECT_NE(j.find("\"resets_received\":2"), std::string::npos);
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
}

TEST(TcpSessionStats, dump_contains_all_fields) {
    TcpSession<>::Stats s{.tx_packets = 77, .acks_sent = 33};
    auto d = s.dump();
    EXPECT_NE(d.find("TcpSession::Stats"), std::string::npos);
    EXPECT_NE(d.find("tx_packets: 77"), std::string::npos);
    EXPECT_NE(d.find("acks_sent: 33"), std::string::npos);
}

TEST(TcpSessionStats, operator_minus_diffs_counters) {
    TcpSession<>::Stats s1{.tx_packets = 50, .rx_packets = 100, .acks_sent = 20};
    TcpSession<>::Stats s2{.tx_packets = 30, .rx_packets =  70, .acks_sent = 10};
    auto delta = s1 - s2;
    EXPECT_EQ(delta.tx_packets, 20u);
    EXPECT_EQ(delta.rx_packets, 30u);
    EXPECT_EQ(delta.acks_sent, 10u);
}

TEST(TcpSessionStats, equality_compares_all_fields) {
    TcpSession<>::Stats a{.tx_packets = 1, .rx_packets = 2};
    TcpSession<>::Stats b{.tx_packets = 1, .rx_packets = 2};
    EXPECT_EQ(a, b);
    b.rx_packets = 3;
    EXPECT_NE(a, b);
}

TEST(TcpSessionStats, formatter_produces_dump_output) {
    TcpSession<>::Stats s{.rx_packets = 42};
    auto formatted = std::format("{}", s);
    EXPECT_NE(formatted.find("rx_packets: 42"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig observability
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, equality_compares_all_fields) {
    TcpConfig a{};
    a.tuple = {.src_ip = 0x0A000001, .dst_ip = 0x0A000002,
               .src_port = 12345, .dst_port = 443};
    a.mss = 1460;
    a.recv_window = 65535;
    a.port_id = 0;
    TcpConfig b = a;
    EXPECT_EQ(a, b);
    b.mss = 1400;
    EXPECT_NE(a, b);
}

TEST(TcpConfig, equality_compares_mac_addresses) {
    TcpConfig a{};
    a.src_mac = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
    TcpConfig b = a;
    EXPECT_EQ(a, b);
    b.src_mac = {{0xFF, 0x11, 0x22, 0x33, 0x44, 0x55}};
    EXPECT_NE(a, b);
}

TEST(TcpConfig, to_json_contains_all_fields) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0x0A000001, .dst_ip = 0x0A000002,
                 .src_port = 12345, .dst_port = 443};
    cfg.mss = 1460;
    cfg.recv_window = 65535;
    cfg.port_id = 1;
    cfg.tx_queue_id = 2;
    cfg.rx_queue_id = 3;
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"mss\":1460"), std::string::npos);
    EXPECT_NE(j.find("\"recv_window\":65535"), std::string::npos);
    EXPECT_NE(j.find("\"port_id\":1"), std::string::npos);
    EXPECT_NE(j.find("\"tx_queue_id\":2"), std::string::npos);
    EXPECT_NE(j.find("\"rx_queue_id\":3"), std::string::npos);
    EXPECT_NE(j.find("\"src_port\":12345"), std::string::npos);
    EXPECT_NE(j.find("\"dst_port\":443"), std::string::npos);
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
}

TEST(TcpConfig, dump_contains_all_fields) {
    TcpConfig cfg{};
    cfg.mss = 1460;
    cfg.recv_window = 32768;
    auto d = cfg.dump();
    EXPECT_NE(d.find("TcpConfig"), std::string::npos);
    EXPECT_NE(d.find("1460"), std::string::npos);
    EXPECT_NE(d.find("32768"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// PlatformConfig observability
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformConfig, equality_compares_all_fields) {
    PlatformConfig a{};
    PlatformConfig b{};
    EXPECT_EQ(a, b);
    b.nb_rx_queues = 4;
    EXPECT_NE(a, b);
}

TEST(PlatformConfig, to_json_contains_all_fields) {
    PlatformConfig cfg{.port_id = 1, .nb_rx_queues = 2, .nb_tx_queues = 3,
                       .nb_rx_desc = 512, .nb_tx_desc = 1024,
                       .mbuf_pool_size = 8191, .mbuf_cache_size = 512,
                       .enable_promiscuous = true, .link_timeout_ms = 5000};
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"port_id\":1"), std::string::npos);
    EXPECT_NE(j.find("\"nb_rx_queues\":2"), std::string::npos);
    EXPECT_NE(j.find("\"nb_tx_queues\":3"), std::string::npos);
    EXPECT_NE(j.find("\"nb_rx_desc\":512"), std::string::npos);
    EXPECT_NE(j.find("\"nb_tx_desc\":1024"), std::string::npos);
    EXPECT_NE(j.find("\"mbuf_pool_size\":8191"), std::string::npos);
    EXPECT_NE(j.find("\"mbuf_cache_size\":512"), std::string::npos);
    EXPECT_NE(j.find("\"enable_promiscuous\":true"), std::string::npos);
    EXPECT_NE(j.find("\"link_timeout_ms\":5000"), std::string::npos);
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
}

TEST(PlatformConfig, dump_contains_all_fields) {
    PlatformConfig cfg{.port_id = 0, .nb_rx_queues = 2, .nb_tx_queues = 2};
    auto d = cfg.dump();
    EXPECT_NE(d.find("PlatformConfig"), std::string::npos);
    EXPECT_NE(d.find("2rx/2tx"), std::string::npos);
}

TEST(PlatformConfig, to_json_promiscuous_false) {
    PlatformConfig cfg{};
    cfg.enable_promiscuous = false;
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"enable_promiscuous\":false"), std::string::npos);
}

TEST(TcpConfig, format_mac_all_zeros) {
    rte_ether_addr zero = {{0, 0, 0, 0, 0, 0}};
    EXPECT_EQ(TcpConfig::format_mac(zero), "00:00:00:00:00:00");
}

TEST(TcpConfig, format_mac_broadcast) {
    rte_ether_addr bcast = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    EXPECT_EQ(TcpConfig::format_mac(bcast), "ff:ff:ff:ff:ff:ff");
}

TEST(TcpConfig, format_mac_mixed_bytes) {
    rte_ether_addr mac = {{0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E}};
    EXPECT_EQ(TcpConfig::format_mac(mac), "00:1a:2b:3c:4d:5e");
}

TEST(TcpConfig, equality_default_initialized) {
    TcpConfig a{};
    TcpConfig b{};
    EXPECT_EQ(a, b);
}

TEST(TcpConfig, dump_includes_ip_and_mac) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0x0A000001, .dst_ip = 0xC0A80001,
                 .src_port = 5000, .dst_port = 443};
    cfg.src_mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    auto d = cfg.dump();
    EXPECT_NE(d.find("10.0.0.1"), std::string::npos);
    EXPECT_NE(d.find("192.168.0.1"), std::string::npos);
    EXPECT_NE(d.find("aa:bb:cc:dd:ee:ff"), std::string::npos);
    EXPECT_NE(d.find("5000"), std::string::npos);
    EXPECT_NE(d.find("443"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::validate()
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, validate_valid_config_returns_empty) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0x0A000001, .dst_ip = 0x0A000002,
                 .src_port = 12345, .dst_port = 443};
    cfg.mss = 1460;
    cfg.recv_window = 65535;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TcpConfig, validate_zero_src_ip) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0, .dst_ip = 0x0A000002,
                 .src_port = 12345, .dst_port = 443};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("src_ip"), std::string_view::npos);
}

TEST(TcpConfig, validate_zero_dst_ip) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0x0A000001, .dst_ip = 0,
                 .src_port = 12345, .dst_port = 443};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("dst_ip"), std::string_view::npos);
}

TEST(TcpConfig, validate_zero_src_port) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0x0A000001, .dst_ip = 0x0A000002,
                 .src_port = 0, .dst_port = 443};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("src_port"), std::string_view::npos);
}

TEST(TcpConfig, validate_zero_dst_port) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0x0A000001, .dst_ip = 0x0A000002,
                 .src_port = 12345, .dst_port = 0};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("dst_port"), std::string_view::npos);
}

TEST(TcpConfig, validate_zero_mss) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0x0A000001, .dst_ip = 0x0A000002,
                 .src_port = 12345, .dst_port = 443};
    cfg.mss = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("mss"), std::string_view::npos);
}

TEST(TcpConfig, validate_excessive_mss) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0x0A000001, .dst_ip = 0x0A000002,
                 .src_port = 12345, .dst_port = 443};
    cfg.mss = 9001;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("mss"), std::string_view::npos);
}

TEST(TcpConfig, validate_zero_recv_window) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0x0A000001, .dst_ip = 0x0A000002,
                 .src_port = 12345, .dst_port = 443};
    cfg.recv_window = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("recv_window"), std::string_view::npos);
}

TEST(TcpConfig, validate_default_config_fails) {
    // Default-constructed config has zero IPs and ports — should fail
    TcpConfig cfg{};
    EXPECT_FALSE(cfg.validate().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig std::formatter
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, formatter_contains_key_fields) {
    TcpConfig cfg{};
    cfg.tuple = {.src_ip = 0x0A000001, .dst_ip = 0xC0A80001,
                 .src_port = 5000, .dst_port = 443};
    cfg.mss = 1460;
    cfg.recv_window = 32768;
    auto formatted = std::format("{}", cfg);
    EXPECT_NE(formatted.find("TcpConfig"), std::string::npos);
    EXPECT_NE(formatted.find("10.0.0.1"), std::string::npos);
    EXPECT_NE(formatted.find("192.168.0.1"), std::string::npos);
    EXPECT_NE(formatted.find("1460"), std::string::npos);
    EXPECT_NE(formatted.find("32768"), std::string::npos);
}

TEST(TcpConfig, formatter_default_config) {
    TcpConfig cfg{};
    auto formatted = std::format("{}", cfg);
    EXPECT_NE(formatted.find("TcpConfig"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// PlatformConfig std::formatter
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformConfig, FormatterContainsKeyFields) {
    PlatformConfig cfg{
        .port_id = 2,
        .nb_rx_queues = 4,
        .nb_tx_queues = 2,
        .nb_rx_desc = 512,
        .nb_tx_desc = 1024,
        .mbuf_pool_size = 8191,
        .enable_promiscuous = true,
    };
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("PlatformConfig"), std::string::npos);
    EXPECT_NE(s.find("port=2"), std::string::npos);
    EXPECT_NE(s.find("4rx"), std::string::npos);
    EXPECT_NE(s.find("2tx"), std::string::npos);
    EXPECT_NE(s.find("pool=8191"), std::string::npos);
    EXPECT_NE(s.find("promisc=true"), std::string::npos);
}

TEST(PlatformConfig, FormatterDefaultConfig) {
    PlatformConfig cfg{};
    cfg.mbuf_pool_size = 4095; // valid 2^n-1
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("PlatformConfig"), std::string::npos);
    EXPECT_NE(s.find("pool=4095"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// PlatformConfig::warnings
// ─────────────────────────────────────────────────────────────────────────────

TEST(PlatformConfigWarnings, SensibleDefaultsNoWarnings) {
    PlatformConfig cfg{
        .nb_rx_desc = 256,
        .nb_tx_desc = 512,
        .mbuf_pool_size = 4095,
        .mbuf_cache_size = 256,
        .enable_promiscuous = false,
        .link_timeout_ms = 2000,
    };
    auto w = cfg.warnings();
    EXPECT_TRUE(w.empty()) << "Unexpected warning: " << (w.empty() ? "" : w[0]);
}

TEST(PlatformConfigWarnings, SmallRxDesc) {
    PlatformConfig cfg{.nb_rx_desc = 32, .nb_tx_desc = 512,
                       .mbuf_pool_size = 4095, .mbuf_cache_size = 256,
                       .link_timeout_ms = 2000};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("nb_rx_desc") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Expected small rx desc warning";
}

TEST(PlatformConfigWarnings, SmallTxDesc) {
    PlatformConfig cfg{.nb_rx_desc = 256, .nb_tx_desc = 32,
                       .mbuf_pool_size = 4095, .mbuf_cache_size = 256,
                       .link_timeout_ms = 2000};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("nb_tx_desc") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Expected small tx desc warning";
}

TEST(PlatformConfigWarnings, LargeCacheRelativeToPool) {
    PlatformConfig cfg{.nb_rx_desc = 256, .nb_tx_desc = 512,
                       .mbuf_pool_size = 1023, .mbuf_cache_size = 512,
                       .link_timeout_ms = 2000};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("mbuf_cache_size") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Expected large cache warning";
}

TEST(PlatformConfigWarnings, PromiscuousMode) {
    PlatformConfig cfg{.nb_rx_desc = 256, .nb_tx_desc = 512,
                       .mbuf_pool_size = 4095, .mbuf_cache_size = 256,
                       .enable_promiscuous = true, .link_timeout_ms = 2000};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("promiscuous") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Expected promiscuous warning";
}

TEST(PlatformConfigWarnings, ZeroLinkTimeout) {
    PlatformConfig cfg{.nb_rx_desc = 256, .nb_tx_desc = 512,
                       .mbuf_pool_size = 4095, .mbuf_cache_size = 256,
                       .link_timeout_ms = 0};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("link_timeout_ms") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Expected zero link timeout warning";
}
