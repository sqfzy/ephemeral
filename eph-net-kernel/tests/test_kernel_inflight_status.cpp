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
