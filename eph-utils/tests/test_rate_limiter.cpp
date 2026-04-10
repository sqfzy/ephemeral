#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "eph/utils/rate_limiter.hpp"

using eph::utils::TokenBucket;

namespace {

// Small helper to sleep with millisecond precision for refill tests.
inline void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and getters
// ---------------------------------------------------------------------------

TEST(TokenBucketTest, constructor_starts_full) {
    TokenBucket tb{{.capacity = 10, .refill_per_second = 1.0}};
    EXPECT_EQ(tb.capacity(), 10u);
    EXPECT_DOUBLE_EQ(tb.refill_rate(), 1.0);
    // A freshly constructed bucket should report roughly full capacity.
    EXPECT_GE(tb.available_tokens(), 9.5);
    EXPECT_LE(tb.available_tokens(), 10.0);
}

TEST(TokenBucketTest, getters_return_constructor_values) {
    TokenBucket tb{{.capacity = 1200, .refill_per_second = 20.0}};
    EXPECT_EQ(tb.capacity(), 1200u);
    EXPECT_DOUBLE_EQ(tb.refill_rate(), 20.0);
}

TEST(TokenBucketTest, non_positive_refill_is_clamped_to_zero) {
    TokenBucket tb{{.capacity = 5, .refill_per_second = -1.0}};
    EXPECT_DOUBLE_EQ(tb.refill_rate(), 0.0);
    // Still initially full.
    EXPECT_TRUE(tb.try_acquire(5));
    // No refill will ever occur — next call fails.
    sleep_ms(5);
    EXPECT_FALSE(tb.try_acquire(1));
}

// ---------------------------------------------------------------------------
// Basic acquire
// ---------------------------------------------------------------------------

TEST(TokenBucketTest, try_acquire_one_on_full_bucket_succeeds) {
    TokenBucket tb{{.capacity = 1, .refill_per_second = 0.1}};
    EXPECT_TRUE(tb.try_acquire(1));
}

TEST(TokenBucketTest, exhaust_then_fail) {
    // Very slow refill so the window is effectively static for the test.
    TokenBucket tb{{.capacity = 5, .refill_per_second = 0.0001}};
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(tb.try_acquire(1)) << "i=" << i;
    }
    EXPECT_FALSE(tb.try_acquire(1));
}

TEST(TokenBucketTest, weighted_acquire_deducts_weight) {
    TokenBucket tb{{.capacity = 10, .refill_per_second = 0.0001}};
    EXPECT_TRUE(tb.try_acquire(5));   // 10 -> 5
    EXPECT_TRUE(tb.try_acquire(5));   // 5  -> 0
    EXPECT_FALSE(tb.try_acquire(1));
}

TEST(TokenBucketTest, weight_greater_than_capacity_always_fails) {
    TokenBucket tb{{.capacity = 10, .refill_per_second = 1.0}};
    EXPECT_FALSE(tb.try_acquire(11));
    EXPECT_FALSE(tb.try_acquire(1000));
    // The bucket should still be full — failed acquire must not partially
    // deduct.
    EXPECT_GE(tb.available_tokens(), 9.5);
}

TEST(TokenBucketTest, partial_weight_deduction_on_insufficient_tokens_fails_atomically) {
    TokenBucket tb{{.capacity = 10, .refill_per_second = 0.0001}};
    EXPECT_TRUE(tb.try_acquire(7));  // 10 -> 3
    // Now only 3 tokens; asking for 5 should fail WITHOUT deducting.
    EXPECT_FALSE(tb.try_acquire(5));
    // Remaining 3 should still be there.
    EXPECT_TRUE(tb.try_acquire(3));
    EXPECT_FALSE(tb.try_acquire(1));
}

TEST(TokenBucketTest, zero_weight_is_free_noop_success) {
    TokenBucket tb{{.capacity = 2, .refill_per_second = 0.0001}};
    EXPECT_TRUE(tb.try_acquire(0));
    EXPECT_TRUE(tb.try_acquire(0));
    // Still 2 tokens available.
    EXPECT_TRUE(tb.try_acquire(1));
    EXPECT_TRUE(tb.try_acquire(1));
    EXPECT_FALSE(tb.try_acquire(1));
}

// ---------------------------------------------------------------------------
// Refill behaviour
// ---------------------------------------------------------------------------

TEST(TokenBucketTest, refill_replenishes_after_elapsed_time) {
    // 1000 tokens/sec = 1 token/ms.  Drain then wait 50 ms.
    TokenBucket tb{{.capacity = 100, .refill_per_second = 1000.0}};
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(tb.try_acquire(1));
    }
    EXPECT_FALSE(tb.try_acquire(1));

    sleep_ms(80);  // ~80 tokens should refill (allow for clock fuzz)

    int ok = 0;
    for (int i = 0; i < 50; ++i) {
        if (tb.try_acquire(1)) ++ok;
    }
    // Lower bound fairly loose to tolerate CI jitter; upper bound caps
    // at capacity to ensure we don't over-refill.
    EXPECT_GE(ok, 30);
    EXPECT_LE(ok, 100);
}

TEST(TokenBucketTest, long_idle_caps_at_capacity) {
    TokenBucket tb{{.capacity = 5, .refill_per_second = 10000.0}};
    // Drain.
    for (int i = 0; i < 5; ++i) (void)tb.try_acquire(1);

    // Sleep much longer than needed to overflow a naive refill — the
    // bucket must clamp at capacity_, not grow unbounded.
    sleep_ms(50);

    // At 10000/s for 50 ms that's 500 tokens of refill — must clamp to 5.
    EXPECT_LE(tb.available_tokens(), 5.0 + 1e-6);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(tb.try_acquire(1)) << "i=" << i;
    }
    EXPECT_FALSE(tb.try_acquire(1));
}

