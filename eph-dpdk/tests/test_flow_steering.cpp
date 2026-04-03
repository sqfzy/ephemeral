/// @file test_flow_steering.cpp
/// Unit tests for flow_steering.hpp: RxDispatchMode, FlowRule lifecycle,
/// RSS configuration helpers.
///
/// NOTE: Actual NIC hardware detection requires DPDK EAL + real NIC.
/// These tests cover:
///   - Enum names and values
///   - FlowRule RAII semantics (move, default state)
///   - detect_rx_dispatch_mode return value semantics

#include <format>

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

// ---------------------------------------------------------------------------
// std::formatter
// ---------------------------------------------------------------------------

TEST(RxDispatchMode, FormatterProducesOutput) {
    EXPECT_EQ(std::format("{}", RxDispatchMode::Software), "Software (Reactor)");
    EXPECT_EQ(std::format("{}", RxDispatchMode::RssPartitioned), "RSS Partitioned");
    EXPECT_EQ(std::format("{}", RxDispatchMode::FlowDirector), "Flow Director (rte_flow)");
}

TEST(RxDispatchMode, FormatterWorksInCompositeFormat) {
    auto s = std::format("mode={} port={}", RxDispatchMode::Software, 0);
    EXPECT_EQ(s, "mode=Software (Reactor) port=0");
}

// ---------------------------------------------------------------------------
// FlowRule dump and formatter
// ---------------------------------------------------------------------------

TEST(FlowRule, DumpInactive) {
    FlowRule rule;
    EXPECT_EQ(rule.dump(), "FlowRule(inactive)");
}

TEST(FlowRule, DumpActiveShowsPortAndQueue) {
    FlowRule rule;
    rule.port_id = 1;
    rule.queue_id = 3;
    // Simulate an active rule (non-null handle). We use a fake pointer
    // because we can't create a real rte_flow without DPDK EAL.
    rule.handle = reinterpret_cast<rte_flow*>(0xDEAD);
    auto d = rule.dump();
    EXPECT_NE(d.find("port=1"), std::string::npos);
    EXPECT_NE(d.find("queue=3"), std::string::npos);
    EXPECT_NE(d.find("active"), std::string::npos);
    // Prevent destructor from calling rte_flow_destroy on the fake pointer
    rule.handle = nullptr;
}

TEST(FlowRule, FormatterProducesOutput) {
    FlowRule rule;
    auto s = std::format("{}", rule);
    EXPECT_NE(s.find("inactive"), std::string::npos);
}

TEST(FlowRule, ExplicitBoolConversion) {
    FlowRule rule;
    EXPECT_FALSE(static_cast<bool>(rule));
    // Verify explicit conversion is required (not implicit)
    static_assert(!std::is_convertible_v<FlowRule, bool>,
                  "FlowRule's bool conversion must be explicit");
    static_assert(requires(const FlowRule& r) { static_cast<bool>(r); },
                  "FlowRule must support explicit bool conversion");
}

TEST(FlowRule, SelfMoveAssignmentIsSafe) {
    FlowRule rule;
    rule.port_id = 5;
    rule.queue_id = 7;
    // Self-move should not crash or corrupt state
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-diagnostic-self-move)
    rule = std::move(rule);
    EXPECT_EQ(rule.port_id, 5);
    EXPECT_EQ(rule.queue_id, 7);
}

TEST(FlowRule, TypeTraits) {
    // FlowRule is RAII — not copyable, only movable
    static_assert(!std::is_copy_constructible_v<FlowRule>);
    static_assert(!std::is_copy_assignable_v<FlowRule>);
    static_assert(std::is_move_constructible_v<FlowRule>);
    static_assert(std::is_move_assignable_v<FlowRule>);
    static_assert(std::is_nothrow_move_constructible_v<FlowRule>);
    static_assert(std::is_nothrow_move_assignable_v<FlowRule>);
}

TEST(FlowRule, DefaultValues) {
    FlowRule rule;
    EXPECT_EQ(rule.port_id, 0);
    EXPECT_EQ(rule.queue_id, 0);
    EXPECT_EQ(rule.handle, nullptr);
    EXPECT_FALSE(rule.valid());
}

TEST(RxDispatchMode, AllEnumValuesHaveNames) {
    // Verify all enum values produce non-empty, non-"unknown" names
    EXPECT_NE(rx_dispatch_mode_name(RxDispatchMode::Software), "unknown");
    EXPECT_NE(rx_dispatch_mode_name(RxDispatchMode::RssPartitioned), "unknown");
    EXPECT_NE(rx_dispatch_mode_name(RxDispatchMode::FlowDirector), "unknown");
    // Names should not be empty
    EXPECT_FALSE(rx_dispatch_mode_name(RxDispatchMode::Software).empty());
    EXPECT_FALSE(rx_dispatch_mode_name(RxDispatchMode::RssPartitioned).empty());
    EXPECT_FALSE(rx_dispatch_mode_name(RxDispatchMode::FlowDirector).empty());
}

TEST(ConfigureRss, OneQueueReturnsDescriptiveError) {
    auto result = configure_rss(0, 1);
    ASSERT_FALSE(result.has_value());
    // Verify the error message is helpful
    EXPECT_NE(result.error().find("2"), std::string::npos);
}
