/// @file test_backoff.cpp
/// Unit tests for `eph::utils::ExponentialBackoff` / `ConstantBackoff`.
///
/// Ported from `eph-net/tests/test_net_reconnect_policy.cpp` (the math moved
/// to eph-utils) with the API updated to the merged `next_delay() -> optional`
/// shape, plus new coverage for the multiplier==1.0 constant case, the
/// absorbing-exhaustion invariant, and `ConstantBackoff`.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <gtest/gtest.h>

#include "eph/utils/backoff.hpp"

namespace eu = eph::utils;
using namespace std::chrono_literals;
using Exp = eu::ExponentialBackoff;

// ---------------------------------------------------------------------------
// Config validation (ExponentialBackoff)
// ---------------------------------------------------------------------------

TEST(ExponentialBackoff, ClampsBelowOneMultiplier) {
    Exp p{Exp::Config{.multiplier = 0.5}};  // < 1.0 → 2.0
    EXPECT_EQ(p.config().multiplier, 2.0);
}

TEST(ExponentialBackoff, MultiplierOneIsConstantNotPromoted) {
    // The key behavioral fix vs the legacy ReconnectPolicy, which silently
    // promoted multiplier==1.0 to 2.0. A generic backoff must let 1.0 express
    // a constant delay.
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = 10s,
                      .multiplier      = 1.0,
                      .jitter_factor   = 0.0}};
    EXPECT_EQ(p.config().multiplier, 1.0);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(*p.next_delay(), 100ms) << "iteration " << i;
    }
}

TEST(ExponentialBackoff, ClampsNegativeJitter) {
    Exp p{Exp::Config{.jitter_factor = -0.1}};
    EXPECT_EQ(p.config().jitter_factor, 0.0);
}

TEST(ExponentialBackoff, ClampsExcessiveJitterToZero999) {
    Exp p{Exp::Config{.jitter_factor = 1.0}};
    EXPECT_DOUBLE_EQ(p.config().jitter_factor, 0.999);
    Exp p2{Exp::Config{.jitter_factor = 9999.0}};
    EXPECT_DOUBLE_EQ(p2.config().jitter_factor, 0.999);
}

TEST(ExponentialBackoff, ClampsMaxBelowInitial) {
    Exp p{Exp::Config{.initial_backoff = 500ms, .max_backoff = 100ms}};
    EXPECT_EQ(p.config().max_backoff, 500ms);
}

TEST(ExponentialBackoff, ClampsNegativeInitialBackoff) {
    Exp p{Exp::Config{.initial_backoff = -100ms}};
    EXPECT_GT(p.config().initial_backoff.count(), 0);
    EXPECT_GT(p.next_delay()->count(), 0);
}

TEST(ExponentialBackoff, ClampsZeroInitialBackoff) {
    Exp p{Exp::Config{.initial_backoff = 0ms}};
    EXPECT_GT(p.config().initial_backoff.count(), 0);
}

TEST(ExponentialBackoff, ClampsNaNMultiplier) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = 10000ms,
                      .multiplier      = nan,
                      .jitter_factor   = 0.0}};
    EXPECT_GT(p.next_delay()->count(), 0);
    EXPECT_GT(p.next_delay()->count(), 0);
    EXPECT_LE(p.next_delay()->count(), 10000);
}

TEST(ExponentialBackoff, ClampsInfMultiplier) {
    const double inf = std::numeric_limits<double>::infinity();
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = 10000ms,
                      .multiplier      = inf,
                      .jitter_factor   = 0.0}};
    EXPECT_LE(p.next_delay()->count(), 10000);
    EXPECT_LE(p.next_delay()->count(), 10000);
}

TEST(ExponentialBackoff, ClampsNaNJitterFactor) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = 10000ms,
                      .multiplier      = 2.0,
                      .jitter_factor   = nan}};
    for (int i = 0; i < 5; ++i) {
        auto v = p.next_delay();
        EXPECT_GT(v->count(), 0);
        EXPECT_LE(v->count(), 10000);
    }
}

TEST(ExponentialBackoff, ClampsInfJitterFactor) {
    const double inf = std::numeric_limits<double>::infinity();
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = 10000ms,
                      .multiplier      = 2.0,
                      .jitter_factor   = inf}};
    auto v = p.next_delay();
    EXPECT_GT(v->count(), 0);
    EXPECT_LE(v->count(), 10000);
}

// ---------------------------------------------------------------------------
// Overflow / saturation
// ---------------------------------------------------------------------------

