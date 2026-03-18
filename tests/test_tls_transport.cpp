#include <gtest/gtest.h>

#include "dpdk_test_env.hpp"
#include "eph/dpdk/platform.hpp"

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
    // With net_null only port 0 exists.  The error may say "out of range"
    // (if count > 0 but port_id >= count) or "No DPDK ports" (if no ports
    // at all).  Both are valid rejections for port_id=99.
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
        ASSERT_TRUE(result.has_value()) << result.error();
        platform_ = new Platform(std::move(*result));
    }

    static void TearDownTestSuite() {
        delete platform_;
        platform_ = nullptr;
    }

    static Platform* platform_;
};

Platform* PlatformTest::platform_ = nullptr;

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
