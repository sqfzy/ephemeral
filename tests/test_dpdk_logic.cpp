#include <gtest/gtest.h>

#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/tx_engine.hpp"

using namespace eph::dpdk;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time config validation via static_assert
//
// These fire at compile time — if any fails, the build breaks with a
// readable message.  No runtime test needed; their existence IS the test.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr PlatformConfig good_cfg{
    .port_id         = 0,
    .nb_rx_queues    = 1,
    .nb_tx_queues    = 1,
    .nb_rx_desc      = 256,
    .nb_tx_desc      = 512,
    .mbuf_pool_size  = 4095,
    .mbuf_cache_size = 128,
    .link_timeout_ms = 0,
};
static_assert(config_ok(good_cfg), "Default config should be valid");

// Pool size 2^n-1 family
static_assert(config_ok(PlatformConfig{.mbuf_pool_size = 1023}));
static_assert(config_ok(PlatformConfig{.mbuf_pool_size = 8191}));

// Bad configs rejected at compile time
static_assert(!config_ok(PlatformConfig{.nb_rx_queues = 0}));
static_assert(!config_ok(PlatformConfig{.nb_tx_queues = 0}));
static_assert(!config_ok(PlatformConfig{.nb_rx_desc = 0}));
static_assert(!config_ok(PlatformConfig{.nb_tx_desc = 0}));
static_assert(!config_ok(PlatformConfig{.link_timeout_ms = -1}));
static_assert(!config_ok(PlatformConfig{.mbuf_pool_size = 1000}));
static_assert(!config_ok(PlatformConfig{.mbuf_pool_size = 100}));

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time utility tests via static_assert
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// is_power_of_two_minus_one
static_assert( detail::is_power_of_two_minus_one(1));     // 2^1-1
static_assert( detail::is_power_of_two_minus_one(3));     // 2^2-1
static_assert( detail::is_power_of_two_minus_one(7));     // 2^3-1
static_assert( detail::is_power_of_two_minus_one(1023));  // 2^10-1
static_assert( detail::is_power_of_two_minus_one(4095));  // 2^12-1
static_assert(!detail::is_power_of_two_minus_one(0));
static_assert(!detail::is_power_of_two_minus_one(2));
static_assert(!detail::is_power_of_two_minus_one(100));
static_assert(!detail::is_power_of_two_minus_one(1024));

// next_valid_pool_size
static_assert(detail::next_valid_pool_size(1000) == 1023);
static_assert(detail::next_valid_pool_size(1023) == 1023);
static_assert(detail::next_valid_pool_size(4095) == 4095);
static_assert(detail::next_valid_pool_size(4096) == 8191);

// constexpr clamp_desc
static_assert(detail::clamp_desc(16,  64, 4096, 1)  == 64);   // clamp to min
static_assert(detail::clamp_desc(8192, 32, 512, 1)  == 512);  // clamp to max
static_assert(detail::clamp_desc(100, 32, 4096, 32) == 128);  // align up
static_assert(detail::clamp_desc(256, 32, 4096, 1)  == 256);  // passthrough

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// TxEngine compile-time checks via static_assert
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// These instantiate the type to verify static_asserts pass.
// The compiler rejects TxEngine<0, 1024> or TxEngine<32, 100>.
using ValidEngine   = TxEngine<32, 1024>;
using SmallEngine   = TxEngine<1, 2>;
using LargeEngine   = TxEngine<64, 4096>;

static_assert(ValidEngine::burst_size() == 32);
static_assert(ValidEngine::ring_size()  == 1024);
static_assert(SmallEngine::burst_size() == 1);
static_assert(SmallEngine::ring_size()  == 2);

// These would fail to compile (uncomment to verify):
// using BadBurst = TxEngine<0, 1024>;   // BurstSize must be > 0
// using BadRing  = TxEngine<32, 100>;   // RingSize must be power of 2

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Runtime tests for validate_config (verifying message content)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValidateConfig, ValidConfigReturnsEmpty) {
    EXPECT_TRUE(validate_config(good_cfg).empty());
}

TEST(ValidateConfig, ZeroRxQueuesMessage) {
    constexpr PlatformConfig cfg{.nb_rx_queues = 0};
    auto msg = validate_config(cfg);
    EXPECT_FALSE(msg.empty());
    EXPECT_NE(msg.find("nb_rx_queues"), std::string_view::npos);
}

TEST(ValidateConfig, BadPoolSizeMessage) {
    constexpr PlatformConfig cfg{.mbuf_pool_size = 1000};
    auto msg = validate_config(cfg);
    EXPECT_FALSE(msg.empty());
    EXPECT_NE(msg.find("2^n"), std::string_view::npos);
}

TEST(ValidateConfig, ValidPoolSizes) {
    for (uint32_t v : {uint32_t{1023}, uint32_t{4095}, uint32_t{8191}}) {
        PlatformConfig cfg{.mbuf_pool_size = v};
        EXPECT_TRUE(validate_config(cfg).empty())
            << "Expected valid for pool_size=" << v;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// clamp_desc runtime tests (complement the static_asserts above)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ClampDesc, ClampsToMinimum) {
    EXPECT_EQ(detail::clamp_desc(16, 64, 4096, 1), 64);
}

TEST(ClampDesc, ClampsToMaximum) {
    EXPECT_EQ(detail::clamp_desc(1024, 32, 512, 1), 512);
}

TEST(ClampDesc, AlignsUp) {
    EXPECT_EQ(detail::clamp_desc(100, 32, 4096, 32), 128);
}

TEST(ClampDesc, PassthroughWhenInRange) {
    EXPECT_EQ(detail::clamp_desc(256, 32, 4096, 1), 256);
}

// ─────────────────────────────────────────────────────────────────────────────
// SpscLogRing runtime tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SpscLogRing, PushPopSingleEntry) {
    SpscLogRing<4> ring;
    EXPECT_TRUE(ring.empty());

    LogEntry in{.level = spdlog::level::warn};
    std::strcpy(in.msg, "test message");

    EXPECT_TRUE(ring.try_push(in));
    EXPECT_FALSE(ring.empty());

    LogEntry out{};
    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.level, spdlog::level::warn);
    EXPECT_STREQ(out.msg, "test message");
    EXPECT_TRUE(ring.empty());
}

TEST(SpscLogRing, PopFromEmptyReturnsFalse) {
    SpscLogRing<4> ring;
    LogEntry e{};
    EXPECT_FALSE(ring.try_pop(e));
}

TEST(SpscLogRing, FullRingRejectsPush) {
    SpscLogRing<4> ring;
    LogEntry e{};

    EXPECT_TRUE(ring.try_push(e));
    EXPECT_TRUE(ring.try_push(e));
    EXPECT_TRUE(ring.try_push(e));
    EXPECT_FALSE(ring.try_push(e));  // 4th push fails
}

TEST(SpscLogRing, FillDrainCycleWithWraparound) {
    SpscLogRing<8> ring;
    LogEntry e{};

    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < 7; ++i)
            EXPECT_TRUE(ring.try_push(e));
        for (int i = 0; i < 7; ++i)
            EXPECT_TRUE(ring.try_pop(e));
        EXPECT_TRUE(ring.empty());
    }
}
