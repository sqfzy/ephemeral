/// @file test_daemon_disconnected_hook.cpp
/// Tests for `daemon_disconnected_hook.hpp` (T1.1 + T1.2 skeleton —
/// primitive only, rx/tx_burst wire-up deferred).
///
/// What's tested:
///   - InFlightStatus to_string mapping
///   - thread_local detail round-trip (set → read → clear → read again)
///   - Cross-thread isolation: thread A's detail does not leak into B
///   - detected_at_ns is non-zero after set_*; monotonically advances
///     across consecutive sets
///   - clear_*() returns the detail to its default state (Unsent / 0
///     / 0 / empty phase / 0 ns)
///
/// Defer: integration test that exercises the actual rx/tx_burst
/// detection flow needs a live daemon kill scenario — see T1.4 in
/// DEFERRED.md.

#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "eph/net/dpdk/detail/daemon_disconnected_hook.hpp"

namespace ed = eph::net::dpdk::detail;
using ed::InFlightStatus;

TEST(DaemonDisconnectedHook, InFlightStatusToString) {
    EXPECT_STREQ(ed::to_string(InFlightStatus::Unsent),    "Unsent");
    EXPECT_STREQ(ed::to_string(InFlightStatus::Sent),      "Sent");
    EXPECT_STREQ(ed::to_string(InFlightStatus::Uncertain), "Uncertain");
}

TEST(DaemonDisconnectedHook, DefaultDetailIsUnsentZero) {
    ed::clear_daemon_disconnected_detail();
    auto& d = ed::last_daemon_disconnected_detail();
    EXPECT_EQ(d.status, InFlightStatus::Unsent);
    EXPECT_EQ(d.bytes_observed, 0u);
    EXPECT_EQ(d.bytes_confirmed, 0u);
    EXPECT_STREQ(d.phase, "");
    EXPECT_EQ(d.detected_at_ns, 0u);
}

TEST(DaemonDisconnectedHook, SetDetailRoundTrip) {
    ed::clear_daemon_disconnected_detail();
    ed::set_daemon_disconnected_detail(
        InFlightStatus::Uncertain,
        /*bytes_observed=*/1024,
        /*bytes_confirmed=*/512,
        /*phase=*/"tcp_send_pre_burst");

    const auto& d = ed::last_daemon_disconnected_detail();
    EXPECT_EQ(d.status, InFlightStatus::Uncertain);
    EXPECT_EQ(d.bytes_observed, 1024u);
    EXPECT_EQ(d.bytes_confirmed, 512u);
    EXPECT_STREQ(d.phase, "tcp_send_pre_burst");
    EXPECT_GT(d.detected_at_ns, 0u);
}

TEST(DaemonDisconnectedHook, ClearResetsToDefault) {
    ed::set_daemon_disconnected_detail(
        InFlightStatus::Sent, 100, 100, "x");
    ed::clear_daemon_disconnected_detail();

    const auto& d = ed::last_daemon_disconnected_detail();
    EXPECT_EQ(d.status, InFlightStatus::Unsent);
    EXPECT_EQ(d.bytes_observed, 0u);
    EXPECT_EQ(d.bytes_confirmed, 0u);
    EXPECT_STREQ(d.phase, "");
    EXPECT_EQ(d.detected_at_ns, 0u);
}

TEST(DaemonDisconnectedHook, ConsecutiveSetsAdvanceTimestamp) {
    ed::clear_daemon_disconnected_detail();

    ed::set_daemon_disconnected_detail(
        InFlightStatus::Unsent, 0, 0, "first");
    const uint64_t ts1 = ed::last_daemon_disconnected_detail().detected_at_ns;

    // Sleep tiny amount so steady_clock advances at least one tick.
    std::this_thread::sleep_for(std::chrono::microseconds(10));

    ed::set_daemon_disconnected_detail(
        InFlightStatus::Sent, 100, 100, "second");
    const uint64_t ts2 = ed::last_daemon_disconnected_detail().detected_at_ns;

    EXPECT_GT(ts2, ts1);
    EXPECT_EQ(ed::last_daemon_disconnected_detail().status,
              InFlightStatus::Sent);
    EXPECT_STREQ(ed::last_daemon_disconnected_detail().phase, "second");
}

TEST(DaemonDisconnectedHook, ThreadLocalIsolation) {
    ed::clear_daemon_disconnected_detail();
    ed::set_daemon_disconnected_detail(
        InFlightStatus::Sent, 11, 22, "main_thread");

    InFlightStatus child_observed_status{};
    size_t         child_observed_bytes{};
    const char*    child_observed_phase = nullptr;

    std::thread t([&] {
        // Child thread starts with its own (default-constructed)
        // thread_local detail — main thread's set_* must not leak in.
        const auto& d = ed::last_daemon_disconnected_detail();
        child_observed_status = d.status;
        child_observed_bytes  = d.bytes_observed;
        child_observed_phase  = d.phase;

        // Set on the child thread; this MUST NOT clobber main's detail.
        ed::set_daemon_disconnected_detail(
            InFlightStatus::Uncertain, 99, 0, "child_thread");
    });
    t.join();

    EXPECT_EQ(child_observed_status, InFlightStatus::Unsent);
    EXPECT_EQ(child_observed_bytes,  0u);
    EXPECT_STREQ(child_observed_phase, "");

    // Main thread's detail is unchanged by the child's set.
    const auto& d = ed::last_daemon_disconnected_detail();
    EXPECT_EQ(d.status, InFlightStatus::Sent);
    EXPECT_EQ(d.bytes_observed, 11u);
    EXPECT_EQ(d.bytes_confirmed, 22u);
    EXPECT_STREQ(d.phase, "main_thread");
}

TEST(DaemonDisconnectedHook, NullPhaseClampsToEmpty) {
    ed::clear_daemon_disconnected_detail();
    ed::set_daemon_disconnected_detail(
        InFlightStatus::Unsent, 0, 0, /*phase=*/nullptr);
    EXPECT_STREQ(ed::last_daemon_disconnected_detail().phase, "");
}
