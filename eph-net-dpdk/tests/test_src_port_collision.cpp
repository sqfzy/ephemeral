/// @file test_src_port_collision.cpp
/// Cross-tenant src_port collision negative tests (T3.2).
///
/// Companion to T2.4 (closed via `commit 613fa93d` — doc-only audit
/// found the architectural enforcement was already in place via
/// `MpTopology::valid()` pairwise overlap check). T3.2 adds explicit
/// boundary-case coverage so future refactors of the overlap algorithm
/// must continue to honour the contract.
///
/// Existing `test_mp_topology.cpp` has one OverlappingPorts test
/// (partial overlap of [32768,40000) vs [35000,50000)). This file
/// adds the systematic boundary cases the action list called for:
///
///   1. Non-overlapping (gap between ranges) — accepted
///   2. Full containment ([10,90) contains [40,60)) — rejected
///   3. Partial overlap (off-by-one at lo edge) — rejected
///   4. Partial overlap (off-by-one at hi edge) — rejected
///   5. Adjacent / touching half-open intervals (edge meet) — accepted
///   6. Identical ranges — rejected
///   7. Single-port range collision — rejected
///   8. Three-way pairwise overlap — rejected (a vs b OK, b vs c OK,
///      a vs c overlap)
///
/// Half-open interval semantics: `[a, b)` and `[c, d)` overlap iff
/// `a < d && c < b`. Adjacent ranges where `b == c` do NOT overlap.
/// This file pins that semantics — a future refactor that switches
/// to closed intervals would need to update every test here.

#include <gtest/gtest.h>

#include "eph/dpdk/mp_topology.hpp"

using eph::dpdk::MpTopology;
using eph::dpdk::ProcSpec;

TEST(SrcPortCollision, NonOverlappingWithGap_Accepted) {
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=40000},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=45000, .port_hi=50000},
    });
    EXPECT_TRUE(t.valid()) << t.dump();
}

TEST(SrcPortCollision, FullContainment_Rejected) {
    // [10000, 60000) fully contains [20000, 50000) — overlap.
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=10000, .port_hi=60000},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=20000, .port_hi=50000},
    });
    EXPECT_FALSE(t.valid());
}

TEST(SrcPortCollision, PartialOverlapAtLoEdge_Rejected) {
    // [30000, 40000) and [39999, 50000) — overlap at 39999.
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=30000, .port_hi=40000},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=39999, .port_hi=50000},
    });
    EXPECT_FALSE(t.valid());
}

TEST(SrcPortCollision, PartialOverlapAtHiEdge_Rejected) {
    // [40001, 50000) overlaps [30000, 40002) at 40001.
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=30000, .port_hi=40002},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=40001, .port_hi=50000},
    });
    EXPECT_FALSE(t.valid());
}

TEST(SrcPortCollision, AdjacentTouchingIntervals_Accepted) {
    // Half-open: [30000, 40000) and [40000, 50000) DO NOT overlap
    // (40000 is exclusive on the left, inclusive on the right is its
    // first byte). This is intentional — operators commonly partition
    // the ephemeral window without leaving gaps.
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=30000, .port_hi=40000},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=40000, .port_hi=50000},
    });
    EXPECT_TRUE(t.valid()) << t.dump();
}

TEST(SrcPortCollision, IdenticalRanges_Rejected) {
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=40000},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=32768, .port_hi=40000},
    });
    EXPECT_FALSE(t.valid());
}

TEST(SrcPortCollision, SinglePortCollision_Rejected) {
    // Both [32768, 32769) and [32768, 32769) — single-port disjoint
    // tenants; collision detected.
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=32769},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=32768, .port_hi=32769},
    });
    EXPECT_FALSE(t.valid());
}

TEST(SrcPortCollision, ThreeWayPairwiseOverlap_Rejected) {
    // a [30000, 40000), b [40000, 50000) — adjacent OK pairwise
    // c [35000, 45000) — overlaps both. Pairwise overlap check must
    // catch a vs c AND b vs c.
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=30000, .port_hi=40000},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=40000, .port_hi=50000},
        ProcSpec{.queue_lo=4, .queue_hi=6, .port_lo=35000, .port_hi=45000},
    });
    EXPECT_FALSE(t.valid());
}

TEST(SrcPortCollision, MaxBoundary65536_Accepted) {
    // port_hi == 65536 (exclusive) is the documented max — covers the
    // full ephemeral window without uint16_t wrap.
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=49152},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=49152, .port_hi=65536},
    });
    EXPECT_TRUE(t.valid()) << t.dump();
}

TEST(SrcPortCollision, BeyondMaxBoundary_Rejected) {
    // port_hi > 65536 must be rejected; ProcSpec.port_hi is uint32_t but
    // src_port allocation casts to uint16_t so anything >65536 silently
    // wraps. valid() catches this.
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=60000, .port_hi=70000},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=70000, .port_hi=80000},
    });
    EXPECT_FALSE(t.valid());
}

TEST(SrcPortCollision, EmptyRangeLoEqualsHi_Rejected) {
    // port_lo == port_hi → empty range, rejected even without any peer.
    auto t = MpTopology::from_specs(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=40000, .port_hi=40000},
    });
    EXPECT_FALSE(t.valid());
}

TEST(SrcPortCollision, ManySlotsAllDisjoint_Accepted) {
    // 8 slots covering the ephemeral window evenly, all disjoint by
    // construction. Sanity check the O(N²) overlap pass under load.
    constexpr uint8_t kN = 8;
    std::array<ProcSpec, kN> specs;
    constexpr uint32_t kStart = 32768;
    constexpr uint32_t kStep  = (65536u - kStart) / kN;
    for (uint8_t i = 0; i < kN; ++i) {
        specs[i] = ProcSpec{.queue_lo=static_cast<uint16_t>(i*2),
                            .queue_hi=static_cast<uint16_t>(i*2+2),
                            .port_lo=kStart + i * kStep,
                            .port_hi=kStart + (i + 1) * kStep};
    }
    // from_specs takes initializer_list — build via the public ctor path.
    MpTopology t;
    t.self_index = 0;
    t.total_procs = kN;
    for (uint8_t i = 0; i < kN; ++i) t.procs[i] = specs[i];
    EXPECT_TRUE(t.valid()) << t.dump();
}

TEST(SrcPortCollision, ManySlotsOneOverlap_Rejected) {
    // Same as above but slot[5] is bumped down so it overlaps slot[4].
    constexpr uint8_t kN = 8;
    std::array<ProcSpec, kN> specs;
    constexpr uint32_t kStart = 32768;
    constexpr uint32_t kStep  = (65536u - kStart) / kN;
    for (uint8_t i = 0; i < kN; ++i) {
        specs[i] = ProcSpec{.queue_lo=static_cast<uint16_t>(i*2),
                            .queue_hi=static_cast<uint16_t>(i*2+2),
                            .port_lo=kStart + i * kStep,
                            .port_hi=kStart + (i + 1) * kStep};
    }
    // Make slot 5 overlap into slot 4's range.
    specs[5].port_lo -= 100;
    // from_specs takes initializer_list — build via the public ctor path.
    MpTopology t;
    t.self_index = 0;
    t.total_procs = kN;
    for (uint8_t i = 0; i < kN; ++i) t.procs[i] = specs[i];
    EXPECT_FALSE(t.valid());
}
