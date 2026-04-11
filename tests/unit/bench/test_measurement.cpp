/// @file tests/unit/bench/test_measurement.cpp
/// Smoke tests for `bench::measurement` helpers (Phase 10).
///
/// `monotonic_raw_ns()` and `print_report()` are small enough that
/// covering them with an end-to-end scenario is overkill; these unit
/// tests lock in the invariants the scenarios actually rely on:
///   - clock is monotonically non-decreasing
///   - successive reads are close to real-time (sanity)
///   - shutdown flag round-trips through the installed signal handler
///   - print_report gracefully handles an empty Recorder

#include <chrono>
#include <csignal>
#include <thread>

#include <gtest/gtest.h>

#include "core/measurement.hpp"

#include <eph/utils/recorder.hpp>

// ── monotonic_raw_ns ───────────────────────────────────────────────────

TEST(Measurement, MonotonicRawNsMonotonicAcross1000Reads) {
    // Monotonicity is the single most important property — it's what
    // allows `t1 - t0` to be a valid latency measurement.
    uint64_t prev = bench::monotonic_raw_ns();
    for (int i = 0; i < 1000; ++i) {
        uint64_t cur = bench::monotonic_raw_ns();
        EXPECT_GE(cur, prev);
        prev = cur;
    }
}

TEST(Measurement, MonotonicRawNsAdvancesDuringSleep) {
    // Sanity: 5 ms of real-world sleep should show up as at least
    // 1 ms of clock advance (generous threshold for CI jitter).
    auto t0 = bench::monotonic_raw_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto t1 = bench::monotonic_raw_ns();
    EXPECT_GT(t1 - t0, 1'000'000ull);
}

// ── shutdown flag + signal handler ─────────────────────────────────────

TEST(Measurement, ShutdownFlagInitiallyFalse) {
    // Reset first in case a previous test left it set.
    bench::g_shutdown_requested.store(false, std::memory_order_release);
    EXPECT_FALSE(bench::shutdown_requested());
}

TEST(Measurement, SignalHandlerSetsFlag) {
    bench::g_shutdown_requested.store(false, std::memory_order_release);
    bench::install_signal_handler();

    // Deliver SIGTERM to ourselves and verify the flag flips. We pick
    // SIGTERM over SIGINT because some harness parents forward SIGINT
    // to the whole group.
    ::raise(SIGTERM);

    EXPECT_TRUE(bench::shutdown_requested());

    // Clean up for subsequent tests.
    bench::g_shutdown_requested.store(false, std::memory_order_release);
}

// ── print_report ───────────────────────────────────────────────────────

TEST(Measurement, PrintReportEmptyRecorderDoesNotCrash) {
    // Empty Recorder → compute_stats() returns nullopt → print_report
    // prints the "no data recorded" branch. We redirect stdout to a pipe
    // so the test does not litter gtest output.
    eph::utils::Recorder rec{"smoke"};
    ::testing::internal::CaptureStdout();
    bench::print_report("lat_smoke", "kernel", rec, /*warmup_discarded=*/0);
    auto out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("lat_smoke"), std::string::npos);
    EXPECT_NE(out.find("kernel"), std::string::npos);
    EXPECT_NE(out.find("no data recorded"), std::string::npos);
}

TEST(Measurement, PrintReportWithSamplesShowsStats) {
    eph::utils::Recorder rec{"smoke"};
    // Feed a deterministic distribution: 100 samples at 1000 ns.
    // All percentiles should come back close to 1000.
    for (int i = 0; i < 100; ++i) {
        rec.record_ns(1000);
    }
    ::testing::internal::CaptureStdout();
    bench::print_report("lat_smoke", "dpdk", rec, /*warmup_discarded=*/10);
    auto out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("lat_smoke"), std::string::npos);
    EXPECT_NE(out.find("dpdk"), std::string::npos);
    EXPECT_NE(out.find("samples: 100"), std::string::npos);
    EXPECT_NE(out.find("warmup 10 discarded"), std::string::npos);
    EXPECT_NE(out.find("p50"), std::string::npos);
}
