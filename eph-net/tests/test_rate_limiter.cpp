/// @file test_rate_limiter.cpp
/// Unit tests for the token bucket RateLimiter.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/net/rate_limiter.hpp"

using namespace eph::net;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time verification of constexpr Config validation
// ─────────────────────────────────────────────────────────────────────────────

static_assert(RateLimiter::Config{}.validate().empty(),
    "Default RateLimiter::Config must be valid at compile time");

static_assert(RateLimiter::Config{.rate_per_sec = 1000.0, .burst = 50}.validate().empty(),
    "Explicit valid RateLimiter::Config must pass compile-time validation");

static_assert(!RateLimiter::Config{.rate_per_sec = -1.0, .burst = 10}.validate().empty(),
    "Negative rate_per_sec must fail compile-time validation");

static_assert(!RateLimiter::Config{.rate_per_sec = 100.0, .burst = 0}.validate().empty(),
    "Zero burst must fail compile-time validation");

static_assert(RateLimiter::Config{.rate_per_sec = 0.0, .burst = 10}.validate().empty(),
    "Zero rate with valid burst must pass compile-time validation");

// ─────────────────────────────────────────────────────────────────────────────
// Basic token bucket semantics
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, BurstAllowsInitialBurst) {
    // A limiter with burst=10 should allow 10 immediate acquisitions.
    RateLimiter rl(100.0, 10);

    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(rl.try_acquire()) << "acquisition " << i << " should succeed";
    }
    // 11th should fail — burst exhausted, not enough time for refill.
    EXPECT_FALSE(rl.try_acquire());
}

TEST(RateLimiter, TryAcquireMultipleTokens) {
    RateLimiter rl(1000.0, 20);

    EXPECT_TRUE(rl.try_acquire(10));
    EXPECT_TRUE(rl.try_acquire(10));
    // Only 20 tokens were available; next request for 1 should fail immediately.
    EXPECT_FALSE(rl.try_acquire(1));
}

TEST(RateLimiter, TryAcquireReturnsFalseWhenExhausted) {
    RateLimiter rl(10.0, 1);

    EXPECT_TRUE(rl.try_acquire());
    EXPECT_FALSE(rl.try_acquire());
    EXPECT_FALSE(rl.try_acquire());
}

TEST(RateLimiter, TryAcquireExceedingBurstFails) {
    // Requesting more tokens than the burst capacity should always fail.
    RateLimiter rl(1000.0, 5);
    EXPECT_FALSE(rl.try_acquire(6));
    // Tokens should not have been consumed by the failed request.
    EXPECT_TRUE(rl.try_acquire(5));
}

// ─────────────────────────────────────────────────────────────────────────────
// Token refill
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, TokensRefillOverTime) {
    // Rate = 1000/sec => 1 token per millisecond.
    RateLimiter rl(1000.0, 5);

    // Exhaust all tokens.
    EXPECT_TRUE(rl.try_acquire(5));
    EXPECT_FALSE(rl.try_acquire());

    // Wait enough time for at least 1 token to refill.
    // 5ms at 1000/s = 5 tokens expected.
    std::this_thread::sleep_for(10ms);

    EXPECT_TRUE(rl.try_acquire())
        << "at least 1 token should have refilled after 10ms at 1000/s";
}

TEST(RateLimiter, RefillCapsAtBurst) {
    RateLimiter rl(100000.0, 3);

    // Consume 1 token, wait for refill, then verify we never exceed burst.
    EXPECT_TRUE(rl.try_acquire(1));
    std::this_thread::sleep_for(50ms);  // Way more than enough to refill

    // Should have 3 tokens (capped at burst), not more.
    EXPECT_TRUE(rl.try_acquire(3));
    EXPECT_FALSE(rl.try_acquire(1));
}

