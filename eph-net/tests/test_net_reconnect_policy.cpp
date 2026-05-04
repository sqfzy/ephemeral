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

TEST(ReconnectPolicy, ClampsNegativeInitialBackoff) {
    // Negative initial_backoff would feed back into the multiplier each
    // iteration: -100ms × 2 = -200ms, then min(-200ms, max_backoff). With
    // max_backoff defaulted to 1600ms (positive), std::min picks the
    // negative value and current_base_ marches monotonically more
    // negative every call — eventually `count() * multiplier` overflows
    // double range and the static_cast<int64_t> is UB. Treat any
    // non-positive initial_backoff as a configuration error and clamp to
    // the documented default (100ms) so the policy stays well-defined.
    en::ReconnectPolicyConfig cfg{.initial_backoff = -100ms};
    en::ReconnectPolicy p{cfg};
    EXPECT_GT(p.config().initial_backoff.count(), 0)
        << "initial_backoff must be clamped to a positive value";
    // And the first emitted backoff must also be positive.
    EXPECT_GT(p.next_backoff().count(), 0);
}

TEST(ReconnectPolicy, ClampsZeroInitialBackoff) {
    // Zero initial_backoff is effectively a busy loop — sleep_for(0) on
    // most platforms yields once and immediately retries, hammering the
    // remote endpoint at line rate. Treat as configuration error.
    en::ReconnectPolicyConfig cfg{.initial_backoff = 0ms};
    en::ReconnectPolicy p{cfg};
    EXPECT_GT(p.config().initial_backoff.count(), 0);
}

TEST(ReconnectPolicy, UnboundedMaxBackoffDoesNotOverflow) {
    // With max_backoff = milliseconds::max() and multiplier = 2, the
    // pre-fix code computed `current_base_.count() * 2.0` once
    // current_base_ approached INT64_MAX, producing a double value
    // > INT64_MAX, and `static_cast<int64_t>(huge)` is UB per
    // [conv.fpint]. Cap the growth defensively so we never feed the
    // cast a value out of int64_t range.
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 100ms,
        .max_backoff     = std::chrono::milliseconds::max(),
        .multiplier      = 2.0,
        .jitter_factor   = 0.0,
    };
    en::ReconnectPolicy p{cfg};
    // 100 iterations is already well past INT64_MAX/2 in the pre-fix
    // math (~63 doublings exhausts the range). The post-fix code must
    // run cleanly without UB and the returned value must stay
    // non-negative on every iteration.
    int64_t prev = -1;
    for (int i = 0; i < 100; ++i) {
        auto v = p.next_backoff();
        EXPECT_GE(v.count(), 0)
            << "iteration " << i << " produced a negative backoff "
            << v.count() << "ms — likely int64 overflow from "
            << "unbounded exponential growth";
        // Monotonicity guard: with jitter=0 and growing exponentially up
        // to a saturation ceiling, the sequence must be non-decreasing.
        // A wrap-to-negative would surface as v < prev between iterations
        // post-saturation, even when the magnitude check above passes.
        EXPECT_GE(v.count(), prev)
            << "iteration " << i << " went backward: " << v.count()
            << " < " << prev << " — saturation cap was breached";
        prev = v.count();
    }
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

// NaN / Inf clamps. Without these, a NaN multiplier slips past `<=1.0`
// (NaN comparisons return false), then `current_base.count() * NaN
// = NaN` flows into `static_cast<int64_t>(NaN)` inside next_backoff,
// which is UB per [conv.fpint]. Same hazard for jitter_factor —
// `std::uniform_real_distribution<double>(NaN, NaN)` is also UB.
// The clamps must use isfinite to catch both NaN and +-Inf.
TEST(ReconnectPolicy, ClampsNaNMultiplier) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 100ms,
        .max_backoff     = 10000ms,
        .multiplier      = nan,
        .jitter_factor   = 0.0,
    };
    en::ReconnectPolicy p{cfg};
    auto first  = p.next_backoff();
    auto second = p.next_backoff();
    auto third  = p.next_backoff();
    EXPECT_GT(first.count(),  0);
    EXPECT_GT(second.count(), 0);
    EXPECT_GT(third.count(),  0);
    EXPECT_LE(third.count(), 10000);
}

TEST(ReconnectPolicy, ClampsInfMultiplier) {
    const double inf = std::numeric_limits<double>::infinity();
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 100ms,
        .max_backoff     = 10000ms,
        .multiplier      = inf,
        .jitter_factor   = 0.0,
    };
    en::ReconnectPolicy p{cfg};
    auto first  = p.next_backoff();
    auto second = p.next_backoff();
    EXPECT_LE(first.count(), 10000);
    EXPECT_LE(second.count(), 10000);
}

TEST(ReconnectPolicy, ClampsNaNJitterFactor) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 100ms,
        .max_backoff     = 10000ms,
        .multiplier      = 2.0,
        .jitter_factor   = nan,
    };
    en::ReconnectPolicy p{cfg};
    for (int i = 0; i < 5; ++i) {
        auto v = p.next_backoff();
        EXPECT_GT(v.count(), 0);
        EXPECT_LE(v.count(), 10000);
    }
}

TEST(ReconnectPolicy, ClampsInfJitterFactor) {
    const double inf = std::numeric_limits<double>::infinity();
    en::ReconnectPolicyConfig cfg{
        .initial_backoff = 100ms,
        .max_backoff     = 10000ms,
        .multiplier      = 2.0,
        .jitter_factor   = inf,
    };
    en::ReconnectPolicy p{cfg};
    auto v = p.next_backoff();
    EXPECT_GT(v.count(), 0);
    EXPECT_LE(v.count(), 10000);
}
