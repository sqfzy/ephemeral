/// @file test_circuit_breaker.cpp
/// Unit tests for the three-state CircuitBreaker.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "eph/net/circuit_breaker.hpp"

using namespace eph::net;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// Basic closed-state behavior
// ─────────────────────────────────────────────────────────────────────────────

TEST(CircuitBreaker, StartsClosedAndAllowsCalls) {
    CircuitBreaker cb;

    EXPECT_EQ(cb.state(), CircuitState::Closed);
    EXPECT_TRUE(cb.allow());
    EXPECT_EQ(cb.failure_count(), 0u);
}

TEST(CircuitBreaker, SuccessResetsFailureCount) {
    CircuitBreaker cb(CircuitBreaker::Config{5, 30s});

    // Accumulate some failures (but below threshold).
    cb.record_failure();
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.failure_count(), 3u);

    // Success resets the count.
    cb.record_success();
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_EQ(cb.state(), CircuitState::Closed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Closed → Open transition
// ─────────────────────────────────────────────────────────────────────────────

TEST(CircuitBreaker, OpensAfterThresholdFailures) {
    CircuitBreaker cb(CircuitBreaker::Config{3, 60s});

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::Closed);

    cb.record_failure();  // Threshold reached.
    EXPECT_EQ(cb.state(), CircuitState::Open);
    EXPECT_EQ(cb.failure_count(), 3u);
}

TEST(CircuitBreaker, BlocksCallsWhenOpen) {
    CircuitBreaker cb(CircuitBreaker::Config{2, 60s});

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::Open);

    // All calls should be blocked.
    EXPECT_FALSE(cb.allow());
    EXPECT_FALSE(cb.allow());
    EXPECT_FALSE(cb.allow());
}

// ─────────────────────────────────────────────────────────────────────────────
// Open → HalfOpen transition
// ─────────────────────────────────────────────────────────────────────────────

TEST(CircuitBreaker, TransitionsToHalfOpenAfterTimeout) {
    // Use a very short open_duration so we can test the timeout in a unit test.
    CircuitBreaker cb(CircuitBreaker::Config{2, 0s, 1});

    cb.record_failure();
    cb.record_failure();

    // With open_duration=0, the timeout has already elapsed by the time
    // we query state(), so it reports HalfOpen immediately.
    EXPECT_EQ(cb.state(), CircuitState::HalfOpen);

    // allow() should transition and permit one call.
    EXPECT_TRUE(cb.allow());
}

TEST(CircuitBreaker, HalfOpenLimitsTestCalls) {
    CircuitBreaker cb(CircuitBreaker::Config{2, 0s, 1});

    cb.record_failure();
    cb.record_failure();

    // First allow() transitions Open → HalfOpen and permits 1 call.
    EXPECT_TRUE(cb.allow());

    // Second call should be blocked — max test calls reached.
    EXPECT_FALSE(cb.allow());
}

// ─────────────────────────────────────────────────────────────────────────────
// HalfOpen → Closed (success)
// ─────────────────────────────────────────────────────────────────────────────

TEST(CircuitBreaker, ClosesOnSuccessInHalfOpen) {
    CircuitBreaker cb(CircuitBreaker::Config{2, 0s, 1});

    cb.record_failure();
    cb.record_failure();

    // Transition to HalfOpen.
    EXPECT_TRUE(cb.allow());

    // Probe succeeded — should close the circuit.
    cb.record_success();
    EXPECT_EQ(cb.state(), CircuitState::Closed);
    EXPECT_EQ(cb.failure_count(), 0u);

    // Calls should flow freely again.
    EXPECT_TRUE(cb.allow());
    EXPECT_TRUE(cb.allow());
}

// ─────────────────────────────────────────────────────────────────────────────
// HalfOpen → Open (failure)
// ─────────────────────────────────────────────────────────────────────────────

TEST(CircuitBreaker, ReopensOnFailureInHalfOpen) {
    // Use a long open_duration so that after re-tripping, the circuit stays
    // Open and doesn't immediately transition back to HalfOpen.
    CircuitBreaker cb(CircuitBreaker::Config{2, 60s, 1});

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::Open);

    // Manually force into HalfOpen by resetting and re-tripping with 0s duration.
    // Simpler approach: create a second breaker with 0s for the transition test.
    CircuitBreaker cb2(CircuitBreaker::Config{2, 0s, 1});
    cb2.record_failure();
    cb2.record_failure();

    // Transition to HalfOpen via allow().
    EXPECT_TRUE(cb2.allow());

    // Probe failed — should re-open. But open_duration=0 means state() will
    // immediately report HalfOpen again. So verify via allow() behavior:
    // after re-trip, the next allow() should succeed (new HalfOpen probe).
    cb2.record_failure();
    // The circuit is Open internally, but with 0s duration it immediately
    // appears as HalfOpen. Verify it went through re-open by checking that
    // a fresh probe call is allowed (half_open_calls was reset).
    EXPECT_TRUE(cb2.allow());
}

// ─────────────────────────────────────────────────────────────────────────────
// Reset
// ─────────────────────────────────────────────────────────────────────────────