// ─────────────────────────────────────────────────────────────────────────────
// Rate limiting sustained throughput
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, RateLimitsSustainedThroughput) {
    // Rate = 100/sec, burst = 1.
    // Over 100ms we should get roughly 10 tokens (plus the initial 1).
    RateLimiter rl(100.0, 1);

    // Consume initial burst.
    EXPECT_TRUE(rl.try_acquire());

    const auto start = std::chrono::steady_clock::now();
    int acquired = 0;

    // Try to acquire as many as possible in 100ms.
    while (std::chrono::steady_clock::now() - start < 100ms) {
        if (rl.try_acquire()) {
            ++acquired;
        }
        std::this_thread::sleep_for(1ms);
    }

    // At 100/s over 100ms, expect ~10 tokens. Allow generous range due to
    // scheduling jitter, but it should not be wildly off.
    EXPECT_GE(acquired, 5)  << "too few tokens acquired — rate too slow?";
    EXPECT_LE(acquired, 20) << "too many tokens acquired — rate not limiting?";
}

// ─────────────────────────────────────────────────────────────────────────────
// Zero rate
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, ZeroRateBlocksAfterBurst) {
    // Rate = 0 means no refill. Only the initial burst is available.
    RateLimiter rl(0.0, 3);

    EXPECT_TRUE(rl.try_acquire());
    EXPECT_TRUE(rl.try_acquire());
    EXPECT_TRUE(rl.try_acquire());

    // No more tokens — rate is 0, so waiting won't help.
    std::this_thread::sleep_for(10ms);
    EXPECT_FALSE(rl.try_acquire());
}

// ─────────────────────────────────────────────────────────────────────────────
// Reset
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, ResetRefillsTokens) {
    RateLimiter rl(10.0, 5);

    // Exhaust all tokens.
    EXPECT_TRUE(rl.try_acquire(5));
    EXPECT_FALSE(rl.try_acquire());

    // Reset should refill to burst capacity.
    rl.reset();
    EXPECT_TRUE(rl.try_acquire(5));
}

// ─────────────────────────────────────────────────────────────────────────────
// available() monitoring
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, AvailableReportsTokenCount) {
    RateLimiter rl(100.0, 10);

    EXPECT_DOUBLE_EQ(rl.available(), 10.0);

    (void)rl.try_acquire(3);
    // available() may be slightly above 7.0 due to elapsed time, but close.
    EXPECT_GE(rl.available(), 7.0);
    EXPECT_LE(rl.available(), 10.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// acquire() (blocking)
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, AcquireBlocksUntilTokenAvailable) {
    // Rate = 10000/s (1 token per 0.1ms), burst = 1.
    RateLimiter rl(10000.0, 1);

    // Consume the initial token.
    EXPECT_TRUE(rl.try_acquire());

    // acquire() should block briefly, then succeed once a token refills.
    const auto start = std::chrono::steady_clock::now();
    rl.acquire();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Should have taken at least ~0.1ms but not unreasonably long.
    // We use a generous upper bound to avoid flaky tests.
    EXPECT_LT(elapsed, 100ms) << "acquire() took too long to unblock";
}

TEST(RateLimiter, AcquireTimesOutWithZeroRate) {
    // Rate=0 and burst already exhausted: acquire() should time out and return false,
    // not spin indefinitely.
    RateLimiter rl(0.0, 1);
    EXPECT_TRUE(rl.try_acquire()); // exhaust burst

    const auto start = std::chrono::steady_clock::now();
    bool result = rl.acquire(1, std::chrono::milliseconds{100});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result) << "acquire() should return false on timeout";
    EXPECT_GE(elapsed, 90ms) << "should have waited near the timeout duration";
    EXPECT_LT(elapsed, 500ms) << "should not wait much longer than timeout";
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread safety
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, ConcurrentAcquireDoesNotOverConsume) {
    // Burst = 1000, rate = 0 (no refill). Two threads race to consume tokens.
    // Total consumed across both threads must equal exactly 1000.
    constexpr std::size_t kBurst = 1000;
    RateLimiter rl(0.0, kBurst);

    std::atomic<std::size_t> total_acquired{0};

    auto worker = [&]() {
        std::size_t local = 0;
        while (rl.try_acquire()) {
            ++local;
        }
        total_acquired.fetch_add(local, std::memory_order_relaxed);
    };

    std::thread t1(worker);
    std::thread t2(worker);
    t1.join();
    t2.join();

    EXPECT_EQ(total_acquired.load(), kBurst)
        << "concurrent threads consumed " << total_acquired.load()
        << " tokens, expected exactly " << kBurst;
}

