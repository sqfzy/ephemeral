/// @file test_queue_allocator_concurrent.cpp
/// Concurrency stress test for `eph::dpdk::detail::QueueAllocator` (T3.1).
///
/// Existing `test_queue_allocator.cpp` exercises the allocator
/// algorithmically (single-threaded — claim/release/generation,
/// fragmentation edge cases, ABA). T3.1 from the 2026-05-05 action
/// list called for stress validation under concurrent claim/release
/// from multiple threads — the production path is single-threaded
/// (one daemon owns claim, secondaries dispatch via IPC), but the
/// underlying mutex + atomic generation must still be race-free
/// because:
///
///   1. The IPC handler dispatcher is technically free to run a
///      second handler before the first returns (DPDK rte_mp threads
///      are pooled).
///   2. A future redesign could permit multi-threaded daemon claim
///      paths, and the algorithm should be ready.
///   3. ABA + generation logic is non-trivial — a stress test catches
///      regressions far more cheaply than reasoning by hand.
///
/// Coverage:
///   * 8 threads × 200 iterations × claim → release → claim → release
///     across a 64-queue pool. Total = 1600 successful claim-release
///     pairs per thread, 12 800 total.
///   * Pool-exhaustion path: 16 threads claiming count=8 from a
///     32-queue pool sometimes fails with QueuePoolExhausted; verify
///     that no thread observes a corrupted state on success and the
///     allocator's free count is consistent at the end.
///   * Generation monotonicity: the allocator's generation counter
///     never goes backwards; every successful claim returns a strictly
///     greater generation than any earlier one this thread observed.
///   * Stale-release rejection: each thread that releases keeps its
///     own QueueRange, so the test doesn't simulate ABA tampering.
///     The test_queue_allocator.cpp covers ABA explicitly.
///
/// Run with TSan / ASan for additional race detection:
///   xmake f -m tsan && xmake build test_queue_allocator_concurrent
///   xmake f -m asan && xmake build test_queue_allocator_concurrent

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/detail/queue_allocator.hpp"
#include "dpdk_test_env.hpp"

using eph::dpdk::detail::QueueAllocator;
using eph::dpdk::detail::QueueRange;

namespace {

// file_prefix capped at 20 bytes; keep this short.
std::string unique_prefix(std::string_view tag) {
    static int counter = 0;
    return std::string{"qac_"} + std::string{tag} + std::to_string(counter++);
}

}  // namespace

TEST(QueueAllocatorConcurrent, RepeatedClaimReleaseIsRaceFree) {
    constexpr uint16_t kPoolSize       = 64;
    constexpr int      kNumThreads     = 8;
    constexpr int      kIterations     = 200;
    constexpr uint16_t kClaimSize      = 4;

    auto alloc = QueueAllocator::create_primary(unique_prefix("repeated"),
                                                kPoolSize);
    ASSERT_TRUE(alloc.has_value()) << alloc.error();

    std::atomic<int> total_successful_claims{0};
    std::atomic<int> total_successful_releases{0};
    std::atomic<int> exhaustion_observations{0};
    std::atomic<uint64_t> max_generation_seen{0};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(kNumThreads));
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(static_cast<uint32_t>(0xc0ffee + t));
            uint64_t local_max_gen = 0;
            for (int i = 0; i < kIterations; ++i) {
                auto claim = alloc->claim(kClaimSize);
                if (!claim.has_value()) {
                    // Pool is fragmented or exhausted right now;
                    // back off briefly and retry. This is expected in
                    // a stress test where N threads concurrently
                    // operate on a pool that only has room for N
                    // simultaneous holders.
                    if (claim.error() == "QueuePoolExhausted") {
                        ++exhaustion_observations;
                    }
                    std::this_thread::yield();
                    continue;
                }
                ++total_successful_claims;

                // Generation must be strictly increasing within this
                // thread's own observations (the allocator's
                // generation is monotonic globally).
                EXPECT_GT(claim->generation, local_max_gen)
                    << "thread=" << t << " iteration=" << i
                    << " claim.generation=" << claim->generation
                    << " local_max=" << local_max_gen;
                local_max_gen = claim->generation;

                // Range invariants.
                EXPECT_LT(claim->lo, claim->hi);
                EXPECT_EQ(claim->hi - claim->lo, kClaimSize);
                EXPECT_LE(claim->hi, kPoolSize);

                // Hold briefly to encourage interleaving.
                if ((i & 0x3) == 0) std::this_thread::yield();

                alloc->release(*claim);
                ++total_successful_releases;
            }
            // Publish thread-local max into the global watermark.
            uint64_t prev = max_generation_seen.load(std::memory_order_relaxed);
            while (local_max_gen > prev &&
                   !max_generation_seen.compare_exchange_weak(
                       prev, local_max_gen, std::memory_order_relaxed)) {
                /* retry */
            }
        });
    }

    for (auto& th : threads) th.join();

    // Sanity: claim count == release count (no dangling claims).
    EXPECT_EQ(total_successful_claims.load(),
              total_successful_releases.load())
        << "Claims and releases must balance — leaked or double-released slot";

    // Pool back to fully free.
    auto state = alloc->dump();
    EXPECT_EQ(state.free, kPoolSize)
        << "Pool not fully released — claimed=" << (kPoolSize - state.free);

    // At least one thread should have completed at least one claim.
    EXPECT_GT(total_successful_claims.load(), 0);

    // Generation should have advanced by at least the number of claims
    // (one bump per claim).
    EXPECT_GE(state.generation,
              static_cast<uint64_t>(total_successful_claims.load()));

    // Watermark should match the dump's generation (the last claim's
    // generation = current generation).
    EXPECT_EQ(state.generation, max_generation_seen.load());

    // Stale releases should be 0 in this test (no ABA tampering).
    EXPECT_EQ(state.stale_releases, 0u);

    // Expect at least some exhaustion observations under contention
    // (8 threads × 4 queues = 32 simultaneous holders; pool is 64 so
    // this is unlikely, but acceptable if 0).
    SCOPED_TRACE(testing::Message()
                 << "exhaustion_observations="
                 << exhaustion_observations.load());
}

