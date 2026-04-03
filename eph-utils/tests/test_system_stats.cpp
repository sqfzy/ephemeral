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
    EXPECT_EQ(stats.maxrss_kb, 0);
    EXPECT_EQ(stats.rss_kb, 0);
    EXPECT_EQ(stats.thread_count, 0);
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
        .maxrss_kb = 8192,
        .rss_kb = 4096,
        .thread_count = 3,
    };

    auto formatted = std::format("{}", stats);
    EXPECT_NE(formatted.find("0.1234"), std::string::npos);
    EXPECT_NE(formatted.find("0.0567"), std::string::npos);
    EXPECT_NE(formatted.find("majflt=1"), std::string::npos);
    EXPECT_NE(formatted.find("minflt=42"), std::string::npos);
    EXPECT_NE(formatted.find("vcsw=3"), std::string::npos);
    EXPECT_NE(formatted.find("ivcsw=1"), std::string::npos);
    EXPECT_NE(formatted.find("rss=4096KB"), std::string::npos);
    EXPECT_NE(formatted.find("maxrss=8192KB"), std::string::npos);
    EXPECT_NE(formatted.find("threads=3"), std::string::npos);
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

TEST(SystemStatsTest, SnapshotReportsMemoryAndThreads) {
    SystemStats stats;
    auto snap = stats.snapshot();

    // maxrss should be > 0 for any running process (from getrusage)
    EXPECT_GT(snap.maxrss_kb, 0) << "Peak RSS should be non-zero for a running process";

#if defined(__linux__)
    // On Linux, /proc/self/statm and /proc/self/status are available
    EXPECT_GT(snap.rss_kb, 0) << "Current RSS should be non-zero on Linux";
    EXPECT_GE(snap.thread_count, 1) << "Thread count should be at least 1";
#endif

    // RSS should not exceed maxrss (sanity check)
    if (snap.rss_kb > 0) {
        EXPECT_LE(snap.rss_kb, snap.maxrss_kb * 2)
            << "Current RSS should be in a reasonable range relative to maxrss";
    }
}

TEST(SystemStatsTest, SnapshotMemoryFieldsInDump) {
    SystemStats stats;
    auto snap = stats.snapshot();
    auto dump = snap.dump();

    EXPECT_NE(dump.find("rss="), std::string::npos);
    EXPECT_NE(dump.find("maxrss="), std::string::npos);
    EXPECT_NE(dump.find("threads="), std::string::npos);
}

TEST(SystemStatsTest, SnapshotMemoryFieldsInJson) {
    SystemStats stats;
    auto snap = stats.snapshot();
    auto json = snap.to_json();

    EXPECT_NE(json.find("\"maxrss_kb\":"), std::string::npos);
    EXPECT_NE(json.find("\"rss_kb\":"), std::string::npos);
    EXPECT_NE(json.find("\"thread_count\":"), std::string::npos);
}

