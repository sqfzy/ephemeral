#include "eph_dpdk/platform.hpp"
#include "mock_hal.hpp"

#include <memory>

#include <gtest/gtest.h>

using namespace eph_dpdk;
using eph_dpdk::test::MockDpdkHal;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a valid PlatformConfig and a ready-to-use MockDpdkHal owner.
// ─────────────────────────────────────────────────────────────────────────────

static PlatformConfig default_config() {
    PlatformConfig cfg;
    cfg.port_id         = 0;
    cfg.nb_rx_queues    = 1;
    cfg.nb_tx_queues    = 1;
    cfg.nb_rx_desc      = 256;
    cfg.nb_tx_desc      = 512;
    cfg.mbuf_pool_size  = 4095;   // 2^12 − 1 ✓
    cfg.mbuf_cache_size = 128;
    cfg.link_timeout_ms = 0;      // single link check, no sleeping in tests
    return cfg;
}

/// Convenience: run create() with a MockDpdkHal.
/// Returns the platform result AND a shared_ptr to the mock so tests can
/// inspect call counts even after the platform is destroyed.
static std::pair<std::expected<DpdkPlatform, std::string>,
                 std::shared_ptr<MockDpdkHal>>
make_platform(const PlatformConfig& cfg,
              std::shared_ptr<MockDpdkHal> mock) {
    auto result = DpdkPlatform::create(0, nullptr, cfg, mock);
    return {std::move(result), std::move(mock)};
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_config tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValidateConfig, ValidConfigReturnsEmpty) {
    EXPECT_TRUE(validate_config(default_config()).empty());
}

TEST(ValidateConfig, ZeroRxQueuesIsError) {
    auto cfg = default_config();
    cfg.nb_rx_queues = 0;
    EXPECT_FALSE(validate_config(cfg).empty());
}

TEST(ValidateConfig, ZeroTxQueuesIsError) {
    auto cfg = default_config();
    cfg.nb_tx_queues = 0;
    EXPECT_FALSE(validate_config(cfg).empty());
}

TEST(ValidateConfig, ZeroRxDescIsError) {
    auto cfg = default_config();
    cfg.nb_rx_desc = 0;
    EXPECT_FALSE(validate_config(cfg).empty());
}

TEST(ValidateConfig, ZeroTxDescIsError) {
    auto cfg = default_config();
    cfg.nb_tx_desc = 0;
    EXPECT_FALSE(validate_config(cfg).empty());
}

TEST(ValidateConfig, NegativeLinkTimeoutIsError) {
    auto cfg = default_config();
    cfg.link_timeout_ms = -1;
    EXPECT_FALSE(validate_config(cfg).empty());
}

TEST(ValidateConfig, PoolSizeNotPowerOfTwoMinusOneReturnsWarning) {
    auto cfg = default_config();
    cfg.mbuf_pool_size = 1000;  // not 2^n − 1
    std::string msg = validate_config(cfg);
    // Should be non-empty (warning) and mention "2^n"
    EXPECT_FALSE(msg.empty());
    EXPECT_NE(msg.find("2^n"), std::string::npos);
}

TEST(ValidateConfig, ValidPoolSizes) {
    for (uint32_t v : {uint32_t{1023}, uint32_t{4095}, uint32_t{8191}}) {
        auto cfg       = default_config();
        cfg.mbuf_pool_size = v;
        EXPECT_TRUE(validate_config(cfg).empty())
            << "Expected valid for pool_size=" << v;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DpdkPlatform::create happy path
// ─────────────────────────────────────────────────────────────────────────────

TEST(DpdkPlatformCreate, HappyPath) {
    auto [result, mock] = make_platform(default_config(),
                                        std::make_shared<MockDpdkHal>());
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_TRUE(result->is_running());
    EXPECT_EQ(result->port_id(), 0u);
    EXPECT_NE(result->mempool(), nullptr);

    // Verify initialization sequence was followed
    EXPECT_EQ(mock->eal_init_calls, 1);
    EXPECT_EQ(mock->eth_dev_configure_calls, 1);
    EXPECT_EQ(mock->eth_dev_start_calls, 1);
}

TEST(DpdkPlatformCreate, EalInitFailureReturnsError) {
    auto mock           = std::make_shared<MockDpdkHal>();
    mock->eal_init_ret  = -1;
    auto [result, _]    = make_platform(default_config(), std::move(mock));
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("rte_eal_init"), std::string::npos);
}

TEST(DpdkPlatformCreate, NoPortsAvailableReturnsError) {
    auto mock         = std::make_shared<MockDpdkHal>();
    mock->dev_count   = 0;
    auto [result, _]  = make_platform(default_config(), std::move(mock));
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("available"), std::string::npos);
}

TEST(DpdkPlatformCreate, PortIdOutOfRangeReturnsError) {
    auto cfg        = default_config();
    cfg.port_id     = 5;
    auto mock       = std::make_shared<MockDpdkHal>();
    mock->dev_count = 2;  // only ports 0 and 1
    auto [result, _] = make_platform(cfg, std::move(mock));
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("out of range"), std::string::npos);
}

