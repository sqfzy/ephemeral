#include <gtest/gtest.h>

#include <cmath>

#include "eph/utils/ema.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// Ema basic tests
// ---------------------------------------------------------------------------

TEST(EmaTest, single_value_initializes) {
    Ema ema(0.5);
    EXPECT_FALSE(ema.initialized());
    EXPECT_DOUBLE_EQ(ema.value(), 0.0);

    double result = ema.update(42.0);
    EXPECT_TRUE(ema.initialized());
    EXPECT_DOUBLE_EQ(result, 42.0);
    EXPECT_DOUBLE_EQ(ema.value(), 42.0);
}

TEST(EmaTest, converges_toward_repeated_value) {
    Ema ema(0.1);  // slow alpha
    (void)ema.update(0.0);

    // Feed constant 100.0 many times — EMA should converge close to 100.
    for (int i = 0; i < 200; ++i) {
        (void)ema.update(100.0);
    }
    EXPECT_NEAR(ema.value(), 100.0, 0.01);
}

TEST(EmaTest, from_period_correctness) {
    // period=9 => alpha = 2/(9+1) = 0.2
    auto ema = Ema::from_period(9);
    EXPECT_DOUBLE_EQ(ema.alpha(), 0.2);

    // period=1 => alpha = 2/(1+1) = 1.0
    auto ema1 = Ema::from_period(1);
    EXPECT_DOUBLE_EQ(ema1.alpha(), 1.0);

    // period=19 => alpha = 2/20 = 0.1
    auto ema19 = Ema::from_period(19);
    EXPECT_DOUBLE_EQ(ema19.alpha(), 0.1);
}

TEST(EmaTest, reset_clears_state) {
    Ema ema(0.5);
    (void)ema.update(100.0);
    (void)ema.update(200.0);
    EXPECT_TRUE(ema.initialized());
    EXPECT_NE(ema.value(), 0.0);

    ema.reset();
    EXPECT_FALSE(ema.initialized());
    EXPECT_DOUBLE_EQ(ema.value(), 0.0);

    // After reset, next update should seed directly.
    double result = ema.update(50.0);
    EXPECT_DOUBLE_EQ(result, 50.0);
}

TEST(EmaTest, alpha_one_tracks_exactly) {
    // alpha=1 means EMA = latest value always.
    Ema ema(1.0);
    (void)ema.update(10.0);
    EXPECT_DOUBLE_EQ(ema.value(), 10.0);

    (void)ema.update(20.0);
    EXPECT_DOUBLE_EQ(ema.value(), 20.0);

    (void)ema.update(-5.0);
    EXPECT_DOUBLE_EQ(ema.value(), -5.0);
}

TEST(EmaTest, alpha_near_zero_is_very_smooth) {
    // alpha very small: EMA should barely move from initial seed.
    Ema ema(0.001);
    (void)ema.update(100.0);

    // Feed 200.0 a few times — should barely budge from 100.
    for (int i = 0; i < 10; ++i) {
        (void)ema.update(200.0);
    }
    // After 10 updates with alpha=0.001:
    // EMA moves very slowly. Should still be close to 100.
    EXPECT_LT(ema.value(), 102.0);
    EXPECT_GT(ema.value(), 100.0);
}

TEST(EmaTest, manual_formula_verification) {
    // Verify exact formula: ema = alpha * value + (1 - alpha) * prev
    const double alpha = 0.3;
    Ema ema(alpha);

    (void)ema.update(10.0);  // seed = 10.0
    EXPECT_DOUBLE_EQ(ema.value(), 10.0);

    double result = ema.update(20.0);
    double expected = alpha * 20.0 + (1.0 - alpha) * 10.0;  // 13.0
    EXPECT_DOUBLE_EQ(result, expected);

    result = ema.update(15.0);
    expected = alpha * 15.0 + (1.0 - alpha) * 13.0;  // 13.6
    EXPECT_DOUBLE_EQ(result, expected);
}

// ---------------------------------------------------------------------------
// EmaCrossover tests
// ---------------------------------------------------------------------------

TEST(EmaCrossoverTest, bullish_cross) {
    // fast_period=2 (alpha=0.667), slow_period=5 (alpha=0.333)
    EmaCrossover cross(2, 5);

    // Seed both EMAs with same value — no signal.
    auto sig = cross.update(100.0);
    EXPECT_EQ(sig, EmaCrossover::Signal::None);

    // Drive price down so fast < slow.
    for (int i = 0; i < 20; ++i) {
        (void)cross.update(80.0);
    }
    EXPECT_LT(cross.fast(), cross.slow() + 1.0);
    // Both should be near 80 now but fast converges faster.

    // Now drive price up sharply — fast should cross above slow.
    bool got_bullish = false;
    for (int i = 0; i < 30; ++i) {
        sig = cross.update(120.0);
        if (sig == EmaCrossover::Signal::BullishCross) {
            got_bullish = true;
            break;
        }
    }
    EXPECT_TRUE(got_bullish) << "Expected bullish crossover when price surges";
}

TEST(EmaCrossoverTest, bearish_cross) {
    EmaCrossover cross(2, 5);

    // Seed and drive price up so fast > slow.
    (void)cross.update(100.0);
    for (int i = 0; i < 20; ++i) {
        (void)cross.update(120.0);
    }

    // Now drive price down sharply — fast should cross below slow.
    bool got_bearish = false;
    for (int i = 0; i < 30; ++i) {
        auto sig = cross.update(80.0);
        if (sig == EmaCrossover::Signal::BearishCross) {
            got_bearish = true;
            break;
        }
    }
    EXPECT_TRUE(got_bearish) << "Expected bearish crossover when price drops";
}

TEST(EmaCrossoverTest, no_signal_on_stable_price) {
    EmaCrossover cross(3, 10);

    // Feed constant price — should never generate a crossover after init.
    (void)cross.update(50.0);  // seed
    for (int i = 0; i < 50; ++i) {
        auto sig = cross.update(50.0);
        EXPECT_EQ(sig, EmaCrossover::Signal::None)
            << "No crossover expected on constant price at iteration " << i;
    }
}

TEST(EmaCrossoverTest, first_update_returns_none) {
    EmaCrossover cross(2, 5);
    // Very first update seeds both EMAs — no crossover possible.
    auto sig = cross.update(100.0);
    EXPECT_EQ(sig, EmaCrossover::Signal::None);
}

TEST(EmaCrossoverTest, fast_and_slow_accessors) {
    EmaCrossover cross(2, 5);
    (void)cross.update(100.0);

    // After seed, both should equal 100.
    EXPECT_DOUBLE_EQ(cross.fast(), 100.0);
    EXPECT_DOUBLE_EQ(cross.slow(), 100.0);

    (void)cross.update(110.0);
    // Fast (alpha=0.667) should respond more than slow (alpha=0.333).
    EXPECT_GT(cross.fast(), cross.slow());
}