TEST(RateLimiter, ConcurrentAcquireWithRefill) {
    // Rate = 100000/s, burst = 10. Two threads acquire for 50ms.
    // Verify no crashes/data races and that both threads make progress.
    RateLimiter rl(100000.0, 10);

    std::atomic<int> count1{0};
    std::atomic<int> count2{0};

    auto worker = [&](std::atomic<int>& count) {
        const auto deadline = std::chrono::steady_clock::now() + 50ms;
        while (std::chrono::steady_clock::now() < deadline) {
            if (rl.try_acquire()) {
                count.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    };

    std::thread t1(worker, std::ref(count1));
    std::thread t2(worker, std::ref(count2));
    t1.join();
    t2.join();

    // Both threads should have acquired at least some tokens.
    EXPECT_GT(count1.load(), 0) << "thread 1 acquired zero tokens";
    EXPECT_GT(count2.load(), 0) << "thread 2 acquired zero tokens";

    const int total = count1.load() + count2.load();
    // At 100000/s over 50ms = ~5000, plus initial burst of 10.
    // Allow wide range due to scheduling, but should be in the right ballpark.
    EXPECT_GT(total, 100) << "total acquired suspiciously low";
}

// ─────────────────────────────────────────────────────────────────────────────
// Config validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiterConfig, ValidDefaultConfig) {
    RateLimiter::Config cfg;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(RateLimiterConfig, ValidExplicitConfig) {
    RateLimiter::Config cfg{.rate_per_sec = 1000.0, .burst = 50};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(RateLimiterConfig, ZeroRateIsValid) {
    // Zero rate means no refill — only initial burst is available.
    RateLimiter::Config cfg{.rate_per_sec = 0.0, .burst = 10};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(RateLimiterConfig, NegativeRateIsInvalid) {
    RateLimiter::Config cfg{.rate_per_sec = -1.0, .burst = 10};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("rate_per_sec"), std::string_view::npos);
}

TEST(RateLimiterConfig, ZeroBurstIsInvalid) {
    RateLimiter::Config cfg{.rate_per_sec = 100.0, .burst = 0};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("burst"), std::string_view::npos);
}

TEST(RateLimiterConfig, ConstructFromConfig) {
    RateLimiter::Config cfg{.rate_per_sec = 500.0, .burst = 20};
    RateLimiter rl(cfg);

    // Should start with full burst capacity.
    EXPECT_DOUBLE_EQ(rl.available(), 20.0);
    EXPECT_TRUE(rl.try_acquire(20));
    EXPECT_FALSE(rl.try_acquire(1));
}

// ─────────────────────────────────────────────────────────────────────────────
// Config dump, to_json, formatter, equality
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiterConfig, DumpContainsKeyFields) {
    RateLimiter::Config cfg{.rate_per_sec = 1200.0, .burst = 50};
    auto d = cfg.dump();
    EXPECT_NE(d.find("1200.00"), std::string::npos);
    EXPECT_NE(d.find("50"), std::string::npos);
}

TEST(RateLimiterConfig, ToJsonValidStructure) {
    RateLimiter::Config cfg{.rate_per_sec = 100.0, .burst = 10};
    auto j = cfg.to_json();
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    EXPECT_NE(j.find("\"rate_per_sec\":100.00"), std::string::npos);
    EXPECT_NE(j.find("\"burst\":10"), std::string::npos);
}

TEST(RateLimiterConfig, Equality) {
    RateLimiter::Config a{.rate_per_sec = 100.0, .burst = 10};
    RateLimiter::Config b{.rate_per_sec = 100.0, .burst = 10};
    RateLimiter::Config c{.rate_per_sec = 200.0, .burst = 10};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(RateLimiterConfig, FormatterContainsKeyFields) {
    RateLimiter::Config cfg{.rate_per_sec = 1200.0, .burst = 50};
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("RateLimiter"), std::string::npos);
    EXPECT_NE(s.find("1200.00"), std::string::npos);
    EXPECT_NE(s.find("burst=50"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Config warnings (advisory diagnostics)
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiterConfig, WarningsEmptyForReasonableConfig) {
    RateLimiter::Config cfg{.rate_per_sec = 1000.0, .burst = 100};
    EXPECT_TRUE(cfg.warnings().empty());
}

TEST(RateLimiterConfig, WarnsOnExcessiveRate) {
    RateLimiter::Config cfg{.rate_per_sec = 2'000'000'000.0, .burst = 10};
    auto w = cfg.warnings();
    EXPECT_FALSE(w.empty());
    bool found = false;
    for (const auto& s : w) {
        if (s.find("billion") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "expected warning about excessive rate";
}

TEST(RateLimiterConfig, WarnsOnLargeBurst) {
    RateLimiter::Config cfg{.rate_per_sec = 100.0, .burst = 2'000'000};
    auto w = cfg.warnings();
    EXPECT_FALSE(w.empty());
    bool found = false;
    for (const auto& s : w) {
        if (s.find("very large") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "expected warning about large burst";
}

TEST(RateLimiterConfig, WarnsOnZeroRateWithBurst) {
    RateLimiter::Config cfg{.rate_per_sec = 0.0, .burst = 10};
    auto w = cfg.warnings();
    EXPECT_FALSE(w.empty());
    bool found = false;
    for (const auto& s : w) {
        if (s.find("never refill") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found) << "expected warning about zero rate";
}

// ─────────────────────────────────────────────────────────────────────────────
// Accessors: burst() and rate_per_ns()
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, BurstAccessorMatchesConfig) {
    RateLimiter rl(500.0, 42);
    EXPECT_DOUBLE_EQ(rl.burst(), 42.0);
}

TEST(RateLimiter, RatePerNsMatchesConfig) {
    RateLimiter rl(1'000'000'000.0, 1);
    // 1e9 tokens/sec = 1.0 token/ns
    EXPECT_DOUBLE_EQ(rl.rate_per_ns(), 1.0);
}

TEST(RateLimiter, RatePerNsZeroRate) {
    RateLimiter rl(0.0, 10);
    EXPECT_DOUBLE_EQ(rl.rate_per_ns(), 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// config() accessor
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, ConfigAccessorReturnsConstructionConfig) {
    RateLimiter::Config cfg{.rate_per_sec = 500.0, .burst = 42};
    RateLimiter rl(cfg);
    EXPECT_EQ(rl.config(), cfg);
}

TEST(RateLimiter, ConfigAccessorFromDirectConstruction) {
    RateLimiter rl(1200.0, 100);
    EXPECT_DOUBLE_EQ(rl.config().rate_per_sec, 1200.0);
    EXPECT_EQ(rl.config().burst, 100u);
}

// ─────────────────────────────────────────────────────────────────────────────
// to_json() — live state snapshot
// ─────────────────────────────────────────────────────────────────────────────

TEST(RateLimiter, ToJsonFullBurst) {
    RateLimiter rl(100.0, 10);
    auto j = rl.to_json();
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    EXPECT_NE(j.find("\"available_tokens\":"), std::string::npos);
    EXPECT_NE(j.find("\"config\":{"), std::string::npos);
    EXPECT_NE(j.find("\"rate_per_sec\":100.00"), std::string::npos);
    EXPECT_NE(j.find("\"burst\":10"), std::string::npos);
}

TEST(RateLimiter, ToJsonAfterExhaustion) {
    RateLimiter rl(0.0, 5);
    ASSERT_TRUE(rl.try_acquire(5));
    auto j = rl.to_json();
    // Tokens should be at or near zero
    EXPECT_NE(j.find("\"available_tokens\":0.00"), std::string::npos);
}

TEST(RateLimiter, ToJsonAfterReset) {
    RateLimiter rl(0.0, 10);
    ASSERT_TRUE(rl.try_acquire(10));
    rl.reset();
    auto j = rl.to_json();
    EXPECT_NE(j.find("\"available_tokens\":10.00"), std::string::npos);
}
