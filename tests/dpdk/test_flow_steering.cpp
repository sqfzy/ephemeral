/// @file test_flow_steering.cpp
/// Unit tests for flow_steering.hpp: RxDispatchMode, FlowRule lifecycle,
/// RSS configuration helpers.
///
/// NOTE: Actual NIC hardware detection requires DPDK EAL + real NIC.
/// These tests cover:
///   - Enum names and values
///   - FlowRule RAII semantics (move, default state)
///   - detect_rx_dispatch_mode return value semantics

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/flow_steering.hpp"

using namespace eph::dpdk;

// ---------------------------------------------------------------------------
// RxDispatchMode
// ---------------------------------------------------------------------------

TEST(RxDispatchMode, NameSoftware) {
    EXPECT_EQ(rx_dispatch_mode_name(RxDispatchMode::Software),
              "Software (Reactor)");
}

TEST(RxDispatchMode, NameRss) {
    EXPECT_EQ(rx_dispatch_mode_name(RxDispatchMode::RssPartitioned),
              "RSS Partitioned");
}

TEST(RxDispatchMode, NameFlowDirector) {
    EXPECT_EQ(rx_dispatch_mode_name(RxDispatchMode::FlowDirector),
              "Flow Director (rte_flow)");
}

TEST(RxDispatchMode, EnumValues) {
    // Verify distinct values
    EXPECT_NE(static_cast<uint8_t>(RxDispatchMode::Software),
              static_cast<uint8_t>(RxDispatchMode::RssPartitioned));
    EXPECT_NE(static_cast<uint8_t>(RxDispatchMode::RssPartitioned),
              static_cast<uint8_t>(RxDispatchMode::FlowDirector));
}

// ---------------------------------------------------------------------------
// FlowRule RAII
// ---------------------------------------------------------------------------

TEST(FlowRule, DefaultConstructedIsInvalid) {
    FlowRule rule;
    EXPECT_FALSE(rule.valid());
    EXPECT_EQ(rule.handle, nullptr);
}

TEST(FlowRule, MoveTransfersOwnership) {
    FlowRule a;
    a.port_id = 1;
    a.queue_id = 3;
    // Can't set handle without real rte_flow, but verify move semantics
    FlowRule b = std::move(a);
    EXPECT_EQ(b.port_id, 1);
    EXPECT_EQ(b.queue_id, 3);
    EXPECT_EQ(a.handle, nullptr);  // Source nulled
}

TEST(FlowRule, MoveAssignmentTransfers) {
    FlowRule a;
    a.port_id = 2;
    a.queue_id = 5;
    FlowRule b;
    b = std::move(a);
    EXPECT_EQ(b.port_id, 2);
    EXPECT_EQ(b.queue_id, 5);
    EXPECT_EQ(a.handle, nullptr);
}

TEST(FlowRule, RemoveOnNullHandleIsSafe) {
    FlowRule rule;
    rule.remove();  // Should not crash
    EXPECT_FALSE(rule.valid());
}

// ---------------------------------------------------------------------------
// configure_rss validation
// ---------------------------------------------------------------------------

TEST(ConfigureRss, RejectsLessThanTwoQueues) {
    auto result = configure_rss(0, 1);
    EXPECT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("at least 2"), std::string::npos);
}

TEST(ConfigureRss, RejectsZeroQueues) {
    auto result = configure_rss(0, 0);
    EXPECT_FALSE(result.has_value());
}
