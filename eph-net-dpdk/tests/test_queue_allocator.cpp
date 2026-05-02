/// @file test_queue_allocator.cpp
/// Unit tests for `eph::dpdk::detail::QueueAllocator` (S5 of the
/// daemon-led Platform reshape).
///
/// Boots EAL once per binary in `--no-pci --no-huge --vdev=net_null0`
/// mode (see `dpdk_test_env.hpp`). The allocator is a hugepage
/// memzone, but `--no-huge` causes DPDK to back memzones with
/// regular memory, so the bring-up still works on any host. The
/// IPC handler thunks are NOT exercised here — those need a peer
/// process and live under the integration / e2e tests on a real
/// DPDK rig.
///
/// Coverage:
///   * Pure logic — claim / release / generation / fragmentation
///   * No port_id (RETA refresh is gated by sentinel 0xFFFF)
///   * No daemon binary (we drive the allocator API directly)

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "eph/dpdk/detail/queue_allocator.hpp"
#include "dpdk_test_env.hpp"

using eph::dpdk::detail::QueueAllocator;
using eph::dpdk::detail::QueueRange;

namespace {

/// Helper: build a unique file_prefix per test so memzones don't
/// collide between cases (the test env doesn't tear down DPDK
/// between cases).
std::string unique_prefix(std::string_view tag) {
    static int counter = 0;
    return std::string{"qa_"} + std::string{tag} + "_" +
           std::to_string(counter++);
}

} // namespace

TEST(QueueAllocator, CreatePrimary_TotalQueuesZero_Rejected) {
    auto r = QueueAllocator::create_primary(unique_prefix("zero"), 0);
    EXPECT_FALSE(r.has_value());
}

TEST(QueueAllocator, CreatePrimary_TotalQueuesTooLarge_Rejected) {
    auto r = QueueAllocator::create_primary(unique_prefix("toolarge"), 257);
    EXPECT_FALSE(r.has_value());
}

TEST(QueueAllocator, CreatePrimary_EmptyFilePrefix_Rejected) {
    auto r = QueueAllocator::create_primary("", 8);
    EXPECT_FALSE(r.has_value());
}

TEST(QueueAllocator, CreatePrimary_Valid_Succeeds) {
    auto r = QueueAllocator::create_primary(unique_prefix("ok"), 16);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(bool(*r));
    auto state = r->dump();
    EXPECT_EQ(state.total, 16u);
    EXPECT_EQ(state.free,  16u);
    EXPECT_EQ(state.largest_free_run, 16u);
    EXPECT_EQ(state.generation, 0u);
}

TEST(QueueAllocator, Claim_FirstFitGreedy_GrantsLowestFreeRun) {
    auto r = QueueAllocator::create_primary(unique_prefix("firstfit"), 16);
    ASSERT_TRUE(r.has_value()) << r.error();

    auto a = r->claim(4);
    ASSERT_TRUE(a.has_value()) << a.error();
    EXPECT_EQ(a->lo, 0u);
    EXPECT_EQ(a->hi, 4u);
    EXPECT_GT(a->generation, 0u);

    auto b = r->claim(2);
    ASSERT_TRUE(b.has_value()) << b.error();
    EXPECT_EQ(b->lo, 4u);
    EXPECT_EQ(b->hi, 6u);

    auto state = r->dump();
    EXPECT_EQ(state.free, 10u);
    EXPECT_EQ(state.largest_free_run, 10u);
}

TEST(QueueAllocator, Claim_PoolExhausted_ReturnsCanonicalError) {
    auto r = QueueAllocator::create_primary(unique_prefix("exhaust"), 4);
    ASSERT_TRUE(r.has_value()) << r.error();

    auto a = r->claim(4);
    ASSERT_TRUE(a.has_value()) << a.error();

    auto b = r->claim(1);
    ASSERT_FALSE(b.has_value());
    EXPECT_EQ(b.error(), "QueuePoolExhausted");
}

TEST(QueueAllocator, Claim_LargerThanPool_ReturnsCanonicalError) {
    auto r = QueueAllocator::create_primary(unique_prefix("toobig"), 4);
    ASSERT_TRUE(r.has_value()) << r.error();

    auto a = r->claim(8);
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error(), "QueuePoolExhausted");
}

TEST(QueueAllocator, Claim_ZeroCount_Rejected) {
    auto r = QueueAllocator::create_primary(unique_prefix("zerocount"), 4);
    ASSERT_TRUE(r.has_value()) << r.error();

    auto a = r->claim(0);
    EXPECT_FALSE(a.has_value());
}

