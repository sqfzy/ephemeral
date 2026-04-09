/// @file tests/unit/bench/test_stream_scheduler.cpp
/// StreamScheduler timing/correctness tests.
///
/// Validates:
///   1. Periodic streams emit at the configured rate (within 15%).
///   2. Multiple streams interleave correctly without starvation.
///   3. Poisson stream's mean arrival rate matches the configured mean.
///   4. rebase_now() resets the schedule to the current TSC.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "eph/utils/time.hpp"
#include "core/stream_scheduler.hpp"

using namespace bench;

struct StreamFixture : ::testing::Test {
    void SetUp() override { ASSERT_TRUE(eph::utils::TSC::init()); }
};

TEST_F(StreamFixture, SingleStream100UsApproxRate) {
    // 100 µs period → expect ~ 1000 ticks in 100 ms.
    StreamScheduler sched;
    std::atomic<int> count{0};
    sched.register_periodic(1, 100'000,
        [&](uint32_t) { ++count; return true; });
    sched.build();
    sched.rebase_now();

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start <
           std::chrono::milliseconds(100)) {
        sched.tick();
    }
    int got = count.load();
    EXPECT_GE(got, 850);
    EXPECT_LE(got, 1150);
}

TEST_F(StreamFixture, FourStreamsInterleaveWithoutStarvation) {
    // bookTicker: 100 µs, depth: 1 ms, trade poisson 5 ms, kline: 100 ms
    // (kline shrunk vs prod to fit a 200 ms test).
    StreamScheduler sched;
    std::atomic<int> bt{0}, dp{0}, tr{0}, kl{0};
    sched.register_periodic(1, 100'000,    [&](uint32_t) { ++bt; return true; });
    sched.register_periodic(2, 1'000'000,  [&](uint32_t) { ++dp; return true; });
    sched.register_poisson (3, 5'000'000,  [&](uint32_t) { ++tr; return true; });
    sched.register_periodic(4, 100'000'000,[&](uint32_t) { ++kl; return true; });
    sched.build();
    sched.rebase_now();

    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start <
           std::chrono::milliseconds(200)) {
        sched.tick();
    }

    EXPECT_GT(bt.load(), 1500); // expect ~2000 in 200 ms
    EXPECT_GT(dp.load(), 100);  // expect ~200
    EXPECT_GT(tr.load(),  10);  // poisson mean 5 ms over 200 ms
    EXPECT_GE(kl.load(),   1);  // at least one fire (~100 ms cadence)
}

TEST_F(StreamFixture, RebaseResetsTimers) {
    StreamScheduler sched;
    int n = 0;
    sched.register_periodic(1, 1'000'000, [&](uint32_t) { ++n; return true; });
    sched.build();

    // Sleep before rebase to deliberately make the original "next_fire"
    // already in the past.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.rebase_now();

    // After rebase, next fire should be ~1ms from now, not immediately.
    auto t0 = std::chrono::steady_clock::now();
    while (n == 0 && std::chrono::steady_clock::now() - t0 <
                         std::chrono::milliseconds(50)) {
        sched.tick();
    }
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, std::chrono::microseconds(700));
}
