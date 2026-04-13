/// @file test_shutdown_signal.cpp
/// Unit tests for shutdown_signal.hpp — process-wide cooperative shutdown flag.
///
/// Note: We avoid testing actual signal delivery (SIGINT/SIGTERM) because
/// sending signals in a test binary can interfere with the test runner.
/// Instead we test the flag semantics and the handler function directly.

#include <gtest/gtest.h>

#include <atomic>

#include "eph/utils/shutdown_signal.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// Fixture: reset the global flag before each test
// ---------------------------------------------------------------------------

class ShutdownSignalTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset to default "running" state
        g_shutdown_flag.store(true, std::memory_order_release);
    }
    void TearDown() override {
        // Restore for other tests
        g_shutdown_flag.store(true, std::memory_order_release);
    }
};

// ---------------------------------------------------------------------------
// g_shutdown_flag default state
// ---------------------------------------------------------------------------

TEST_F(ShutdownSignalTest, default_state_is_true) {
    EXPECT_TRUE(g_shutdown_flag.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// Direct flag manipulation
// ---------------------------------------------------------------------------

TEST_F(ShutdownSignalTest, store_false_stops_running) {
    g_shutdown_flag.store(false, std::memory_order_release);
    EXPECT_FALSE(g_shutdown_flag.load(std::memory_order_relaxed));
}

TEST_F(ShutdownSignalTest, store_true_restores_running) {
    g_shutdown_flag.store(false, std::memory_order_release);
    g_shutdown_flag.store(true, std::memory_order_release);
    EXPECT_TRUE(g_shutdown_flag.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// on_shutdown_signal handler function (direct call, no actual signal)
// ---------------------------------------------------------------------------

TEST_F(ShutdownSignalTest, handler_sets_flag_false) {
    EXPECT_TRUE(g_shutdown_flag.load(std::memory_order_relaxed));
    detail::on_shutdown_signal(SIGINT);
    EXPECT_FALSE(g_shutdown_flag.load(std::memory_order_acquire));
}

TEST_F(ShutdownSignalTest, handler_idempotent) {
    detail::on_shutdown_signal(SIGINT);
    detail::on_shutdown_signal(SIGTERM);
    detail::on_shutdown_signal(SIGINT);
    EXPECT_FALSE(g_shutdown_flag.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------------------
// install_shutdown_handlers — safe to call multiple times
// ---------------------------------------------------------------------------

TEST_F(ShutdownSignalTest, install_handlers_does_not_crash) {
    ASSERT_NO_THROW(install_shutdown_handlers());
}

TEST_F(ShutdownSignalTest, install_handlers_idempotent) {
    install_shutdown_handlers();
    install_shutdown_handlers();
    install_shutdown_handlers();
    // Flag should still be true (handlers only fire on actual signals)
    EXPECT_TRUE(g_shutdown_flag.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// Poll-loop pattern
// ---------------------------------------------------------------------------

TEST_F(ShutdownSignalTest, poll_loop_exits_when_flag_cleared) {
    int iterations = 0;
    // Simulate: clear flag after a few iterations
    while (g_shutdown_flag.load(std::memory_order_relaxed)) {
        ++iterations;
        if (iterations >= 10) {
            detail::on_shutdown_signal(SIGINT);
        }
    }
    EXPECT_EQ(iterations, 10);
}

// ---------------------------------------------------------------------------
// Atomic properties
// ---------------------------------------------------------------------------

TEST_F(ShutdownSignalTest, flag_is_lock_free) {
    EXPECT_TRUE(g_shutdown_flag.is_lock_free());
}
