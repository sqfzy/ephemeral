#include "eph_dpdk/tx_engine.hpp"
#include "mock_hal.hpp"

#include <thread>
#include <chrono>

#include <gtest/gtest.h>

using namespace eph_dpdk;
using eph_dpdk::test::MockDpdkHal;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static TxEngineConfig default_cfg() {
    TxEngineConfig cfg;
    cfg.port_id                  = 0;
    cfg.queue_id                 = 0;
    cfg.ring_size                = 16;   // small ring for easy "ring full" tests
    cfg.burst_size               = 8;
    cfg.drop_log_interval        = 1000; // high, so tests don't trigger it unless asked
    cfg.pool_check_burst_interval = 1;   // check every burst for pool tests
    return cfg;
}

static std::pair<std::expected<TxEngine, std::string>, std::shared_ptr<MockDpdkHal>>
make_engine(const TxEngineConfig& cfg = default_cfg()) {
    auto mock = std::make_shared<MockDpdkHal>();
    auto result = TxEngine::create(mock->pool_ptr, cfg, mock);
    return {std::move(result), std::move(mock)};
}

static const char PAYLOAD[] = "hello, DPDK";
static constexpr size_t PAYLOAD_LEN = sizeof(PAYLOAD) - 1;

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

TEST(TxEngineCreate, HappyPath) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(mock->ring_create_calls, 1);
}

TEST(TxEngineCreate, NullPoolReturnsError) {
    auto mock = std::make_shared<MockDpdkHal>();
    auto result = TxEngine::create(nullptr, default_cfg(), mock);
    EXPECT_FALSE(result.has_value());
}

TEST(TxEngineCreate, RingSizeNotPowerOfTwoReturnsError) {
    auto cfg = default_cfg();
    cfg.ring_size = 100;  // not power of 2
    auto [result, _] = make_engine(cfg);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("power of 2"), std::string::npos);
}

TEST(TxEngineCreate, ZeroBurstSizeReturnsError) {
    auto cfg = default_cfg();
    cfg.burst_size = 0;
    auto [result, _] = make_engine(cfg);
    EXPECT_FALSE(result.has_value());
}

TEST(TxEngineCreate, DestructorFreesRing) {
    auto mock = std::make_shared<MockDpdkHal>();
    {
        auto result = TxEngine::create(mock->pool_ptr, default_cfg(), mock);
        ASSERT_TRUE(result.has_value());
    }  // TxEngine destroyed here
    EXPECT_EQ(mock->ring_free_calls, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// enqueue — application thread path
// ─────────────────────────────────────────────────────────────────────────────

TEST(TxEngineEnqueue, SinglePacketSucceeds) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->enqueue(PAYLOAD, PAYLOAD_LEN), 0);
    EXPECT_EQ(mock->mbuf_alloc_calls, 1);
    // mbuf should now be in the ring (not freed yet)
    EXPECT_EQ(mock->mbuf_free_calls_layer2, 0);
}

TEST(TxEngineEnqueue, MultiplePacketsSucceed) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value());

    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(result->enqueue(PAYLOAD, PAYLOAD_LEN), 0);

    EXPECT_EQ(mock->mbuf_alloc_calls, 5);
    EXPECT_EQ(result->stats().tx_dropped, 0u);
}

TEST(TxEngineEnqueue, RingFullReturnsEagain) {
    auto cfg = default_cfg();
    cfg.ring_size = 4;  // ring holds max 4-1=3 items (rte_ring semantics)
    auto [result, mock] = make_engine(cfg);
    ASSERT_TRUE(result.has_value());

    // Fill ring until we hit -EAGAIN
    int eagain_count = 0;
    for (int i = 0; i < 8; ++i) {
        if (result->enqueue(PAYLOAD, PAYLOAD_LEN) == -EAGAIN)
            ++eagain_count;
    }
    EXPECT_GT(eagain_count, 0);
    EXPECT_GT(result->stats().tx_dropped, 0u);
}

