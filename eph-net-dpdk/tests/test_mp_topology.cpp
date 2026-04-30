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
#include <vector>

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
    ASSERT_EQ(t.procs.size(), 2u);

    EXPECT_EQ(t.procs[0].queue_lo, 0);
    EXPECT_EQ(t.procs[0].queue_hi, 2);
    EXPECT_EQ(t.procs[1].queue_lo, 2);
    EXPECT_EQ(t.procs[1].queue_hi, 4);

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
    ASSERT_EQ(t.procs.size(), 3u);

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
    ASSERT_EQ(t.procs.size(), 1u);
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
    EXPECT_TRUE(t.procs.empty());
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyUniform, OverMaxTotalProcs_ReturnsInvalid) {
    auto t = MpTopology::uniform(0, /*total_procs=*/65, /*nb_rx_queues=*/65);
    EXPECT_TRUE(t.procs.empty());
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyUniform, FewerQueuesThanProcs_ReturnsInvalid) {
    auto t = MpTopology::uniform(0, /*total_procs=*/4, /*nb_rx_queues=*/2);
    EXPECT_TRUE(t.procs.empty());
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyUniform, SelfIndexOOB_ReturnsInvalid) {
    auto t = MpTopology::uniform(/*self_index=*/3, /*total_procs=*/2, 4);
    EXPECT_TRUE(t.procs.empty());
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyUniform, PortWindowOverflow_ReturnsInvalid) {
    // port_base + port_total > 65536 — would silently wrap on uint16_t
    // arithmetic, so the factory rejects up-front.
    auto t = MpTopology::uniform(0, 2, 4,
                                 /*port_base=*/60000,
                                 /*port_total=*/10000);
    EXPECT_TRUE(t.procs.empty());
    EXPECT_FALSE(t.valid());
}

// ─────────────────────────────────────────────────────────────────────────────
// MpTopology::valid — overlap and edge detection
// ─────────────────────────────────────────────────────────────────────────────

TEST(MpTopologyValid, CustomNonOverlapping_Accepted) {
    MpTopology t{
        .self_index = 1,
        .procs = {
            ProcSpec{.tag="trader",  .queue_lo=0, .queue_hi=6, .port_lo=32768, .port_hi=50000},
            ProcSpec{.tag="monitor", .queue_lo=6, .queue_hi=7, .port_lo=50000, .port_hi=55000},
            ProcSpec{.tag="auditor", .queue_lo=7, .queue_hi=8, .port_lo=55000, .port_hi=60000},
        },
    };
    EXPECT_TRUE(t.valid()) << t.dump();
    EXPECT_EQ(t.self().tag, "monitor");
}

TEST(MpTopologyValid, EmptyProcs_Rejected) {
    MpTopology t{};
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, TooManyProcs_Rejected) {
    MpTopology t;
    t.procs.resize(MpTopology::kMaxProcs + 1);
    for (size_t i = 0; i < t.procs.size(); ++i) {
        t.procs[i] = ProcSpec{
            .tag      = {},
            .queue_lo = static_cast<uint16_t>(i),
            .queue_hi = static_cast<uint16_t>(i + 1),
            .port_lo  = static_cast<uint16_t>(32768 + i),
            .port_hi  = static_cast<uint16_t>(32768 + i + 1),
        };
    }
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, SelfIndexOOB_Rejected) {
    MpTopology t{
        .self_index = 5,
        .procs = {
            ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=40000},
            ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=40000, .port_hi=50000},
        },
    };
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, EmptyQueueRange_Rejected) {
    MpTopology t{
        .self_index = 0,
        .procs = {
            // queue_lo == queue_hi → empty range, rejected
            ProcSpec{.queue_lo=2, .queue_hi=2, .port_lo=32768, .port_hi=40000},
        },
    };
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, InvertedPortRange_Rejected) {
    MpTopology t{
        .self_index = 0,
        .procs = {
            ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=40000, .port_hi=32768},
        },
    };
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, OverlappingQueues_Rejected) {
    MpTopology t{
        .self_index = 0,
        .procs = {
            ProcSpec{.queue_lo=0, .queue_hi=4, .port_lo=32768, .port_hi=40000},
            // [3,5) overlaps [0,4) at queue id 3
            ProcSpec{.queue_lo=3, .queue_hi=5, .port_lo=40000, .port_hi=50000},
        },
    };
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, OverlappingPorts_Rejected) {
    MpTopology t{
        .self_index = 0,
        .procs = {
            ProcSpec{.queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=40000},
            // disjoint queues but ports overlap at 35000..40000
            ProcSpec{.queue_lo=2, .queue_hi=4, .port_lo=35000, .port_hi=50000},
        },
    };
    EXPECT_FALSE(t.valid());
}

TEST(MpTopologyValid, AdjacentRangesNotOverlap_Accepted) {
    // [0,4) and [4,8) share boundary 4 but don't overlap (half-open).
    MpTopology t{
        .self_index = 0,
        .procs = {
            ProcSpec{.queue_lo=0, .queue_hi=4, .port_lo=32768, .port_hi=49152},
            ProcSpec{.queue_lo=4, .queue_hi=8, .port_lo=49152, .port_hi=65536},
        },
    };
    EXPECT_TRUE(t.valid());
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
    MpTopology t{
        .self_index = 1,
        .procs = {
            ProcSpec{.tag="trader",  .queue_lo=0, .queue_hi=2, .port_lo=32768, .port_hi=40000},
            ProcSpec{.tag="monitor", .queue_lo=2, .queue_hi=4, .port_lo=40000, .port_hi=50000},
        },
    };
    const auto s = t.dump();
    // Sanity: dump mentions both tags + the star prefix marks self_index=1.
    EXPECT_NE(s.find("trader"),  std::string::npos);
    EXPECT_NE(s.find("monitor"), std::string::npos);
    EXPECT_NE(s.find("[1]*"),    std::string::npos);
    EXPECT_NE(s.find("[0] "),    std::string::npos);   // not self → space
}