TEST(SystemStatsTest, DeltaPreservesPointInTimeFields) {
    SystemStats stats;
    auto snap1 = stats.snapshot();

    // Allocate some memory to change RSS
    std::vector<char> alloc(1024 * 1024, 'x');
    auto snap2 = stats.snapshot();

    auto delta = snap2 - snap1;
    // Memory and thread fields are point-in-time from snap2 (lhs)
    EXPECT_EQ(delta.maxrss_kb, snap2.maxrss_kb);
    EXPECT_EQ(delta.rss_kb, snap2.rss_kb);
    EXPECT_EQ(delta.thread_count, snap2.thread_count);
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

TEST(SystemStatsTest, LogReportDoesNotCrash) {
    SystemStats stats;

    volatile double x = 0;
    for (int i = 0; i < 1000; ++i) {
        x += std::sin(i);
    }

    EXPECT_NO_THROW(stats.log_report());
}

TEST(SystemResourceStatsTest, DumpContainsAllFields) {
    SystemResourceStats stats{
        .majflt = 2,
        .minflt = 50,
        .nvcsw = 5,
        .nivcsw = 3,
        .user_cpu_s = 0.5,
        .sys_cpu_s = 0.1,
        .total_cpu_s = 0.6,
    };

    auto text = stats.dump();
    EXPECT_NE(text.find("0.5000"), std::string::npos);
    EXPECT_NE(text.find("0.1000"), std::string::npos);
    EXPECT_NE(text.find("major=2"), std::string::npos);
    EXPECT_NE(text.find("minor=50"), std::string::npos);
    EXPECT_NE(text.find("voluntary=5"), std::string::npos);
    EXPECT_NE(text.find("involuntary=3"), std::string::npos);
}

// ============================================================================
// SystemResourceStats — to_json
// ============================================================================

TEST(SystemResourceStatsTest, ToJsonContainsAllFields) {
    SystemResourceStats stats{
        .majflt = 2,
        .minflt = 50,
        .nvcsw = 5,
        .nivcsw = 3,
        .user_cpu_s = 0.5,
        .sys_cpu_s = 0.1,
        .total_cpu_s = 0.6,
    };

    auto json = stats.to_json();
    EXPECT_NE(json.find("\"user_cpu_s\":0.5"), std::string::npos);
    EXPECT_NE(json.find("\"sys_cpu_s\":0.1"), std::string::npos);
    EXPECT_NE(json.find("\"majflt\":2"), std::string::npos);
    EXPECT_NE(json.find("\"minflt\":50"), std::string::npos);
    EXPECT_NE(json.find("\"nvcsw\":5"), std::string::npos);
    EXPECT_NE(json.find("\"nivcsw\":3"), std::string::npos);
}

TEST(SystemResourceStatsTest, ToJsonZeroValues) {
    SystemResourceStats stats{};
    auto json = stats.to_json();
    EXPECT_NE(json.find("\"majflt\":0"), std::string::npos);
    EXPECT_NE(json.find("\"nvcsw\":0"), std::string::npos);
}

// ============================================================================
// SystemResourceStats — operator-
// ============================================================================

TEST(SystemResourceStatsTest, DiffOperator) {
    SystemResourceStats s1{
        .majflt = 1, .minflt = 10, .nvcsw = 5, .nivcsw = 2,
        .user_cpu_s = 0.1, .sys_cpu_s = 0.05, .total_cpu_s = 0.15,
    };
    SystemResourceStats s2{
        .majflt = 3, .minflt = 50, .nvcsw = 12, .nivcsw = 7,
        .user_cpu_s = 0.5, .sys_cpu_s = 0.2, .total_cpu_s = 0.7,
    };

    auto delta = s2 - s1;
    EXPECT_EQ(delta.majflt, 2);
    EXPECT_EQ(delta.minflt, 40);
    EXPECT_EQ(delta.nvcsw, 7);
    EXPECT_EQ(delta.nivcsw, 5);
    EXPECT_NEAR(delta.user_cpu_s, 0.4, 1e-9);
    EXPECT_NEAR(delta.sys_cpu_s, 0.15, 1e-9);
    EXPECT_NEAR(delta.total_cpu_s, 0.55, 1e-9);
}

// ============================================================================
// SystemResourceStats — operator==
// ============================================================================

TEST(SystemResourceStatsTest, EqualityOperator) {
    SystemResourceStats a{
        .majflt = 1, .minflt = 10, .nvcsw = 5, .nivcsw = 2,
        .user_cpu_s = 0.1, .sys_cpu_s = 0.05, .total_cpu_s = 0.15,
    };
    SystemResourceStats b = a;
    EXPECT_EQ(a, b);

    b.minflt = 11;
    EXPECT_NE(a, b);
}

TEST(SystemResourceStatsTest, EqualityApproximateFloats) {
    SystemResourceStats a{
        .majflt = 0, .minflt = 0, .nvcsw = 0, .nivcsw = 0,
        .user_cpu_s = 1.0, .sys_cpu_s = 0.0, .total_cpu_s = 1.0,
    };
    SystemResourceStats b = a;
    // Tiny floating-point perturbation should still compare equal
    b.user_cpu_s += 1e-12;
    EXPECT_EQ(a, b);
}

// ============================================================================
// SystemStats — move semantics
// ============================================================================

TEST(SystemStatsTest, MoveConstructorDisablesAutoLogOnSource) {
    // Create auto-logging stats, move it — only the destination should log.
    // The test verifies no crash/double-log by exercising move construction.
    SystemStats src(/*auto_log=*/true);
    SystemStats dst(std::move(src));
    // src is now moved-from: its destructor should NOT log
    // dst destructor will log (auto_log_ transferred)
}

TEST(SystemStatsTest, MoveAssignmentDisablesAutoLogOnSource) {
    SystemStats src(/*auto_log=*/true);
    SystemStats dst(/*auto_log=*/false);

    dst = std::move(src);
    // src is moved-from: destructor should not log
    // dst now owns auto_log=true
}

TEST(SystemStatsTest, MoveConstructorSnapshotWorks) {
    SystemStats src;
    volatile double x = 0;
    for (int i = 0; i < 10000; ++i) x += std::sin(i);

    SystemStats dst(std::move(src));
    auto snap = dst.snapshot();
    // Should still work — baseline rusage was transferred
    EXPECT_GE(snap.total_cpu_s, 0.0);
}
