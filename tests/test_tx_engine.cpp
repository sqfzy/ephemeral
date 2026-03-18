#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/tx_engine.hpp"

using namespace eph::dpdk;

// ─────────────────────────────────────────────────────────────────────────────
// Type alias for the TxEngine instantiation used in tests.
// Small burst/ring sizes for fast, predictable tests.
// ─────────────────────────────────────────────────────────────────────────────

using TestEngine      = TxEngine<16, 64>;
using SmallRingEngine = TxEngine<8, 4>;

// ─────────────────────────────────────────────────────────────────────────────
// Shared Platform
// ─────────────────────────────────────────────────────────────────────────────

class TxEngineTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        static constexpr PlatformConfig pcfg{
            .port_id         = 0,
            .nb_rx_queues    = 1,
            .nb_tx_queues    = 1,
            .nb_rx_desc      = 64,
            .nb_tx_desc      = 64,
            .mbuf_pool_size  = 1023,
            .mbuf_cache_size = 32,
            .link_timeout_ms = 0,
        };
        static_assert(config_ok(pcfg));

        auto result = Platform::create(pcfg);
        ASSERT_TRUE(result.has_value()) << result.error();
        platform_ = new Platform(std::move(*result));
    }

    static void TearDownTestSuite() {
        delete platform_;
        platform_ = nullptr;
    }

    static Platform* platform_;

    static constexpr const char PAYLOAD[] = "hello, DPDK";
    static constexpr size_t PAYLOAD_LEN = sizeof(PAYLOAD) - 1;
};

Platform* TxEngineTest::platform_ = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TxEngineTest, CreateSucceeds) {
    auto result = TestEngine::create(platform_->mempool());
    ASSERT_TRUE(result.has_value()) << result.error();
}

TEST_F(TxEngineTest, NullPoolReturnsError) {
    auto result = TestEngine::create(nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST_F(TxEngineTest, CompileTimeConstants) {
    static_assert(TestEngine::burst_size() == 16);
    static_assert(TestEngine::ring_size()  == 64);
    static_assert(SmallRingEngine::burst_size() == 8);
    static_assert(SmallRingEngine::ring_size()  == 4);
    SUCCEED();
}

// ─────────────────────────────────────────────────────────────────────────────
// Enqueue + burst happy path
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TxEngineTest, EnqueueAndBurstSendsPackets) {
    auto result = TestEngine::create(platform_->mempool());
    ASSERT_TRUE(result.has_value());

    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(result->enqueue(PAYLOAD, PAYLOAD_LEN), 0);

    uint32_t sent = result->process_one_burst();
    EXPECT_EQ(sent, 5u);
    EXPECT_EQ(result->stats().tx_packets, 5u);
    EXPECT_EQ(result->stats().tx_dropped, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Ring full → EAGAIN
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TxEngineTest, RingFullReturnsEagain) {
    auto result = SmallRingEngine::create(platform_->mempool());
    ASSERT_TRUE(result.has_value());

    int eagain_count = 0;
    for (int i = 0; i < 8; ++i) {
        if (result->enqueue(PAYLOAD, PAYLOAD_LEN) == -EAGAIN)
            ++eagain_count;
    }
    EXPECT_GT(eagain_count, 0);
    EXPECT_GT(result->stats().tx_dropped, 0u);

    // Drain to reclaim mbufs
    while (result->process_one_burst() > 0) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// Pool exhaustion → ENOMEM
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TxEngineTest, PoolExhaustionReturnsEnomem) {
    using BigRingEngine = TxEngine<32, 2048>;
    auto result = BigRingEngine::create(platform_->mempool());
    ASSERT_TRUE(result.has_value());

    int enomem_count = 0;
    int success_count = 0;
    for (int i = 0; i < 1200; ++i) {
        int ret = result->enqueue(PAYLOAD, PAYLOAD_LEN);
        if (ret == -ENOMEM) { ++enomem_count; break; }
        if (ret == 0) ++success_count;
    }

    EXPECT_GT(enomem_count, 0)
        << "Expected pool exhaustion after " << success_count << " enqueues";

    while (result->process_one_burst() > 0) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// Pool utilization warning
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TxEngineTest, PoolUtilizationWarnTriggered) {
    using BigRingEngine = TxEngine<32, 2048>;
    TxRuntimeConfig cfg;
    cfg.pool_check_burst_interval = 1;
    cfg.pool_high_watermark       = 0.50f;

    auto result = BigRingEngine::create(platform_->mempool(), cfg);
    ASSERT_TRUE(result.has_value());

    // Fill ~60% of pool
    for (int i = 0; i < 620; ++i) {
        if (result->enqueue(PAYLOAD, PAYLOAD_LEN) != 0) break;
    }

    result->process_one_burst();

    auto entries = result->drain_log_ring();
    bool has_pool_warn = std::any_of(entries.begin(), entries.end(),
        [](const LogEntry& e) {
            return e.level == spdlog::level::warn
                && std::string_view{e.msg}.find("pool") != std::string_view::npos;
        });
    EXPECT_TRUE(has_pool_warn);

    while (result->process_one_burst() > 0) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(TxEngineTest, RunLoopExitsOnStop) {
    auto result = TestEngine::create(platform_->mempool());
    ASSERT_TRUE(result.has_value());

    std::thread t([&result]() { result->run_loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    result->stop();
    t.join();
    SUCCEED();
}

TEST_F(TxEngineTest, MovePreservesState) {
    auto result = TestEngine::create(platform_->mempool());
    ASSERT_TRUE(result.has_value());

    result->enqueue(PAYLOAD, PAYLOAD_LEN);
    TestEngine moved = std::move(*result);
    moved.process_one_burst();
    EXPECT_EQ(moved.stats().tx_packets, 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Limitations with net_null (documented for completeness)
//
//   - Partial tx_burst send (net_null always sends all packets)
//   - mbuf_fill failure (net_null uses standard data room sizes)
//   - eth_dev_configure / queue_setup / dev_start failures
//   - EAL init failure
//
// The decision logic for these is validated in test_dpdk_logic.cpp.
// ─────────────────────────────────────────────────────────────────────────────
