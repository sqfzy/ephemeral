/// @file test_keepalive_config.cpp
/// Unit tests for `eph::net::KeepaliveConfig` — validate() semantics.

#include <chrono>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/net/keepalive_config.hpp"

using eph::core::Error;
using eph::net::KeepaliveConfig;
using namespace std::chrono_literals;

// ═══════════════════════════════════════════════════════════════════════════
// Default construction (disabled state)
// ═══════════════════════════════════════════════════════════════════════════

TEST(KeepaliveConfig, DefaultConstructedIsEmptyAndValid) {
    KeepaliveConfig cfg{};
    EXPECT_TRUE(cfg.empty());
    EXPECT_EQ(cfg.interval.count(), 0);
    EXPECT_EQ(cfg.probes, 3); // documented sane default
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

// ═══════════════════════════════════════════════════════════════════════════
// Active state — interval > 0
// ═══════════════════════════════════════════════════════════════════════════

TEST(KeepaliveConfig, ActiveCfgWithDefaultProbesIsValid) {
    KeepaliveConfig cfg{.interval = 30s};
    EXPECT_FALSE(cfg.empty());
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

TEST(KeepaliveConfig, ActiveCfgZeroProbesRejected) {
    KeepaliveConfig cfg{.interval = 30s, .probes = 0};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(KeepaliveConfig, ActiveCfgProbeUpperBoundAccepted) {
    KeepaliveConfig cfg{.interval = 30s, .probes = 10};
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

TEST(KeepaliveConfig, ActiveCfgProbeOverUpperBoundRejected) {
    KeepaliveConfig cfg{.interval = 30s, .probes = 11};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

// Disabled with bogus probes — still valid because the field is unused.
TEST(KeepaliveConfig, EmptyIgnoresBogusProbes) {
    KeepaliveConfig cfg{.interval = 0ms, .probes = 0};
    EXPECT_TRUE(cfg.empty());
    EXPECT_TRUE(cfg.validate().has_value());
}

TEST(KeepaliveConfig, EmptyIgnoresExcessiveProbes) {
    KeepaliveConfig cfg{.interval = 0ms, .probes = 250};
    EXPECT_TRUE(cfg.empty());
    EXPECT_TRUE(cfg.validate().has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Negative interval — rejected at the config boundary so neither backend
// reaches the "wrap to a huge cycle delta" / "EINVAL setsockopt" surface.
// ═══════════════════════════════════════════════════════════════════════════

TEST(KeepaliveConfig, NegativeIntervalRejected) {
    KeepaliveConfig cfg{.interval = -1ms, .probes = 3};
    EXPECT_FALSE(cfg.empty()) << "interval != 0 must not be 'empty'";
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(KeepaliveConfig, LargeNegativeIntervalRejected) {
    KeepaliveConfig cfg{.interval = -3600s, .probes = 3};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

// ═══════════════════════════════════════════════════════════════════════════
// Upper-bound interval — rejected to avoid silent truncation when lowered
// into `setsockopt(TCP_KEEPIDLE, int)` (kernel) or wrapping the TSC-cycle
// conversion in `tick_keepalive` (DPDK). 1 day is the documented ceiling.
// ═══════════════════════════════════════════════════════════════════════════

TEST(KeepaliveConfig, OneDayIntervalAccepted) {
    // The exact upper bound is inclusive — useful as a hard ceiling
    // probe without forcing callers to subtract a millisecond.
    KeepaliveConfig cfg{.interval = KeepaliveConfig::kMaxInterval, .probes = 3};
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

TEST(KeepaliveConfig, IntervalAboveOneDayRejected) {
    KeepaliveConfig cfg{
        .interval = KeepaliveConfig::kMaxInterval + 1ms,
        .probes   = 3};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(KeepaliveConfig, ChronoMaxIntervalRejected) {
    // The pre-bound case: the user passes
    // `chrono::milliseconds::max()` (e.g. via a forgotten cast or a
    // misconfigured config-file parse). Without the upper-bound check
    // this would silently corrupt the kernel and DPDK lowering paths.
    KeepaliveConfig cfg{
        .interval = std::chrono::milliseconds::max(),
        .probes   = 3};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}