TEST(QueueAllocator, Release_FreesQueuesAndAllowsReclaim) {
    auto r = QueueAllocator::create_primary(unique_prefix("freerec"), 8);
    ASSERT_TRUE(r.has_value()) << r.error();

    auto a = r->claim(4);
    ASSERT_TRUE(a.has_value()) << a.error();
    EXPECT_EQ(r->dump().free, 4u);

    r->release(*a);
    EXPECT_EQ(r->dump().free, 8u);

    auto b = r->claim(8);
    ASSERT_TRUE(b.has_value()) << b.error();
    EXPECT_EQ(b->lo, 0u);
    EXPECT_EQ(b->hi, 8u);
    EXPECT_GT(b->generation, a->generation);
}

TEST(QueueAllocator, Release_GenerationABA_StaleReleaseRejected) {
    // Scenario: process A claims [0,4), process A dies WITHOUT calling
    // release. Process B then claims [0,4) (gets a new generation).
    // Process A's late release msg arrives; we must NOT clear B's bits.
    //
    // We can't simulate two processes in one binary, but we can drive
    // the API directly: claim → release → reclaim → late-release of
    // the FIRST capture.
    auto r = QueueAllocator::create_primary(unique_prefix("aba"), 8);
    ASSERT_TRUE(r.has_value()) << r.error();

    auto a1 = r->claim(4);
    ASSERT_TRUE(a1.has_value()) << a1.error();
    const QueueRange first_capture = *a1;

    r->release(*a1);                 // legit release
    auto a2 = r->claim(4);            // re-claim same queues
    ASSERT_TRUE(a2.has_value()) << a2.error();
    EXPECT_EQ(a2->lo, 0u);
    EXPECT_EQ(a2->hi, 4u);
    EXPECT_NE(a2->generation, first_capture.generation);

    // Stale release with the FIRST capture — must NOT free a2's bits.
    // The per-queue `claim_gen[]` was bumped when a2 minted a new
    // generation, so first_capture.generation no longer matches the
    // claim_gen of any of [0,4) and the release bounces.
    r->release(first_capture);
    auto state = r->dump();
    EXPECT_EQ(state.free, 4u) << "stale release wrongly freed live claim";
    EXPECT_GT(state.stale_releases, 0u)
        << "expected stale_releases > 0 — the release was rejected via "
           "the per-queue claim_gen mismatch";

    // Sanity: a2 still works.
    r->release(*a2);
    EXPECT_EQ(r->dump().free, 8u);
}

TEST(QueueAllocator, Release_GenerationFromFuture_Rejected) {
    auto r = QueueAllocator::create_primary(unique_prefix("future"), 8);
    ASSERT_TRUE(r.has_value()) << r.error();

    auto a = r->claim(2);
    ASSERT_TRUE(a.has_value()) << a.error();

    QueueRange tampered = *a;
    tampered.generation = a->generation + 999;   // forge from the future
    r->release(tampered);
    EXPECT_GT(r->dump().stale_releases, 0u);
    // Still 6 free; the live claim wasn't released.
    EXPECT_EQ(r->dump().free, 6u);
}

TEST(QueueAllocator, Release_OutOfRange_Rejected) {
    auto r = QueueAllocator::create_primary(unique_prefix("oob"), 4);
    ASSERT_TRUE(r.has_value()) << r.error();

    QueueRange bogus{0, 10, 1};   // hi past total_queues
    r->release(bogus);
    EXPECT_GT(r->dump().stale_releases, 0u);
}

TEST(QueueAllocator, Release_EmptyRange_NoOp) {
    auto r = QueueAllocator::create_primary(unique_prefix("empty"), 4);
    ASSERT_TRUE(r.has_value()) << r.error();

    QueueRange empty_range{0, 0, 0};
    r->release(empty_range);   // silent no-op
    EXPECT_EQ(r->dump().stale_releases, 0u);
}

TEST(QueueAllocator, Bitmap_ReflectsClaimedSet) {
    auto r = QueueAllocator::create_primary(unique_prefix("bitmap"), 8);
    ASSERT_TRUE(r.has_value()) << r.error();

    auto a = r->claim(3);
    ASSERT_TRUE(a.has_value()) << a.error();
    EXPECT_EQ(r->owned_queues_bitmap(), 0b00000111u);

    auto b = r->claim(2);
    ASSERT_TRUE(b.has_value()) << b.error();
    EXPECT_EQ(r->owned_queues_bitmap(), 0b00011111u);

    r->release(*a);
    EXPECT_EQ(r->owned_queues_bitmap(), 0b00011000u);
}

TEST(QueueAllocator, Move_TransfersOwnership) {
    auto r = QueueAllocator::create_primary(unique_prefix("move"), 8);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_TRUE(bool(*r));

    QueueAllocator moved = std::move(*r);
    EXPECT_TRUE(bool(moved));
    EXPECT_FALSE(bool(*r));   // moved-from is empty

    auto a = moved.claim(2);
    ASSERT_TRUE(a.has_value()) << a.error();
}
