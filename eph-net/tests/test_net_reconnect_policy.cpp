/// @file test_net_reconnect_policy.cpp
/// Unit tests for the Phase-2 `eph::net::ReconnectPolicy`.
///
/// Filename is `test_net_reconnect_policy.cpp` (not `test_reconnect_policy.cpp`)
/// to avoid colliding with the legacy `eph-transport/tests/test_reconnect_policy.cpp`
/// target name in the root xmake autoglob — both modules use
/// `target(path.basename(file))` so the basenames must be unique.

#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>

#include "eph/net/reconnect_policy.hpp"

namespace en = eph::net;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Config validation
// ---------------------------------------------------------------------------

TEST(ReconnectPolicy, ClampsInvalidMultiplier) {
    en::ReconnectPolicyConfig cfg{.multiplier = 0.5};  // nonsense
    en::ReconnectPolicy p{cfg};
    EXPECT_EQ(p.config().multiplier, 2.0);
}

TEST(ReconnectPolicy, ClampsNegativeJitter) {
    en::ReconnectPolicyConfig cfg{.jitter_factor = -0.1};
    en::ReconnectPolicy p{cfg};
    EXPECT_EQ(p.config().jitter_factor, 0.0);
}

TEST(ReconnectPolicy, ClampsExcessiveJitter) {
    en::ReconnectPolicyConfig cfg{.jitter_factor = 1.5};
    en::ReconnectPolicy p{cfg};
    EXPECT_LT(p.config().jitter_factor, 1.0);
}

TEST(ReconnectPolicy, ClampsMaxBelowInitial) {
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 500ms, .max_backoff = 100ms};
    en::ReconnectPolicy p{cfg};
    EXPECT_EQ(p.config().max_backoff, 500ms);
}

// ---------------------------------------------------------------------------
// Attempts / should_reconnect
// ---------------------------------------------------------------------------

TEST(ReconnectPolicy, UnlimitedAttemptsByDefault) {
    en::ReconnectPolicy p{{}};
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(p.should_reconnect());
        (void)p.next_backoff();
    }
}

TEST(ReconnectPolicy, MaxAttemptsHonored) {
    en::ReconnectPolicyConfig cfg{.max_attempts = 3};
    en::ReconnectPolicy p{cfg};
    EXPECT_TRUE(p.should_reconnect());
    (void)p.next_backoff();
    EXPECT_TRUE(p.should_reconnect());
    (void)p.next_backoff();
    EXPECT_TRUE(p.should_reconnect());
    (void)p.next_backoff();
    EXPECT_FALSE(p.should_reconnect());
    EXPECT_EQ(p.attempts(), 3u);
}

// ---------------------------------------------------------------------------
// Backoff growth
// ---------------------------------------------------------------------------

TEST(ReconnectPolicy, NoJitterDeterministicGrowth) {
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 100ms,
        .max_backoff     = 10s,
        .multiplier      = 2.0,
        .jitter_factor   = 0.0,
    };
    en::ReconnectPolicy p{cfg};
    EXPECT_EQ(p.next_backoff(), 100ms);
    EXPECT_EQ(p.next_backoff(), 200ms);
    EXPECT_EQ(p.next_backoff(), 400ms);
    EXPECT_EQ(p.next_backoff(), 800ms);
}

TEST(ReconnectPolicy, CapsAtMaxBackoff) {
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 100ms,
        .max_backoff     = 500ms,
        .multiplier      = 2.0,
        .jitter_factor   = 0.0,
    };
    en::ReconnectPolicy p{cfg};
    // Advance repeatedly; once we hit the cap, every subsequent call returns
    // exactly `max_backoff` (no overshoot, no under-run).
    auto last = 0ms;
    for (int i = 0; i < 20; ++i) {
        last = p.next_backoff();
        EXPECT_LE(last, 500ms) << "exceeded max at iteration " << i;
    }
    EXPECT_EQ(last, 500ms);
}

// ---------------------------------------------------------------------------
// Jitter range
// ---------------------------------------------------------------------------

TEST(ReconnectPolicy, JitterStaysInSymmetricBand) {
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 1000ms,
        .max_backoff     = 1000ms,  // pin base so jitter is the only variable
        .multiplier      = 1.01,
        .jitter_factor   = 0.25,
    };
    en::ReconnectPolicy p{cfg};
    // Sample a batch and confirm every draw lies in [750ms, 1250ms]. Because
    // this is stochastic, we do NOT assert a variance lower bound here —
    // that would flake.
    for (int i = 0; i < 200; ++i) {
        auto d = p.next_backoff();
        EXPECT_GE(d, 750ms) << "sample " << i;
        EXPECT_LE(d, 1250ms) << "sample " << i;
    }
}

TEST(ReconnectPolicy, JitterZeroIsDeterministic) {
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 250ms,
        .max_backoff     = 250ms,
        .multiplier      = 1.01,
        .jitter_factor   = 0.0,
    };
    en::ReconnectPolicy p{cfg};
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(p.next_backoff(), 250ms);
    }
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST(ReconnectPolicy, ResetRewindsAttemptsAndBackoff) {
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 100ms,
        .max_backoff     = 10s,
        .multiplier      = 2.0,
        .jitter_factor   = 0.0,
    };
    en::ReconnectPolicy p{cfg};
    (void)p.next_backoff();
    (void)p.next_backoff();
    (void)p.next_backoff();
    EXPECT_EQ(p.attempts(), 3u);

    p.reset();
    EXPECT_EQ(p.attempts(), 0u);
    EXPECT_EQ(p.next_backoff(), 100ms);  // back to initial
}