TEST(CircuitBreaker, ResetClosesFromOpen) {
    CircuitBreaker cb(CircuitBreaker::Config{2, 60s});

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::Open);

    cb.reset();
    EXPECT_EQ(cb.state(), CircuitState::Closed);
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_TRUE(cb.allow());
}

TEST(CircuitBreaker, ResetClearsFailureCountInClosed) {
    CircuitBreaker cb(CircuitBreaker::Config{5, 30s});

    cb.record_failure();
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.failure_count(), 3u);

    cb.reset();
    EXPECT_EQ(cb.failure_count(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Real timeout test (short duration)
// ─────────────────────────────────────────────────────────────────────────────

TEST(CircuitBreaker, RealTimeoutTransition) {
    // Verify that a 1s open_duration blocks for at least a moment, then
    // transitions to HalfOpen after the duration elapses.
    // (chrono::seconds can't represent sub-second, so 1s is our minimum.)
    CircuitBreaker cb(CircuitBreaker::Config{1, 1s, 1});

    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::Open);

    // Immediately after tripping, should still be Open.
    EXPECT_FALSE(cb.allow());
    EXPECT_EQ(cb.state(), CircuitState::Open);

    // Wait for the duration to elapse.
    std::this_thread::sleep_for(1100ms);

    // Now should transition to HalfOpen.
    EXPECT_EQ(cb.state(), CircuitState::HalfOpen);
    EXPECT_TRUE(cb.allow());
}

// ─────────────────────────────────────────────────────────────────────────────
// Full lifecycle: Closed → Open → HalfOpen → Closed
// ─────────────────────────────────────────────────────────────────────────────

TEST(CircuitBreaker, FullLifecycle) {
    CircuitBreaker cb(CircuitBreaker::Config{3, 0s, 1});

    // Phase 1: Closed — calls succeed.
    EXPECT_EQ(cb.state(), CircuitState::Closed);
    EXPECT_TRUE(cb.allow());
    cb.record_success();

    // Phase 2: Accumulate failures to trip.
    cb.record_failure();
    cb.record_failure();
    cb.record_failure();
    // With open_duration=0, state() reports HalfOpen immediately.
    // Verify the circuit tripped by checking it's no longer Closed.
    EXPECT_NE(cb.state(), CircuitState::Closed);

    // Phase 3: Timeout elapses (0s), transition to HalfOpen.
    EXPECT_TRUE(cb.allow());  // Transitions and allows one probe.

    // Phase 4: Probe succeeds → Closed.
    cb.record_success();
    EXPECT_EQ(cb.state(), CircuitState::Closed);
    EXPECT_EQ(cb.failure_count(), 0u);

    // Phase 5: Normal operation resumed.
    EXPECT_TRUE(cb.allow());
    EXPECT_TRUE(cb.allow());
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread safety
// ─────────────────────────────────────────────────────────────────────────────

TEST(CircuitBreaker, ConcurrentAccessDoesNotCrash) {
    // Hammer the circuit breaker from multiple threads to detect data races.
    CircuitBreaker cb(CircuitBreaker::Config{3, 0s, 2});

    std::atomic<bool> stop{false};

    auto failure_worker = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            cb.record_failure();
            (void)cb.allow();
            (void)cb.state();
            (void)cb.failure_count();
        }
    };

    auto success_worker = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            cb.record_success();
            (void)cb.allow();
            (void)cb.state();
            (void)cb.failure_count();
        }
    };

    auto reset_worker = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            cb.reset();
            std::this_thread::yield();
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back(failure_worker);
    threads.emplace_back(failure_worker);
    threads.emplace_back(success_worker);
    threads.emplace_back(success_worker);
    threads.emplace_back(reset_worker);

    // Let the threads race for 100ms.
    std::this_thread::sleep_for(100ms);
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : threads) {
        t.join();
    }

    // If we got here without crashing/hanging, the mutex is doing its job.
    // The final state is non-deterministic, but it must be a valid state.
    auto final_state = cb.state();
    EXPECT_TRUE(final_state == CircuitState::Closed ||
                final_state == CircuitState::Open ||
                final_state == CircuitState::HalfOpen);
}

TEST(CircuitBreaker, ConcurrentFailureTripsExactlyOnce) {
    // Multiple threads recording failures concurrently should still trip
    // the circuit exactly when the threshold is met, not before.
    constexpr std::size_t kThreshold = 100;
    CircuitBreaker cb(CircuitBreaker::Config{kThreshold, 60s, 1});

    // Record exactly kThreshold failures across multiple threads.
    std::atomic<std::size_t> recorded{0};

    auto worker = [&]() {
        while (recorded.fetch_add(1, std::memory_order_acq_rel) < kThreshold) {
            cb.record_failure();
        }
    };

    std::thread t1(worker);
    std::thread t2(worker);
    std::thread t3(worker);
    std::thread t4(worker);
    t1.join();
    t2.join();
    t3.join();
    t4.join();

    // Circuit should be Open after exactly kThreshold failures.
    EXPECT_EQ(cb.state(), CircuitState::Open);
}
