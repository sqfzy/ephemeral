/// @file test_mp_topology.cpp
/// Pure-logic unit tests for `eph::dpdk::MpTopology` and `ProcSpec`.
///
/// These tests deliberately do NOT include `dpdk_test_env.hpp` and do
/// NOT initialize EAL — the topology is a value type that compiles
/// against `<cstdint>` / `<vector>` / `<string_view>` only. Running them
/// safely coexists with another DPDK process holding hugepages / vfio
/// on the host, since this binary never calls `rte_eal_init`.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "eph/dpdk/mp_topology.hpp"

using eph::dpdk::MpTopology;
using eph::dpdk::ProcSpec;

// ─────────────────────────────────────────────────────────────────────────────
// MpTopology::uniform — the 90% path
// ─────────────────────────────────────────────────────────────────────────────

TEST(MpTopologyUniform, EvenSplit2Procs4Queues) {
    auto t = MpTopology::uniform(/*self_index=*/0, /*total_procs=*/2,
                                 /*nb_rx_queues=*/4);
    ASSERT_TRUE(t.valid()) << t.dump();
    ASSERT_EQ(t.total_procs, 2);

    EXPECT_EQ(t.procs[0].queue_lo, 0);
    EXPECT_EQ(t.procs[0].queue_hi, 2);
    EXPECT_EQ(t.procs[1].queue_lo, 2);
    EXPECT_EQ(t.procs[1].queue_hi, 4);

    // procs_view() returns just the populated slots.
    EXPECT_EQ(t.procs_view().size(), 2u);

    // Default port window 32768..65536, split evenly = 16384 each.
    EXPECT_EQ(t.procs[0].port_lo, 32768);
    EXPECT_EQ(t.procs[0].port_hi, 49152);
    EXPECT_EQ(t.procs[1].port_lo, 49152);
    EXPECT_EQ(t.procs[1].port_hi, 65536);

    EXPECT_EQ(t.self_index, 0);
    EXPECT_EQ(&t.self(), &t.procs[0]);
}

TEST(MpTopologyUniform, NonIntegerSplit3Procs8Queues_RemainderToLast) {
    // 8 / 3 = 2 remainder 2. By convention the last proc absorbs the
    // remainder: procs[0..1] get 2 queues each, procs[2] gets 4.
    auto t = MpTopology::uniform(/*self_index=*/2, /*total_procs=*/3,
                                 /*nb_rx_queues=*/8);
    ASSERT_TRUE(t.valid()) << t.dump();
    ASSERT_EQ(t.total_procs, 3);

    EXPECT_EQ(t.procs[0].queue_lo, 0);
    EXPECT_EQ(t.procs[0].queue_hi, 2);
    EXPECT_EQ(t.procs[1].queue_lo, 2);
    EXPECT_EQ(t.procs[1].queue_hi, 4);
    EXPECT_EQ(t.procs[2].queue_lo, 4);
    EXPECT_EQ(t.procs[2].queue_hi, 8);    // last proc absorbs remainder

    // Same rule for ports: 32768 / 3 = 10922 remainder 2.
    EXPECT_EQ(t.procs[0].port_lo, 32768);
    EXPECT_EQ(t.procs[0].port_hi, 32768 + 10922);
    EXPECT_EQ(t.procs[1].port_lo, 32768 + 10922);
    EXPECT_EQ(t.procs[1].port_hi, 32768 + 21844);
    EXPECT_EQ(t.procs[2].port_lo, 32768 + 21844);
    EXPECT_EQ(t.procs[2].port_hi, 65536);  // last proc absorbs remainder
}

TEST(MpTopologyUniform, SingleProcDegenerate) {
    auto t = MpTopology::uniform(/*self_index=*/0, /*total_procs=*/1,
                                 /*nb_rx_queues=*/4);
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.total_procs, 1);
    EXPECT_EQ(t.procs[0].queue_lo, 0);
    EXPECT_EQ(t.procs[0].queue_hi, 4);
    EXPECT_EQ(t.procs[0].port_lo, 32768);
    EXPECT_EQ(t.procs[0].port_hi, 65536);
}