TEST(ExponentialBackoff, UnboundedMaxBackoffDoesNotOverflow) {
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = std::chrono::milliseconds::max(),
                      .multiplier      = 2.0,
                      .jitter_factor   = 0.0}};
    int64_t prev = -1;
    for (int i = 0; i < 100; ++i) {
        auto v = p.next_delay();
        ASSERT_TRUE(v.has_value());  // unlimited attempts
        EXPECT_GE(v->count(), 0) << "iteration " << i;
        EXPECT_GE(v->count(), prev) << "iteration " << i << " went backward";
        prev = v->count();
    }
}

TEST(ExponentialBackoff, UnboundedMaxBackoffWithJitterDoesNotOverflow) {
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = std::chrono::milliseconds::max(),
                      .multiplier      = 2.0,
                      .jitter_factor   = 0.25}};
    int64_t max_seen = -1;
    for (int i = 0; i < 100; ++i) {
        auto v = p.next_delay();
        ASSERT_TRUE(v.has_value());
        EXPECT_GE(v->count(), 0) << "iteration " << i;
        if (v->count() > max_seen) max_seen = v->count();
    }
    EXPECT_GT(max_seen, std::numeric_limits<int64_t>::max() / 4);
}

// ---------------------------------------------------------------------------
// Attempt budget + absorbing exhaustion
// ---------------------------------------------------------------------------

TEST(ExponentialBackoff, UnlimitedAttemptsByDefault) {
    Exp p{Exp::Config{}};
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(p.next_delay().has_value());
    }
}

TEST(ExponentialBackoff, MaxAttemptsHonored) {
    Exp p{Exp::Config{.max_attempts = 3}};
    EXPECT_TRUE(p.next_delay().has_value());
    EXPECT_TRUE(p.next_delay().has_value());
    EXPECT_TRUE(p.next_delay().has_value());
    EXPECT_FALSE(p.next_delay().has_value());
    EXPECT_EQ(p.attempts(), 3u);
}

TEST(ExponentialBackoff, ExhaustionIsAbsorbing) {
    // After the first nullopt, repeated calls must keep returning nullopt
    // WITHOUT advancing attempts_ or current_base_ — otherwise an
    // over-polling consumer (e.g. orchestrator ticking past exhaustion) would
    // silently march the internal base into saturation.
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = 10s,
                      .multiplier      = 2.0,
                      .jitter_factor   = 0.0,
                      .max_attempts    = 2}};
    EXPECT_EQ(*p.next_delay(), 100ms);
    EXPECT_EQ(*p.next_delay(), 200ms);
    for (int i = 0; i < 50; ++i) {
        EXPECT_FALSE(p.next_delay().has_value());
    }
    EXPECT_EQ(p.attempts(), 2u) << "exhausted state must not advance attempts";

    // Reset restarts cleanly at the initial base — proving the base did not
    // drift during the absorbing phase.
    p.reset();
    EXPECT_EQ(*p.next_delay(), 100ms);
    EXPECT_EQ(*p.next_delay(), 200ms);
}

// ---------------------------------------------------------------------------
// Backoff growth + cap
// ---------------------------------------------------------------------------

TEST(ExponentialBackoff, NoJitterDeterministicGrowth) {
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = 10s,
                      .multiplier      = 2.0,
                      .jitter_factor   = 0.0}};
    EXPECT_EQ(*p.next_delay(), 100ms);
    EXPECT_EQ(*p.next_delay(), 200ms);
    EXPECT_EQ(*p.next_delay(), 400ms);
    EXPECT_EQ(*p.next_delay(), 800ms);
}

TEST(ExponentialBackoff, FirstDelayReturnsInitialNoJitter) {
    Exp p{Exp::Config{.initial_backoff = 250ms,
                      .max_backoff     = 10s,
                      .multiplier      = 2.0,
                      .jitter_factor   = 0.0}};
    EXPECT_EQ(*p.next_delay(), 250ms);
    EXPECT_EQ(p.attempts(), 1u);
    EXPECT_EQ(*p.next_delay(), 500ms);
}

TEST(ExponentialBackoff, CapsAtMaxBackoff) {
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = 500ms,
                      .multiplier      = 2.0,
                      .jitter_factor   = 0.0}};
    auto last = 0ms;
    for (int i = 0; i < 20; ++i) {
        last = *p.next_delay();
        EXPECT_LE(last, 500ms) << "exceeded max at iteration " << i;
    }
    EXPECT_EQ(last, 500ms);
}

// ---------------------------------------------------------------------------
// Jitter range
// ---------------------------------------------------------------------------