TEST(TxEngineEnqueue, MbufAllocFailureReturnsEnomem) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value());

    mock->mbuf_alloc_fail_after = 0;  // fail immediately
    EXPECT_EQ(result->enqueue(PAYLOAD, PAYLOAD_LEN), -ENOMEM);
    EXPECT_EQ(result->stats().mbuf_alloc_failures, 1u);
    EXPECT_EQ(result->stats().tx_dropped, 1u);
}

TEST(TxEngineEnqueue, MbufFillFailureReturnsEmsgsize) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value());

    mock->mbuf_fill_ret = false;  // simulate oversized payload
    EXPECT_EQ(result->enqueue(PAYLOAD, PAYLOAD_LEN), -EMSGSIZE);
    // The allocated mbuf must have been freed
    EXPECT_EQ(mock->mbuf_free_calls_layer2, 1);
    EXPECT_EQ(result->stats().tx_dropped, 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// process_one_burst — TX lcore path
// ─────────────────────────────────────────────────────────────────────────────

TEST(TxEngineBurst, EmptyRingReturnsZero) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->process_one_burst(), 0u);
    EXPECT_EQ(mock->eth_tx_burst_calls, 0);
}

TEST(TxEngineBurst, AllPacketsSent) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value());

    // Enqueue 5 packets
    for (int i = 0; i < 5; ++i)
        ASSERT_EQ(result->enqueue(PAYLOAD, PAYLOAD_LEN), 0);

    // tx_burst sends all 5
    uint32_t sent = result->process_one_burst();
    EXPECT_EQ(sent, 5u);
    EXPECT_EQ(result->stats().tx_packets, 5u);
    EXPECT_EQ(result->stats().tx_dropped, 0u);
    // No unsent mbufs → nothing freed by burst
    EXPECT_EQ(mock->mbuf_free_calls_layer2, 0);
}

TEST(TxEngineBurst, PartialSendFreesUnsentMbufs) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value());

    // Enqueue 6 packets
    for (int i = 0; i < 6; ++i)
        ASSERT_EQ(result->enqueue(PAYLOAD, PAYLOAD_LEN), 0);

    // tx_burst sends only 4 out of 6
    mock->tx_burst_send_count = 4;
    uint32_t sent = result->process_one_burst();

    EXPECT_EQ(sent, 4u);
    EXPECT_EQ(result->stats().tx_packets, 4u);
    // 2 unsent mbufs must be freed
    EXPECT_EQ(mock->mbuf_free_calls_layer2, 2);
    EXPECT_EQ(result->stats().tx_dropped, 2u);
}

TEST(TxEngineBurst, AllDroppedWhenTxBurstSendsZero) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value());

    for (int i = 0; i < 4; ++i)
        ASSERT_EQ(result->enqueue(PAYLOAD, PAYLOAD_LEN), 0);

    mock->tx_burst_send_count = 0;
    result->process_one_burst();

    EXPECT_EQ(result->stats().tx_packets, 0u);
    EXPECT_EQ(result->stats().tx_dropped, 4u);
    EXPECT_EQ(mock->mbuf_free_calls_layer2, 4);
}