TEST(MpTopologyUniform, CustomPortWindow) {
    auto t = MpTopology::uniform(/*self_index=*/1, /*total_procs=*/2,
                                 /*nb_rx_queues=*/2,
                                 /*port_base=*/40000,
                                 /*port_total=*/2000);
    ASSERT_TRUE(t.valid());
    EXPECT_EQ(t.procs[0].port_lo, 40000);
    EXPECT_EQ(t.procs[0].port_hi, 41000);
    EXPECT_EQ(t.procs[1].port_lo, 41000);
    EXPECT_EQ(t.procs[1].port_hi, 42000);
}

// ─── uniform() defensive guards: invalid input → invalid topology ────────────

TEST(MpTopologyUniform, ZeroTotalProcs_ReturnsInvalid) {
    auto t = MpTopology::uniform(0, 0, 4);
    EXPECT_EQ(t.total_procs, 0);
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyUniform, OverMaxTotalProcs_ReturnsInvalid) {
    auto t = MpTopology::uniform(0, /*total_procs=*/65, /*nb_rx_queues=*/65);
    EXPECT_EQ(t.total_procs, 0);
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyUniform, FewerQueuesThanProcs_ReturnsInvalid) {
    auto t = MpTopology::uniform(0, /*total_procs=*/4, /*nb_rx_queues=*/2);
    EXPECT_EQ(t.total_procs, 0);
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyUniform, SelfIndexOOB_ReturnsInvalid) {
    auto t = MpTopology::uniform(/*self_index=*/3, /*total_procs=*/2, 4);
    EXPECT_EQ(t.total_procs, 0);
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyUniform, PortWindowOverflow_ReturnsInvalid) {
    // port_base + port_total > 65536 — would silently wrap on uint16_t
    // arithmetic, so the factory rejects up-front.
    auto t = MpTopology::uniform(0, 2, 4,
                                 /*port_base=*/60000,
                                 /*port_total=*/10000);
    EXPECT_EQ(t.total_procs, 0);
    EXPECT_FALSE(t.valid());
}

// ─────────────────────────────────────────────────────────────────────────────
// MpTopology::valid — overlap and edge detection
// ─────────────────────────────────────────────────────────────────────────────

TEST(MpTopologyValid, CustomNonOverlapping_Accepted) {
    auto t = MpTopology::custom(/*self_index=*/1, {
        ProcSpec{.tag="trader",  .queue_lo=0, .queue_hi=6, .port_lo=32768, .port_hi=50000},
        ProcSpec{.tag="monitor", .queue_lo=6, .queue_hi=7, .port_lo=50000, .port_hi=55000},
        ProcSpec{.tag="auditor", .queue_lo=7, .queue_hi=8, .port_lo=55000, .port_hi=60000},
    });
    ASSERT_EQ(t.total_procs, 3);
    EXPECT_TRUE(t.valid()) << t.dump();
    EXPECT_EQ(t.self().tag, "monitor");
}

TEST(MpTopologyValid, EmptyProcs_Rejected) {
    MpTopology t{};
    EXPECT_EQ(t.total_procs, 0);
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, OverMaxProcs_RejectedByValid) {
    // Directly poke total_procs to simulate a buggy caller bypassing
    // the factory: valid() must catch out-of-range total_procs.
    MpTopology t;
    t.total_procs = MpTopology::kMaxProcs + 1;  // beyond array capacity
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, SelfIndexOOB_Rejected) {
    auto t = MpTopology::custom(/*self_index=*/5, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=40000},
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=40000, .port_hi=50000},
    });
    // custom() faithfully sets self_index=5, total_procs=2 → valid()
    // catches the OOB.
    EXPECT_EQ(t.total_procs, 2);
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, EmptyQueueRange_Rejected) {
    auto t = MpTopology::custom(0, {
        // queue_lo == queue_hi → empty range, rejected
        ProcSpec{.queue_lo=2, .queue_hi=2, .port_lo=32768, .port_hi=40000},
    });
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, InvertedPortRange_Rejected) {
    auto t = MpTopology::custom(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=40000, .port_hi=32768},
    });
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, OverlappingQueues_Rejected) {
    auto t = MpTopology::custom(0, {
        ProcSpec{.queue_lo=0, .queue_hi=4, .port_lo=32768, .port_hi=40000},
        // [3,5) overlaps [0,4) at queue id 3
        ProcSpec{.queue_lo=3, .queue_hi=5, .port_lo=40000, .port_hi=50000},
    });
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, OverlappingPorts_Rejected) {
    auto t = MpTopology::custom(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=40000},
        // disjoint queues but ports overlap at 35000..40000
        ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=35000, .port_hi=50000},
    });
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, AdjacentRangesNotOverlap_Accepted) {
    // [0,4) and [4,8) share boundary 4 but don't overlap (half-open).
    auto t = MpTopology::custom(0, {
        ProcSpec{.queue_lo=0, .queue_hi=4, .port_lo=32768, .port_hi=49152},
        ProcSpec{.queue_lo=4, .queue_hi=8, .port_lo=49152, .port_hi=65536},
    });
    EXPECT_TRUE(t.valid());
}