TEST(TokenBucketTest, burst_then_rate_limit) {
    // Capacity 20, refill 200/sec → burst up to 20 then steady state.
    TokenBucket tb{{.capacity = 20, .refill_per_second = 200.0}};

    // Consume the initial burst instantly.
    int burst = 0;
    for (int i = 0; i < 20; ++i) {
        if (tb.try_acquire(1)) ++burst;
    }
    EXPECT_EQ(burst, 20);
    // Next call should fail (no time elapsed for refill).
    EXPECT_FALSE(tb.try_acquire(1));
}

TEST(TokenBucketTest, very_slow_refill_rate_works) {
    // 0.5 tokens/sec = 1 token every 2 seconds.
    TokenBucket tb{{.capacity = 1, .refill_per_second = 0.5}};
    EXPECT_TRUE(tb.try_acquire(1));
    EXPECT_FALSE(tb.try_acquire(1));
    // Don't actually wait 2 s in a unit test — just confirm the bucket
    // reports fractional refill over a small window.
    sleep_ms(100);  // ~0.05 tokens
    EXPECT_LT(tb.available_tokens(), 1.0);
    EXPECT_GE(tb.available_tokens(), 0.0);
}

TEST(TokenBucketTest, very_fast_refill_rate_works) {
    // 1e7 tokens/sec — one token every 100 ns.
    TokenBucket tb{{.capacity = 1000000, .refill_per_second = 1e7}};
    // Drain 1000 tokens.
    for (int i = 0; i < 1000; ++i) (void)tb.try_acquire(1);
    // Still heavily full.
    EXPECT_GE(tb.available_tokens(), 1000.0);
}

// ---------------------------------------------------------------------------
// available_tokens
// ---------------------------------------------------------------------------

TEST(TokenBucketTest, available_tokens_reflects_drain) {
    TokenBucket tb{{.capacity = 10, .refill_per_second = 0.0001}};
    const double before = tb.available_tokens();
    ASSERT_TRUE(tb.try_acquire(3));
    const double after = tb.available_tokens();
    EXPECT_LT(after, before);
    EXPECT_NEAR(after, before - 3.0, 0.1);
}

TEST(TokenBucketTest, available_tokens_never_exceeds_capacity) {
    TokenBucket tb{{.capacity = 10, .refill_per_second = 1000000.0}};
    sleep_ms(10);
    EXPECT_LE(tb.available_tokens(), 10.0 + 1e-9);
}

// ---------------------------------------------------------------------------
// Thread safety
// ---------------------------------------------------------------------------

TEST(TokenBucketTest, multi_thread_try_acquire_is_consistent) {
    // 4 threads each attempt 10000 acquires.  With a slow refill, the
    // total successes must not exceed capacity plus refill budget.
    constexpr int kThreads       = 4;
    constexpr int kPerThr        = 10000;
    constexpr std::uint32_t kCap = 100;

    TokenBucket tb{{.capacity = kCap, .refill_per_second = 0.0001}};

    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::atomic<bool> go{false};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kPerThr; ++i) {
                if (tb.try_acquire(1)) {
                    successes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    // With near-zero refill, successes should be == capacity (100),
    // with a tiny slop for refill granularity.
    EXPECT_GE(successes.load(), static_cast<int>(kCap));
    EXPECT_LE(successes.load(), static_cast<int>(kCap) + 2);
}

TEST(TokenBucketTest, stress_multi_thread_no_crash_no_overshoot) {
    // Larger scale stress run to exercise mutex contention.
    constexpr int kThreads       = 8;
    constexpr int kPerThr        = 20000;
    constexpr std::uint32_t kCap = 500;

    TokenBucket tb{{.capacity = kCap, .refill_per_second = 1000.0}};  // modest refill

    std::atomic<long long> successes{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::atomic<bool> go{false};

    const auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kPerThr; ++i) {
                if (tb.try_acquire(1)) {
                    successes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    const long long upper  = static_cast<long long>(kCap) +
                             static_cast<long long>(1000.0 * elapsed_s + 50.0);

    EXPECT_GT(successes.load(), 0);
    // Total must not exceed initial capacity + refill budget for the
    // observed elapsed window (plus a small slop for clock skew).
    EXPECT_LE(successes.load(), upper)
        << "elapsed=" << elapsed_s << "s successes=" << successes.load();
}

TEST(TokenBucketTest, thread_safe_weighted_acquire) {
    // Verify weighted acquires interleave correctly.
    TokenBucket tb{{.capacity = 100, .refill_per_second = 0.0001}};
    std::atomic<int> total_weight{0};
    std::vector<std::thread> threads;
    std::atomic<bool> go{false};

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) {}
            const std::uint32_t w = static_cast<std::uint32_t>((t % 3) + 1);
            for (int i = 0; i < 1000; ++i) {
                if (tb.try_acquire(w)) {
                    total_weight.fetch_add(static_cast<int>(w),
                                           std::memory_order_relaxed);
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    // Total deducted weight must be <= capacity + tiny refill slop.
    EXPECT_LE(total_weight.load(), 102);
    EXPECT_GT(total_weight.load(), 0);
}

// ---------------------------------------------------------------------------
// Type traits
// ---------------------------------------------------------------------------

TEST(TokenBucketTest, is_not_copyable_or_movable) {
    static_assert(!std::is_copy_constructible_v<TokenBucket>);
    static_assert(!std::is_move_constructible_v<TokenBucket>);
    static_assert(!std::is_copy_assignable_v<TokenBucket>);
    static_assert(!std::is_move_assignable_v<TokenBucket>);
    SUCCEED();
}

TEST(TokenBucketTest, is_not_polymorphic) {
    static_assert(!std::is_polymorphic_v<TokenBucket>);
    SUCCEED();
}
