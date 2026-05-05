/// @file test_kernel_inflight_status.cpp
/// J series cross-backend symmetry tests for kernel-side
/// `InFlightStatus` classification.
///
/// These tests verify the **primitive** populate/read contract — same
/// shape as `test_inflight_classification.cpp` on the DPDK side, but
/// asserts the kernel call-site phase tags. Full end-to-end behaviour
/// (call KernelTcpStream::send → observe Unsent in detail) is covered
/// by the existing `test_kernel_tcp_stream_behavioral` suite indirectly
/// (those tests construct a real socket pair and trigger the failure
/// paths the wire-up populates).

#include <cstring>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include "eph/net/in_flight_status.hpp"

namespace en = eph::net;

TEST(KernelInFlightStatus, SharedThreadLocalAcrossBackends) {
    // Cross-backend invariant: if a process uses both kernel + DPDK
    // streams (rare but legitimate — e.g. control plane on kernel,
    // market data on DPDK), the thread_local detail is the SAME
    // storage. Setting on one path must be readable on another path.
    en::clear_in_flight_detail();

    en::set_in_flight_detail(en::InFlightStatus::Sent,
                             1024, 1024, "DpdkTcpStream::send");
    {
        const auto& d = en::last_in_flight_detail();
        EXPECT_EQ(d.status, en::InFlightStatus::Sent);
        EXPECT_STREQ(d.phase, "DpdkTcpStream::send");
    }

    en::set_in_flight_detail(en::InFlightStatus::Unsent,
                             512, 0, "KernelTcpStream::send");
    {
        const auto& d = en::last_in_flight_detail();
        EXPECT_EQ(d.status, en::InFlightStatus::Unsent);
        EXPECT_EQ(d.bytes_observed, 512u);
        EXPECT_EQ(d.bytes_confirmed, 0u);
        EXPECT_STREQ(d.phase, "KernelTcpStream::send");
    }
}

TEST(KernelInFlightStatus, KernelTcpSendUnsentPhaseRoundTrip) {
    en::clear_in_flight_detail();
    en::set_in_flight_detail(en::InFlightStatus::Unsent,
                             4096, 0,
                             "KernelTcpStream::send(NotAttached)");
    const auto& d = en::last_in_flight_detail();
    EXPECT_EQ(d.status, en::InFlightStatus::Unsent);
    EXPECT_STREQ(d.phase, "KernelTcpStream::send(NotAttached)");
}

TEST(KernelInFlightStatus, KernelUdpSendToUnsentPhaseRoundTrip) {
    en::clear_in_flight_detail();
    en::set_in_flight_detail(en::InFlightStatus::Unsent,
                             1280, 0,
                             "KernelUdpSocket::send_to(EAGAIN)");
    const auto& d = en::last_in_flight_detail();
    EXPECT_EQ(d.status, en::InFlightStatus::Unsent);
    EXPECT_STREQ(d.phase, "KernelUdpSocket::send_to(EAGAIN)");
}

TEST(KernelInFlightStatus, KernelTlsUncertainPhaseRoundTrip) {
    en::clear_in_flight_detail();
    en::set_in_flight_detail(en::InFlightStatus::Uncertain,
                             8192, 0,
                             "KernelTcpStream::send(TLS,sock)");
    const auto& d = en::last_in_flight_detail();
    EXPECT_EQ(d.status, en::InFlightStatus::Uncertain);
    EXPECT_STREQ(d.phase, "KernelTcpStream::send(TLS,sock)");
}

TEST(KernelInFlightStatus, ToStringMappings) {
    EXPECT_STREQ(en::to_string(en::InFlightStatus::Sent),      "Sent");
    EXPECT_STREQ(en::to_string(en::InFlightStatus::Unsent),    "Unsent");
    EXPECT_STREQ(en::to_string(en::InFlightStatus::Uncertain), "Uncertain");
}

TEST(KernelInFlightStatus, ThreadLocalIsolation) {
    // Same thread-isolation invariant as the DPDK-side test —
    // confirms the shared header keeps thread_local semantics right.
    en::clear_in_flight_detail();
    en::set_in_flight_detail(en::InFlightStatus::Sent, 1, 1, "main");

    en::InFlightStatus child_observed{};
    std::thread t([&] {
        const auto& d = en::last_in_flight_detail();
        child_observed = d.status;  // expect default Unsent
        en::set_in_flight_detail(en::InFlightStatus::Uncertain,
                                 99, 0, "child");
    });
    t.join();

    EXPECT_EQ(child_observed, en::InFlightStatus::Unsent);  // child default
    const auto& d = en::last_in_flight_detail();
    EXPECT_EQ(d.status, en::InFlightStatus::Sent);  // main unchanged
    EXPECT_STREQ(d.phase, "main");
}

TEST(KernelInFlightStatus, DetectionTimestampMonotonic) {
    en::clear_in_flight_detail();
    en::set_in_flight_detail(en::InFlightStatus::Unsent, 0, 0, "t1");
    const uint64_t t1 = en::last_in_flight_detail().detected_at_ns;
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    en::set_in_flight_detail(en::InFlightStatus::Sent, 0, 0, "t2");
    const uint64_t t2 = en::last_in_flight_detail().detected_at_ns;
    EXPECT_GT(t2, t1);
}

// to_string has a trailing `return "Unknown";` for values cast outside
// the defined enum range. Pin the sentinel so a future refactor that
// drops the fallthrough surfaces here (UB territory: control falls off
// the end of a non-void function, only -Wreturn-type catches it).
TEST(KernelInFlightStatus, ToStringUnknownReturnsSentinel) {
    auto bogus = static_cast<en::InFlightStatus>(static_cast<uint8_t>(99));
    EXPECT_STREQ(en::to_string(bogus), "Unknown");
}

// set_in_flight_detail's `phase != nullptr` guard coerces a nullptr to
// "" so downstream loggers never deref a dangling pointer. Pin both
// the coercion AND the empty-phase result.
TEST(KernelInFlightStatus, SetInFlightDetailNullPhaseCoercedToEmpty) {
    en::clear_in_flight_detail();
    en::set_in_flight_detail(en::InFlightStatus::Uncertain, 7, 3, nullptr);
    const auto& d = en::last_in_flight_detail();
    EXPECT_EQ(d.status, en::InFlightStatus::Uncertain);
    EXPECT_EQ(d.bytes_observed, 7u);
    EXPECT_EQ(d.bytes_confirmed, 3u);
    ASSERT_NE(d.phase, nullptr);
    EXPECT_STREQ(d.phase, "");
}

// clear_in_flight_detail must reset every field — pin so a future
// addition of a new field that's not zeroed surfaces here.
TEST(KernelInFlightStatus, ClearResetsEveryField) {
    en::set_in_flight_detail(en::InFlightStatus::Sent, 100, 100, "before-clear");
    en::clear_in_flight_detail();
    const auto& d = en::last_in_flight_detail();
    EXPECT_EQ(d.status, en::InFlightStatus::Unsent);
    EXPECT_EQ(d.bytes_observed, 0u);
    EXPECT_EQ(d.bytes_confirmed, 0u);
    ASSERT_NE(d.phase, nullptr);
    EXPECT_STREQ(d.phase, "");
    EXPECT_EQ(d.detected_at_ns, 0u);
}