TEST(MpTopologyValid, PortHiAtBoundary_Accepted) {
    // port_hi=65536 is the documented inclusive upper bound (the half-
    // open `port_hi` may equal 65536 to express the full ephemeral
    // window without uint16_t wrap).
    auto t = MpTopology::custom(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=65536},
    });
    EXPECT_TRUE(t.valid());
}

TEST(MpTopologyValid, PortHiOverflow_Rejected) {
    // ProcSpec stores port_hi as uint32_t, so a `custom()` caller can
    // technically set port_hi=200000 and silently corrupt downstream
    // src_port allocation when it casts to uint16_t. valid() must
    // reject this — see mp_topology.hpp doc claim "constrained to
    // [0, 65536] by valid()".
    auto t = MpTopology::custom(0, {
        ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=200000u},
    });
    EXPECT_FALSE(t.valid());
}

// ─────────────────────────────────────────────────────────────────────────────
// Equality & dump
// ─────────────────────────────────────────────────────────────────────────────

TEST(MpTopologyEquality, FieldwiseStrict) {
    auto a = MpTopology::uniform(0, 2, 4);
    auto b = MpTopology::uniform(0, 2, 4);
    EXPECT_EQ(a, b);

    // self_index difference matters even though procs are identical.
    auto c = MpTopology::uniform(1, 2, 4);
    EXPECT_NE(a, c);

    // procs count difference matters.
    auto d = MpTopology::uniform(0, 1, 4);
    EXPECT_NE(a, d);
}

TEST(MpTopologyDump, ContainsTagAndStarMarksSelf) {
    auto t = MpTopology::custom(/*self_index=*/1, {
        ProcSpec{.tag="trader",  .queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=40000},
        ProcSpec{.tag="monitor", .queue_lo=2, .queue_hi=4, .port_lo=40000, .port_hi=50000},
    });
    const auto s = t.dump();
    // Sanity: dump mentions both tags + the star prefix marks self_index=1.
    EXPECT_NE(s.find("trader"),  std::string::npos);
    EXPECT_NE(s.find("monitor"), std::string::npos);
    EXPECT_NE(s.find("[1]*"),    std::string::npos);
    EXPECT_NE(s.find("[0] "),    std::string::npos);   // not self → space
}

// ─────────────────────────────────────────────────────────────────────────────
// lcore_mask invariants — pure topology level
//
// MpTopology::valid()'s lcore-overlap detection (mp_topology.hpp lines
// 177-191) was added in commit `fceb0f78` for the v2 cross-process
// conflict gate but went unrepresented in this file's test suite —
// coverage was implicit via test_mp_registry only. These tests pin
// the topology-level invariants directly so a future refactor can't
// accidentally change `valid()` without flipping a unit test here.
//
// Discovered via /pax --review LENS round 4 (escalated to L1: drop
// LENS filter, full coverage scan) on 2026-05-01.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Helper: build a 2-proc topology with explicit lcore_mask values.
// Mirrors test_mp_registry's `make_topo_lcored` but local to this file
// so test_mp_topology stays free of test_mp_registry dependencies.
inline MpTopology make_lcored_topo(uint8_t self_index,
                                   uint64_t self_mask,
                                   uint64_t peer_mask) {
    auto t = MpTopology::uniform(self_index, /*total_procs=*/2,
                                 /*nb_rx_queues=*/4);
    t.procs[0].lcore_mask = (self_index == 0) ? self_mask : peer_mask;
    t.procs[1].lcore_mask = (self_index == 1) ? self_mask : peer_mask;
    return t;
}
} // namespace

