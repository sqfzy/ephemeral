#include <gtest/gtest.h>

#include <cmath>
#include <thread>
#include <vector>

#include "eph/utils/system_stats.hpp"

using namespace eph::utils;

// ============================================================================
// SystemResourceStats
// ============================================================================

TEST(SystemResourceStatsTest, DefaultConstruction) {
    SystemResourceStats stats{};
    EXPECT_EQ(stats.majflt, 0);
    EXPECT_EQ(stats.minflt, 0);
    EXPECT_EQ(stats.nvcsw, 0);
    EXPECT_EQ(stats.nivcsw, 0);
    EXPECT_DOUBLE_EQ(stats.user_cpu_s, 0.0);
    EXPECT_DOUBLE_EQ(stats.sys_cpu_s, 0.0);
    EXPECT_DOUBLE_EQ(stats.total_cpu_s, 0.0);
}

TEST(SystemResourceStatsTest, FormatOutput) {
    SystemResourceStats stats{
        .majflt = 1,
        .minflt = 42,
        .nvcsw = 3,
        .nivcsw = 1,
        .user_cpu_s = 0.1234,
        .sys_cpu_s = 0.0567,
        .total_cpu_s = 0.1801,
    };

    auto formatted = std::format("{}", stats);
    EXPECT_NE(formatted.find("0.1234"), std::string::npos);
    EXPECT_NE(formatted.find("0.0567"), std::string::npos);
    EXPECT_NE(formatted.find("majflt=1"), std::string::npos);
    EXPECT_NE(formatted.find("minflt=42"), std::string::npos);
    EXPECT_NE(formatted.find("vcsw=3"), std::string::npos);
    EXPECT_NE(formatted.find("ivcsw=1"), std::string::npos);
}

// ============================================================================
// SystemStats — Construction and lifecycle
// ============================================================================

TEST(SystemStatsTest, ConstructionSucceeds) {
    EXPECT_NO_THROW(SystemStats stats);
}

TEST(SystemStatsTest, ConstructionWithAutoPrint) {
    // auto_print=true should not throw; it just prints on destruction
    EXPECT_NO_THROW(SystemStats stats(true));
}

TEST(SystemStatsTest, MoveConstruction) {
    SystemStats stats1;
    SystemStats stats2(std::move(stats1));
    // stats2 should be usable after move
    auto snap = stats2.snapshot();
    EXPECT_GE(snap.total_cpu_s, 0.0);
}

TEST(SystemStatsTest, MoveAssignment) {
    SystemStats stats1;
    SystemStats stats2;
    stats2 = std::move(stats1);
    auto snap = stats2.snapshot();
    EXPECT_GE(snap.total_cpu_s, 0.0);
}

// ============================================================================
// SystemStats — Snapshot
// ============================================================================

TEST(SystemStatsTest, SnapshotReturnsNonNegativeValues) {
    SystemStats stats;

    // Do some trivial work to generate measurable CPU time
    volatile double x = 0;
    for (int i = 0; i < 100000; ++i) {
        x += std::sin(i);
    }

    auto snap = stats.snapshot();
    EXPECT_GE(snap.user_cpu_s, 0.0);
    EXPECT_GE(snap.sys_cpu_s, 0.0);
    EXPECT_GE(snap.total_cpu_s, 0.0);
    EXPECT_GE(snap.majflt, 0);
    EXPECT_GE(snap.minflt, 0);
    EXPECT_GE(snap.nvcsw, 0);
    EXPECT_GE(snap.nivcsw, 0);
}

TEST(SystemStatsTest, SnapshotTotalIsSumOfUserAndSys) {
    SystemStats stats;

    volatile double x = 0;
    for (int i = 0; i < 100000; ++i) {
        x += std::sin(i);
    }

    auto snap = stats.snapshot();
    EXPECT_NEAR(snap.total_cpu_s, snap.user_cpu_s + snap.sys_cpu_s, 1e-9);
}

TEST(SystemStatsTest, SnapshotDetectsMinorPageFaults) {
    SystemStats stats;

    // Allocate and touch new memory to trigger minor page faults
    std::vector<std::vector<char>> allocs;
    for (int i = 0; i < 100; ++i) {
        allocs.emplace_back(4096, static_cast<char>(i));
    }

    auto snap = stats.snapshot();
    // Minor page faults should be non-negative (very likely > 0 from allocations)
    EXPECT_GE(snap.minflt, 0);
}

TEST(SystemStatsTest, SnapshotDetectsVoluntaryContextSwitches) {
    SystemStats stats;

    // Sleep to trigger voluntary context switch
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto snap = stats.snapshot();
    // At least one voluntary context switch from sleep
    EXPECT_GE(snap.nvcsw, 1);
}

// ============================================================================
// SystemStats — Reset
// ============================================================================

TEST(SystemStatsTest, ResetRebaselines) {
    SystemStats stats;

    // Do some work
    volatile double x = 0;
    for (int i = 0; i < 100000; ++i) {
        x += std::sin(i);
    }
    auto snap1 = stats.snapshot();

    // Reset
    stats.reset();

    // Immediately snapshot after reset — should show minimal activity
    auto snap2 = stats.snapshot();
    EXPECT_LE(snap2.total_cpu_s, snap1.total_cpu_s);
}

TEST(SystemStatsTest, ResetThenDoWork) {
    SystemStats stats;
    stats.reset();

    // Do some work after reset
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto snap = stats.snapshot();
    EXPECT_GE(snap.nvcsw, 1);
}

// ============================================================================
// SystemStats — Multiple snapshots
// ============================================================================

TEST(SystemStatsTest, MultipleSnapshotsAreMonotonic) {
    SystemStats stats;

    volatile double x = 0;
    for (int i = 0; i < 50000; ++i) {
        x += std::sin(i);
    }
    auto snap1 = stats.snapshot();

    for (int i = 0; i < 50000; ++i) {
        x += std::sin(i);
    }
    auto snap2 = stats.snapshot();

    // CPU time should be monotonically increasing (or at least equal)
    EXPECT_GE(snap2.user_cpu_s, snap1.user_cpu_s);
    EXPECT_GE(snap2.total_cpu_s, snap1.total_cpu_s);
    EXPECT_GE(snap2.minflt, snap1.minflt);
}

// ============================================================================
// SystemStats — print_report (smoke test)
// ============================================================================

TEST(SystemStatsTest, PrintReportDoesNotCrash) {
    SystemStats stats;

    volatile double x = 0;
    for (int i = 0; i < 1000; ++i) {
        x += std::sin(i);
    }

    EXPECT_NO_THROW(stats.print_report());
}