TEST(ExponentialBackoff, JitterStaysInSymmetricBand) {
    Exp p{Exp::Config{.initial_backoff = 1000ms,
                      .max_backoff     = 1000ms,  // pin base
                      .multiplier      = 1.01,
                      .jitter_factor   = 0.25}};
    for (int i = 0; i < 200; ++i) {
        auto d = *p.next_delay();
        EXPECT_GE(d, 750ms) << "sample " << i;
        EXPECT_LE(d, 1250ms) << "sample " << i;
    }
}

TEST(ExponentialBackoff, JitterZeroIsDeterministic) {
    Exp p{Exp::Config{.initial_backoff = 250ms,
                      .max_backoff     = 250ms,
                      .multiplier      = 1.01,
                      .jitter_factor   = 0.0}};
    for (int i = 0; i < 10; ++i) EXPECT_EQ(*p.next_delay(), 250ms);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST(ExponentialBackoff, ResetRewindsAttemptsAndBackoff) {
    Exp p{Exp::Config{.initial_backoff = 100ms,
                      .max_backoff     = 10s,
                      .multiplier      = 2.0,
                      .jitter_factor   = 0.0}};
    (void)p.next_delay();
    (void)p.next_delay();
    (void)p.next_delay();
    EXPECT_EQ(p.attempts(), 3u);
    p.reset();
    EXPECT_EQ(p.attempts(), 0u);
    EXPECT_EQ(*p.next_delay(), 100ms);
}

TEST(ExponentialBackoff, ResetRefillsMaxAttemptsBudget) {
    Exp p{Exp::Config{.max_attempts = 2}};
    (void)p.next_delay();
    (void)p.next_delay();
    EXPECT_FALSE(p.next_delay().has_value());
    EXPECT_EQ(p.attempts(), 2u);
    p.reset();
    EXPECT_TRUE(p.next_delay().has_value());
    EXPECT_TRUE(p.next_delay().has_value());
    EXPECT_FALSE(p.next_delay().has_value())
        << "second budget must exhaust at the same boundary";
}

TEST(ExponentialBackoff, AttemptsAccessorReflectsExactCount) {
    Exp p{Exp::Config{.jitter_factor = 0.0}};
    EXPECT_EQ(p.attempts(), 0u);
    for (uint32_t i = 1; i <= 5; ++i) {
        (void)p.next_delay();
        EXPECT_EQ(p.attempts(), i);
    }
}

// ---------------------------------------------------------------------------
// ConstantBackoff
// ---------------------------------------------------------------------------

using Const = eu::ConstantBackoff;

TEST(ConstantBackoff, FixedDelayNoJitter) {
    Const c{Const::Config{.delay = 200ms}};
    for (int i = 0; i < 10; ++i) EXPECT_EQ(*c.next_delay(), 200ms);
}

TEST(ConstantBackoff, MaxAttemptsHonoredAndAbsorbing) {
    Const c{Const::Config{.delay = 50ms, .max_attempts = 3}};
    EXPECT_TRUE(c.next_delay().has_value());
    EXPECT_TRUE(c.next_delay().has_value());
    EXPECT_TRUE(c.next_delay().has_value());
    EXPECT_FALSE(c.next_delay().has_value());
    EXPECT_FALSE(c.next_delay().has_value());  // absorbing
    EXPECT_EQ(c.attempts(), 3u);
}

TEST(ConstantBackoff, JitterStaysInBand) {
    Const c{Const::Config{.delay = 1000ms, .jitter_factor = 0.2}};
    for (int i = 0; i < 200; ++i) {
        auto d = *c.next_delay();
        EXPECT_GE(d, 800ms) << "sample " << i;
        EXPECT_LE(d, 1200ms) << "sample " << i;
    }
}

TEST(ConstantBackoff, NegativeDelayClampedToZero) {
    Const c{Const::Config{.delay = -5ms}};
    EXPECT_EQ(c.config().delay, 0ms);
    EXPECT_EQ(*c.next_delay(), 0ms);  // 0 = retry immediately
}

TEST(ConstantBackoff, ResetRefillsBudget) {
    Const c{Const::Config{.delay = 10ms, .max_attempts = 1}};
    EXPECT_TRUE(c.next_delay().has_value());
    EXPECT_FALSE(c.next_delay().has_value());
    c.reset();
    EXPECT_EQ(c.attempts(), 0u);
    EXPECT_TRUE(c.next_delay().has_value());
}

// Concept conformance is also enforced by static_assert in the header; assert
// it here too so the test TU documents the contract.
static_assert(eu::Backoff<eu::ExponentialBackoff>);
static_assert(eu::Backoff<eu::ConstantBackoff>);