TEST(MpTopologyLcoreValid, AllOptedOut_ValidNoCheck) {
    // Default uniform() leaves all lcore_masks=0 — back-compat opt-out.
    // valid() must not run the lcore overlap check at all and must
    // accept the topology.
    auto t = MpTopology::uniform(0, 2, 4);
    EXPECT_TRUE(t.valid());
}

TEST(MpTopologyLcoreValid, DisjointMasks_Valid) {
    // proc 0 = bits 0,1; proc 1 = bits 2,3. No overlap.
    auto t = make_lcored_topo(0, /*self*/0x3, /*peer*/0xC);
    EXPECT_TRUE(t.valid());
}

TEST(MpTopologyLcoreValid, FullyOverlappingMasks_Rejected) {
    // Both procs claim the same bits — exactly the silent-CPU-theft
    // misuse mode v2 was introduced to catch.
    auto t = make_lcored_topo(0, /*self*/0x3, /*peer*/0x3);
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyLcoreValid, PartialOverlap_Rejected) {
    // proc 0 = bits 0,1,2 (0x7); proc 1 = bits 2,3 (0xC). Bit 2 collides.
    auto t = make_lcored_topo(0, /*self*/0x7, /*peer*/0xC);
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyLcoreValid, AsymmetricOptOut_Valid) {
    // proc 0 declares 0x3, proc 1 leaves mask=0 (opt-out). The
    // lcore-tracking section of valid() is "opt-in" — it triggers
    // when ANY proc has a non-zero mask, but proc-1's zero mask is
    // safe (zero & anything == 0, no overlap).
    auto t = make_lcored_topo(0, /*self*/0x3, /*peer*/0);
    EXPECT_TRUE(t.valid());
}

TEST(MpTopologyLcoreValid, HighBitMasks_Valid) {
    // Bits at the top of the uint64_t — exercise the schema cap.
    // bit 63 = 0x8000000000000000.
    auto t = make_lcored_topo(0,
                              /*self*/uint64_t{1} << 63,
                              /*peer*/uint64_t{1} << 62);
    EXPECT_TRUE(t.valid());
}

TEST(MpTopologyLcoreValid, ThreeProcs_TwoOverlap_Rejected) {
    // Custom 3-proc topology: proc 0 and proc 2 both declare bit 5 —
    // valid() must catch this even though procs 0,1 and 1,2 are clean.
    auto t = MpTopology::custom(/*self_index=*/1, {
        ProcSpec{.tag="A", .queue_lo=0, .queue_hi=1, .port_lo=32768, .port_hi=40000,
                 .lcore_mask=uint64_t{1} << 5},
        ProcSpec{.tag="B", .queue_lo=1, .queue_hi=2, .port_lo=40000, .port_hi=50000,
                 .lcore_mask=uint64_t{1} << 6},
        ProcSpec{.tag="C", .queue_lo=2, .queue_hi=3, .port_lo=50000, .port_hi=60000,
                 .lcore_mask=uint64_t{1} << 5},  // collides with A
    });
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyLcoreValid, ThreeProcs_AllDisjoint_Valid) {
    auto t = MpTopology::custom(/*self_index=*/0, {
        ProcSpec{.tag="A", .queue_lo=0, .queue_hi=1, .port_lo=32768, .port_hi=40000,
                 .lcore_mask=uint64_t{1} << 0},
        ProcSpec{.tag="B", .queue_lo=1, .queue_hi=2, .port_lo=40000, .port_hi=50000,
                 .lcore_mask=uint64_t{1} << 1},
        ProcSpec{.tag="C", .queue_lo=2, .queue_hi=3, .port_lo=50000, .port_hi=60000,
                 .lcore_mask=uint64_t{1} << 2},
    });
    EXPECT_TRUE(t.valid());
}
