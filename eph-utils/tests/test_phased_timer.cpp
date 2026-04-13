/// @file test_phased_timer.cpp
/// Unit tests for PhasedTimer — two-phase TSC deadline timer.

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "eph/utils/phased_timer.hpp"
#include "eph/utils/time.hpp"

using eph::utils::PhasedTimer;
using eph::utils::TSC;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Fixture: ensure TSC is calibrated once
// ---------------------------------------------------------------------------

class PhasedTimerTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        TSC::init();
    }
};

// ---------------------------------------------------------------------------
// Default state (before start)
// ---------------------------------------------------------------------------

TEST_F(PhasedTimerTest, default_constructed_is_not_running) {
    PhasedTimer t;
    EXPECT_FALSE(t.is_running());
}

TEST_F(PhasedTimerTest, default_constructed_is_not_warmup) {
    PhasedTimer t;
    EXPECT_FALSE(t.is_warmup());
}

// ---------------------------------------------------------------------------
// Basic start / warmup / measurement phases
// ---------------------------------------------------------------------------

TEST_F(PhasedTimerTest, immediately_after_start_is_running) {
    PhasedTimer t;
    t.start(100ms, 500ms);
    EXPECT_TRUE(t.is_running());
}

TEST_F(PhasedTimerTest, warmup_phase_detected_immediately_after_start) {
    PhasedTimer t;
    t.start(500ms, 500ms);
    // Right after start, we should be in warmup
    EXPECT_TRUE(t.is_warmup());
    EXPECT_TRUE(t.is_running());
}

TEST_F(PhasedTimerTest, zero_warmup_skips_warmup_phase) {
    PhasedTimer t;
    t.start(0ns, 500ms);
    // With zero warmup, is_warmup() should be false immediately
    EXPECT_FALSE(t.is_warmup());
    EXPECT_TRUE(t.is_running());
}

TEST_F(PhasedTimerTest, timer_eventually_stops_running) {
    PhasedTimer t;
    // Use very short durations so the test completes quickly
    t.start(0ns, 1ms);
    std::this_thread::sleep_for(5ms);
    EXPECT_FALSE(t.is_running());
}

TEST_F(PhasedTimerTest, warmup_eventually_ends) {
    PhasedTimer t;
    t.start(1ms, 200ms);
    std::this_thread::sleep_for(5ms);
    // After 5ms, the 1ms warmup window should have closed
    EXPECT_FALSE(t.is_warmup());
    // But we should still be running (measurement window is 200ms)
    EXPECT_TRUE(t.is_running());
}

TEST_F(PhasedTimerTest, both_phases_expire_after_total_duration) {
    PhasedTimer t;
    t.start(1ms, 2ms);
    std::this_thread::sleep_for(10ms);
    EXPECT_FALSE(t.is_warmup());
    EXPECT_FALSE(t.is_running());
}

// ---------------------------------------------------------------------------
// Restart semantics
// ---------------------------------------------------------------------------

TEST_F(PhasedTimerTest, start_can_be_called_multiple_times) {
    PhasedTimer t;

    // First run — let it expire
    t.start(0ns, 1ms);
    std::this_thread::sleep_for(5ms);
    EXPECT_FALSE(t.is_running());

    // Restart — should be running again
    t.start(0ns, 500ms);
    EXPECT_TRUE(t.is_running());
}

// ---------------------------------------------------------------------------
// Phase transition: warmup → measurement → stopped
// ---------------------------------------------------------------------------

TEST_F(PhasedTimerTest, phase_transition_sequence) {
    PhasedTimer t;
    t.start(5ms, 50ms);

    // Phase 1: warmup active
    EXPECT_TRUE(t.is_warmup());
    EXPECT_TRUE(t.is_running());

    // Wait past warmup
    std::this_thread::sleep_for(10ms);

    // Phase 2: measurement active (warmup done, still running)
    EXPECT_FALSE(t.is_warmup());
    EXPECT_TRUE(t.is_running());

    // Wait past total duration
    std::this_thread::sleep_for(50ms);

    // Phase 3: done
    EXPECT_FALSE(t.is_warmup());
    EXPECT_FALSE(t.is_running());
}

// ---------------------------------------------------------------------------
// Typical benchmark pattern
// ---------------------------------------------------------------------------

TEST_F(PhasedTimerTest, benchmark_loop_pattern) {
    PhasedTimer t;
    t.start(1ms, 5ms);

    int warmup_iters = 0;
    int measure_iters = 0;

    while (t.is_running()) {
        if (t.is_warmup()) {
            ++warmup_iters;
        } else {
            ++measure_iters;
        }
        // Small yield to avoid spinning too hard in CI
        std::this_thread::sleep_for(100us);
    }

    // Both phases should have had at least one iteration
    EXPECT_GT(warmup_iters, 0);
    EXPECT_GT(measure_iters, 0);
}

// ---------------------------------------------------------------------------
// Edge case: zero-length measurement window
// ---------------------------------------------------------------------------

TEST_F(PhasedTimerTest, zero_measurement_with_warmup_still_runs_briefly) {
    PhasedTimer t;
    t.start(1ms, 0ns);
    // Timer should be running during warmup
    EXPECT_TRUE(t.is_running());
    // After warmup expires, is_running should become false
    std::this_thread::sleep_for(5ms);
    EXPECT_FALSE(t.is_running());
}

TEST_F(PhasedTimerTest, zero_warmup_zero_measurement_not_running) {
    PhasedTimer t;
    t.start(0ns, 0ns);
    // Both windows are zero-length — immediate stop
    EXPECT_FALSE(t.is_running());
}

// ---------------------------------------------------------------------------
// POD / trivial semantics
// ---------------------------------------------------------------------------

TEST_F(PhasedTimerTest, is_trivially_copyable) {
    static_assert(std::is_trivially_copyable_v<PhasedTimer>);
}

TEST_F(PhasedTimerTest, copy_preserves_state) {
    PhasedTimer t;
    t.start(0ns, 500ms);
    EXPECT_TRUE(t.is_running());

    PhasedTimer t2 = t;
    EXPECT_TRUE(t2.is_running());
}