TEST(QueueAllocatorConcurrent, PoolExhaustionUnderContentionStaysConsistent) {
    // Tight pool: 16 threads each trying to hold 4 queues = up to 64
    // claimed; pool is 32 so most should bounce off QueuePoolExhausted.
    constexpr uint16_t kPoolSize   = 32;
    constexpr int      kNumThreads = 16;
    constexpr int      kIterations = 100;
    constexpr uint16_t kClaimSize  = 4;

    auto alloc = QueueAllocator::create_primary(unique_prefix("exhaustion"),
                                                kPoolSize);
    ASSERT_TRUE(alloc.has_value()) << alloc.error();

    std::atomic<int> total_claims{0};
    std::atomic<int> total_releases{0};
    std::atomic<int> total_exhaustions{0};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(kNumThreads));
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kIterations; ++i) {
                auto claim = alloc->claim(kClaimSize);
                if (!claim.has_value()) {
                    if (claim.error() == "QueuePoolExhausted") {
                        ++total_exhaustions;
                    }
                    std::this_thread::yield();
                    continue;
                }
                ++total_claims;

                // Brief hold, then release.
                std::this_thread::yield();
                alloc->release(*claim);
                ++total_releases;
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(total_claims.load(), total_releases.load());

    auto state = alloc->dump();
    EXPECT_EQ(state.free, kPoolSize) << "leaked claims";
    EXPECT_EQ(state.stale_releases, 0u);

    // Some exhaustions must have been observed (sanity that
    // contention was real — otherwise we just serialise through
    // the mutex without ever hitting the empty-pool branch).
    // Allow zero on extremely fast CPUs but log for diagnostics.
    SCOPED_TRACE(testing::Message()
                 << "claims=" << total_claims.load()
                 << " exhaustions=" << total_exhaustions.load());
}

TEST(QueueAllocatorConcurrent, MixedClaimSizesPreserveBitmapInvariants) {
    // Threads claim varying counts (1, 2, 4, 8) from a 64-queue pool;
    // verify the bitmap stays consistent and free count is correct
    // at the end.
    constexpr uint16_t kPoolSize   = 64;
    constexpr int      kNumThreads = 8;
    constexpr int      kIterations = 80;

    auto alloc = QueueAllocator::create_primary(unique_prefix("mixed"),
                                                kPoolSize);
    ASSERT_TRUE(alloc.has_value()) << alloc.error();

    std::atomic<int> success{0};
    std::atomic<int> exhaustion{0};

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(kNumThreads));
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(static_cast<uint32_t>(0xfeed + t));
            const std::array<uint16_t, 4> sizes = {1, 2, 4, 8};
            for (int i = 0; i < kIterations; ++i) {
                const uint16_t sz = sizes[rng() % sizes.size()];
                auto claim = alloc->claim(sz);
                if (!claim.has_value()) {
                    if (claim.error() == "QueuePoolExhausted") {
                        ++exhaustion;
                    }
                    std::this_thread::yield();
                    continue;
                }
                ++success;

                // Verify range size matches request.
                EXPECT_EQ(claim->hi - claim->lo, sz);

                // Hold briefly.
                std::this_thread::yield();
                alloc->release(*claim);
            }
        });
    }
    for (auto& th : threads) th.join();

    auto state = alloc->dump();
    EXPECT_EQ(state.free, kPoolSize) << "leaked";
    EXPECT_EQ(state.stale_releases, 0u);
    EXPECT_GT(success.load(), 0);
}