TEST(DpdkPlatformCreate, MempoolCreationFailureReturnsError) {
    auto mock        = std::make_shared<MockDpdkHal>();
    mock->pool_ptr   = nullptr;  // simulate allocation failure
    auto [result, _] = make_platform(default_config(), std::move(mock));
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("mbuf pool"), std::string::npos);
}

TEST(DpdkPlatformCreate, PortConfigureFailureReturnsError) {
    auto mock                     = std::make_shared<MockDpdkHal>();
    mock->eth_dev_configure_ret   = -22;  // -EINVAL
    auto [result, _]              = make_platform(default_config(), std::move(mock));
    EXPECT_FALSE(result.has_value());
}

TEST(DpdkPlatformCreate, RxQueueSetupFailureReturnsError) {
    auto mock                    = std::make_shared<MockDpdkHal>();
    mock->eth_rx_queue_setup_ret = -22;
    auto [result, _]             = make_platform(default_config(), std::move(mock));
    EXPECT_FALSE(result.has_value());
}

TEST(DpdkPlatformCreate, TxQueueSetupFailureReturnsError) {
    auto mock                    = std::make_shared<MockDpdkHal>();
    mock->eth_tx_queue_setup_ret = -22;
    auto [result, _]             = make_platform(default_config(), std::move(mock));
    EXPECT_FALSE(result.has_value());
}

TEST(DpdkPlatformCreate, PortStartFailureReturnsError) {
    auto mock               = std::make_shared<MockDpdkHal>();
    mock->eth_dev_start_ret = -1;
    auto [result, _]        = make_platform(default_config(), std::move(mock));
    EXPECT_FALSE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// Link polling
// ─────────────────────────────────────────────────────────────────────────────

TEST(DpdkPlatformCreate, LinkDownWithZeroTimeoutStillSucceeds) {
    // link_timeout_ms = 0 means check once and continue regardless
    auto cfg            = default_config();
    cfg.link_timeout_ms = 0;
    auto mock           = std::make_shared<MockDpdkHal>();
    mock->link_up       = false;
    auto [result, _]    = make_platform(cfg, std::move(mock));
    // Non-fatal: platform still created, but link is down
    EXPECT_TRUE(result.has_value()) << result.error();
}

TEST(DpdkPlatformCreate, LinkDownWithTimeoutStillSucceeds) {
    // Even with a timeout > 0, if link doesn't come up we log WARN and continue
    auto cfg            = default_config();
    cfg.link_timeout_ms = 50;  // small enough for fast tests
    auto mock           = std::make_shared<MockDpdkHal>();
    mock->link_up       = false;
    // Advance time fast per sleep so we exit the loop immediately
    mock->sleep_advance_ms = 100;
    auto [result, _]    = make_platform(cfg, std::move(mock));
    EXPECT_TRUE(result.has_value()) << result.error();
}

// ─────────────────────────────────────────────────────────────────────────────
// Statistics
// ─────────────────────────────────────────────────────────────────────────────

TEST(DpdkPlatformStats, CollectReturnsZeroedStatsOnSuccess) {
    auto [result, _] = make_platform(default_config(),
                                     std::make_shared<MockDpdkHal>());
    ASSERT_TRUE(result.has_value());
    auto stats = result->collect_stats();
    EXPECT_EQ(stats.rx_packets, 0u);
    EXPECT_EQ(stats.tx_packets, 0u);
    EXPECT_EQ(stats.rx_errors, 0u);
}

TEST(DpdkPlatformStats, CollectStatsOnGetFailureReturnsZeroed) {
    auto mock                = std::make_shared<MockDpdkHal>();
    mock->eth_stats_get_ret  = -1;
    auto [result, _]         = make_platform(default_config(), std::move(mock));
    ASSERT_TRUE(result.has_value());
    auto stats = result->collect_stats();
    // On failure, collect_stats() returns a default-constructed Stats
    EXPECT_EQ(stats.rx_packets, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor cleanup
// ─────────────────────────────────────────────────────────────────────────────

TEST(DpdkPlatformLifecycle, DestructorStopsAndClosesPort) {
    // Keep a strong reference outside the inner block so the mock outlives
    // the platform and we can safely read call counts after destruction.
    auto mock = std::make_shared<MockDpdkHal>();

    {
        auto result = DpdkPlatform::create(0, nullptr, default_config(), mock);
        ASSERT_TRUE(result.has_value()) << result.error();
        // result destroyed here → DpdkPlatform destructor runs
    }

    EXPECT_EQ(mock->eth_dev_stop_calls, 1);
    EXPECT_EQ(mock->eth_dev_close_calls, 1);
    EXPECT_EQ(mock->mempool_free_calls, 1);
    EXPECT_EQ(mock->eal_cleanup_calls, 1);
}

TEST(DpdkPlatformLifecycle, MovePreservesState) {
    auto [result, _] = make_platform(default_config(),
                                     std::make_shared<MockDpdkHal>());
    ASSERT_TRUE(result.has_value());
    DpdkPlatform moved = std::move(*result);
    EXPECT_TRUE(moved.is_running());
    EXPECT_EQ(moved.port_id(), 0u);
}
