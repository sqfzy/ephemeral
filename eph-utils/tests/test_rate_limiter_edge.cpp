/// @file test_rate_limiter_edge.cpp
/// Supplemental edge-case tests for TokenBucket — capacity=0, extreme rates,
/// and boundary conditions not covered by test_rate_limiter.cpp.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>

#include "eph/utils/rate_limiter.hpp"

using eph::utils::TokenBucket;

// ---------------------------------------------------------------------------
// capacity = 0: degenerate bucket
// ---------------------------------------------------------------------------

TEST(TokenBucketEdge, capacity_zero_rejects_weight_one) {
    TokenBucket tb{{.capacity = 0, .refill_per_second = 100.0}};
    EXPECT_EQ(tb.capacity(), 0u);
    // weight=1 > capacity=0 → always fails
    EXPECT_FALSE(tb.try_acquire(1));
}

TEST(TokenBucketEdge, capacity_zero_weight_zero_succeeds) {
    TokenBucket tb{{.capacity = 0, .refill_per_second = 100.0}};
    // weight=0 is always a free no-op
    EXPECT_TRUE(tb.try_acquire(0));
}

TEST(TokenBucketEdge, capacity_zero_available_tokens_is_zero) {
    TokenBucket tb{{.capacity = 0, .refill_per_second = 1000.0}};
    EXPECT_LE(tb.available_tokens(), 0.0 + 1e-9);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // Even after refill time, clamped to capacity=0
    EXPECT_LE(tb.available_tokens(), 0.0 + 1e-9);
}

// ---------------------------------------------------------------------------
// capacity = 1: minimal useful bucket
// ---------------------------------------------------------------------------

TEST(TokenBucketEdge, capacity_one_single_token_then_empty) {
    TokenBucket tb{{.capacity = 1, .refill_per_second = 0.0001}};
    EXPECT_TRUE(tb.try_acquire(1));
    EXPECT_FALSE(tb.try_acquire(1));
}

TEST(TokenBucketEdge, capacity_one_weight_two_always_fails) {
    TokenBucket tb{{.capacity = 1, .refill_per_second = 1000.0}};
    // weight > capacity → immediate reject
    EXPECT_FALSE(tb.try_acquire(2));
    // Original token still there
    EXPECT_TRUE(tb.try_acquire(1));
}

// ---------------------------------------------------------------------------
// refill_per_second = 0: pure burst-then-empty budget
// ---------------------------------------------------------------------------

TEST(TokenBucketEdge, zero_refill_exhausts_permanently) {
    TokenBucket tb{{.capacity = 3, .refill_per_second = 0.0}};
    EXPECT_DOUBLE_EQ(tb.refill_rate(), 0.0);
    EXPECT_TRUE(tb.try_acquire(1));
    EXPECT_TRUE(tb.try_acquire(1));
    EXPECT_TRUE(tb.try_acquire(1));
    EXPECT_FALSE(tb.try_acquire(1));
    // Wait doesn't help
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(tb.try_acquire(1));
}

// ---------------------------------------------------------------------------
// Large weight near capacity boundary
// ---------------------------------------------------------------------------

TEST(TokenBucketEdge, weight_equals_capacity_succeeds_once) {
    TokenBucket tb{{.capacity = 1000, .refill_per_second = 0.0001}};
    EXPECT_TRUE(tb.try_acquire(1000));
    EXPECT_FALSE(tb.try_acquire(1));
}

TEST(TokenBucketEdge, weight_one_over_capacity_fails) {
    TokenBucket tb{{.capacity = 1000, .refill_per_second = 0.0001}};
    EXPECT_FALSE(tb.try_acquire(1001));
    // Bucket is still full — the failed request didn't drain
    EXPECT_TRUE(tb.try_acquire(1000));
}

// ---------------------------------------------------------------------------
// UINT32_MAX capacity
// ---------------------------------------------------------------------------

TEST(TokenBucketEdge, max_uint32_capacity_accepts_weight_one) {
    TokenBucket tb{{.capacity = UINT32_MAX, .refill_per_second = 0.0}};
    EXPECT_EQ(tb.capacity(), UINT32_MAX);
    EXPECT_TRUE(tb.try_acquire(1));
    // Still has ~4 billion tokens
    EXPECT_GE(tb.available_tokens(), 4e9 - 2.0);
}

// ---------------------------------------------------------------------------
// Extremely high refill rate
// ---------------------------------------------------------------------------

TEST(TokenBucketEdge, extreme_refill_rate_clamps_at_capacity) {
    // 1e15 tokens/sec — refill is effectively instant. The key invariant
    // is that available_tokens never exceeds capacity, even with extreme
    // refill rates and long idle periods.
    TokenBucket tb{{.capacity = 10, .refill_per_second = 1e15}};
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_LE(tb.available_tokens(), 10.0 + 1e-9);
    // Can still drain exactly capacity tokens
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(tb.try_acquire(1));
}

// ---------------------------------------------------------------------------
// Negative refill_per_second is clamped to zero
// ---------------------------------------------------------------------------

TEST(TokenBucketEdge, negative_refill_clamped_to_zero) {
    TokenBucket tb{{.capacity = 5, .refill_per_second = -999.0}};
    EXPECT_DOUBLE_EQ(tb.refill_rate(), 0.0);
}

// ---------------------------------------------------------------------------
// Rapid try_acquire(0) should not affect tokens
// ---------------------------------------------------------------------------

TEST(TokenBucketEdge, many_zero_weight_acquires_dont_drain) {
    TokenBucket tb{{.capacity = 10, .refill_per_second = 0.0}};
    for (int i = 0; i < 10000; ++i) {
        ASSERT_TRUE(tb.try_acquire(0));
    }
    // Still full
    EXPECT_GE(tb.available_tokens(), 9.5);
}
