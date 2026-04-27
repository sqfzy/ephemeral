/// @file test_net_reconnect_policy.cpp
/// Unit tests for `eph::net::ReconnectPolicy`.
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

// ─── round-51 boundary additions ─────────────────────────────────────
//
// Gaps in the existing matrix: the max_attempts budget after reset, the
// post-clamp value of excessive jitter (only "< 1.0" was pinned, not
// the documented 0.999 saturation point), and the first-call returns
// initial_backoff property (only covered indirectly via deterministic
// growth).

TEST(ReconnectPolicy, ResetRefillsMaxAttemptsBudget) {
    // The key compliance contract: after a successful reconnect (reset
    // is called), the policy must allow another full budget of attempts.
    // A bug that conflated reset() with "preserve attempts_" would
    // permanently lock out the connection after the first exhaustion.
    en::ReconnectPolicyConfig cfg{.max_attempts = 2};
    en::ReconnectPolicy p{cfg};

    // Burn through the first budget.
    (void)p.next_backoff();
    (void)p.next_backoff();
    EXPECT_FALSE(p.should_reconnect());
    EXPECT_EQ(p.attempts(), 2u);

    // After reset, the budget must refresh.
    p.reset();
    EXPECT_TRUE(p.should_reconnect());
    EXPECT_EQ(p.attempts(), 0u);
    (void)p.next_backoff();
    EXPECT_TRUE(p.should_reconnect());
    (void)p.next_backoff();
    EXPECT_FALSE(p.should_reconnect())
        << "second budget must exhaust at the same boundary";
}

TEST(ReconnectPolicy, ExcessiveJitterClampedToZero999) {
    // The doc on the ctor pins the saturation value: jitter_factor >= 1.0
    // is clamped to 0.999. The existing test only verifies "< 1.0",
    // which would still pass if some refactor changed it to e.g. 0.5.
    // Pin the actual saturation value so accidental drift is caught.
    en::ReconnectPolicyConfig cfg{.jitter_factor = 1.0};
    en::ReconnectPolicy p{cfg};
    EXPECT_DOUBLE_EQ(p.config().jitter_factor, 0.999);

    en::ReconnectPolicyConfig cfg2{.jitter_factor = 9999.0};
    en::ReconnectPolicy p2{cfg2};
    EXPECT_DOUBLE_EQ(p2.config().jitter_factor, 0.999);
}

TEST(ReconnectPolicy, FirstNextBackoffReturnsInitialNoJitter) {
    // The doc says the first `next_backoff()` returns "roughly
    // initial_backoff". With jitter_factor=0 it must return EXACTLY
    // initial_backoff. Catches a refactor that swaps pre/post-advance
    // and would surface 2*initial on the first call.
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 250ms,
        .max_backoff     = 10s,
        .multiplier      = 2.0,
        .jitter_factor   = 0.0,
    };
    en::ReconnectPolicy p{cfg};
    EXPECT_EQ(p.next_backoff(), 250ms);
    EXPECT_EQ(p.attempts(), 1u);
    EXPECT_EQ(p.next_backoff(), 500ms);
}

TEST(ReconnectPolicy, AttemptsAccessorReflectsExactCount) {
    // attempts() must increment exactly once per next_backoff() call.
    // Catches refactors that double-increment or reset the counter at
    // odd moments.
    en::ReconnectPolicyConfig cfg{.jitter_factor = 0.0};
    en::ReconnectPolicy p{cfg};
    EXPECT_EQ(p.attempts(), 0u);
    for (uint32_t i = 1; i <= 5; ++i) {
        (void)p.next_backoff();
        EXPECT_EQ(p.attempts(), i);
    }
}