TEST(TxEngineBurst, BurstRespectsBurstSizeLimit) {
    auto cfg = default_cfg();
    cfg.burst_size = 3;
    auto [result, mock] = make_engine(cfg);
    ASSERT_TRUE(result.has_value());

    // Enqueue 8 packets
    for (int i = 0; i < 8; ++i)
        ASSERT_EQ(result->enqueue(PAYLOAD, PAYLOAD_LEN), 0);

    // First burst sends at most burst_size=3
    result->process_one_burst();
    EXPECT_EQ(mock->eth_tx_burst_calls, 1);
    // At most 3 items were passed to tx_burst
    EXPECT_LE(mock->eth_tx_burst_total_sent, 3u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Drop accounting
// ─────────────────────────────────────────────────────────────────────────────

TEST(TxEngineDrops, DropLogWarnEmittedAfterInterval) {
    auto cfg             = default_cfg();
    cfg.drop_log_interval = 3;  // emit WARN after every 3 drops
    auto [result, mock]  = make_engine(cfg);
    ASSERT_TRUE(result.has_value());

    // Enqueue 4 packets, tx_burst sends none → 4 drops
    for (int i = 0; i < 4; ++i)
        ASSERT_EQ(result->enqueue(PAYLOAD, PAYLOAD_LEN), 0);
    mock->tx_burst_send_count = 0;
    result->process_one_burst();

    // 4 drops ≥ interval 3 → at least one WARN pushed to log ring
    auto entries = result->drain_log_ring();
    bool has_warn = std::any_of(entries.begin(), entries.end(),
        [](const LogEntry& e) { return e.level == spdlog::level::warn; });
    EXPECT_TRUE(has_warn);
}

TEST(TxEngineDrops, DropCountAccumulatesAcrossBursts) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value());

    mock->tx_burst_send_count = 0;  // drop everything
    for (int burst = 0; burst < 3; ++burst) {
        for (int i = 0; i < 2; ++i)
            result->enqueue(PAYLOAD, PAYLOAD_LEN);
        result->process_one_burst();
    }
    EXPECT_EQ(result->stats().tx_dropped, 6u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pool utilization warning
// ─────────────────────────────────────────────────────────────────────────────

TEST(TxEnginePool, WarnWhenUtilizationExceedsThreshold) {
    auto cfg = default_cfg();
    cfg.pool_check_burst_interval = 1;  // check every burst
    cfg.pool_high_watermark       = 0.80f;
    auto [result, mock] = make_engine(cfg);
    ASSERT_TRUE(result.has_value());

    // 900 in-use out of 1000 total = 90% > 80% threshold
    mock->pool_in_use = 900;
    mock->pool_avail  = 100;

    // Enqueue and burst to trigger pool check
    result->enqueue(PAYLOAD, PAYLOAD_LEN);
    result->process_one_burst();

    auto entries = result->drain_log_ring();
    bool has_pool_warn = std::any_of(entries.begin(), entries.end(),
        [](const LogEntry& e) {
            return e.level == spdlog::level::warn
                && std::string_view{e.msg}.find("pool") != std::string_view::npos;
        });
    EXPECT_TRUE(has_pool_warn);
}

TEST(TxEnginePool, NoWarnWhenUtilizationBelowThreshold) {
    auto cfg = default_cfg();
    cfg.pool_check_burst_interval = 1;
    cfg.pool_high_watermark       = 0.80f;
    auto [result, mock] = make_engine(cfg);
    ASSERT_TRUE(result.has_value());

    // 500 in-use out of 1000 total = 50% < 80%
    mock->pool_in_use = 500;
    mock->pool_avail  = 500;

    result->enqueue(PAYLOAD, PAYLOAD_LEN);
    result->process_one_burst();

    auto entries = result->drain_log_ring();
    bool has_pool_warn = std::any_of(entries.begin(), entries.end(),
        [](const LogEntry& e) {
            return e.level == spdlog::level::warn
                && std::string_view{e.msg}.find("pool") != std::string_view::npos;
        });
    EXPECT_FALSE(has_pool_warn);
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle: run_loop + stop
// ─────────────────────────────────────────────────────────────────────────────

TEST(TxEngineLifecycle, RunLoopExitsOnStop) {
    auto [result, _] = make_engine();
    ASSERT_TRUE(result.has_value());

    std::thread t([&result]() { result->run_loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    result->stop();
    t.join();  // must complete promptly
    SUCCEED();
}

TEST(TxEngineLifecycle, MovePreservesState) {
    auto [result, mock] = make_engine();
    ASSERT_TRUE(result.has_value());

    result->enqueue(PAYLOAD, PAYLOAD_LEN);
    TxEngine moved = std::move(*result);
    moved.process_one_burst();

    EXPECT_EQ(moved.stats().tx_packets, 1u);
}
