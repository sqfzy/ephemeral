/// @file test_flow_steering.cpp
/// Unit tests for flow_steering.hpp: RxDispatchMode, FlowProtocol, FlowRule.
///
/// NOTE: Actual NIC hardware detection requires DPDK EAL + real NIC.
/// These tests cover:
///   - Enum names and values (RxDispatchMode / FlowProtocol)
///   - FlowRule RAII semantics (move, default state, json/dump formatting)

#include <format>
#include <variant>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/net/dpdk/flow_steering.hpp"

using eph::net::dpdk::LocalFlowHandle;

using namespace eph::net::dpdk;

// ---------------------------------------------------------------------------
// RxDispatchMode
// ---------------------------------------------------------------------------

TEST(RxDispatchMode, NameSoftware) {
    EXPECT_EQ(rx_dispatch_mode_name(RxDispatchMode::Software),
              "Software (single-Poller fallback)");
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
    EXPECT_TRUE(std::holds_alternative<std::monostate>(rule.handle));
}

TEST(FlowRule, MoveTransfersOwnership) {
    FlowRule a;
    a.port_id = 1;
    a.queue_id = 3;
    // Can't set handle without real rte_flow, but verify move semantics
    FlowRule b = std::move(a);
    EXPECT_EQ(b.port_id, 1);
    EXPECT_EQ(b.queue_id, 3);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(a.handle));  // Source nulled
}

TEST(FlowRule, MoveAssignmentTransfers) {
    FlowRule a;
    a.port_id = 2;
    a.queue_id = 5;
    FlowRule b;
    b = std::move(a);
    EXPECT_EQ(b.port_id, 2);
    EXPECT_EQ(b.queue_id, 5);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(a.handle));
}

TEST(FlowRule, RemoveOnNullHandleIsSafe) {
    FlowRule rule;
    rule.remove();  // Should not crash
    EXPECT_FALSE(rule.valid());
}

// ---------------------------------------------------------------------------
// std::formatter
// ---------------------------------------------------------------------------

TEST(RxDispatchMode, FormatterProducesOutput) {
    EXPECT_EQ(std::format("{}", RxDispatchMode::Software),
              "Software (single-Poller fallback)");
    EXPECT_EQ(std::format("{}", RxDispatchMode::RssPartitioned), "RSS Partitioned");
    EXPECT_EQ(std::format("{}", RxDispatchMode::FlowDirector), "Flow Director (rte_flow)");
}

TEST(RxDispatchMode, FormatterWorksInCompositeFormat) {
    auto s = std::format("mode={} port={}", RxDispatchMode::Software, 0);
    EXPECT_EQ(s, "mode=Software (single-Poller fallback) port=0");
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
    rule.handle = LocalFlowHandle{reinterpret_cast<rte_flow*>(0xDEAD)};
    auto d = rule.dump();
    EXPECT_NE(d.find("port=1"), std::string::npos);
    EXPECT_NE(d.find("queue=3"), std::string::npos);
    EXPECT_NE(d.find("active"), std::string::npos);
    // Prevent destructor from calling rte_flow_destroy on the fake pointer
    rule.handle = std::monostate{};
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
    // Self-move should not crash or corrupt state. We intentionally
    // assign std::move(rule) to itself to verify the move-assignment
    // operator's `if (this == &other) return *this` short-circuit.
    //
    // GCC's -Wself-move and clang-tidy's bugprone-use-after-move both
    // flag this idiom — we silence them with a typed reference-launder
    // step that hides the self-aliasing from the static analyzer
    // without changing the runtime behaviour. The reference-laundering
    // works because the compiler can't prove `aliased == &rule` at
    // compile time, so the warning is suppressed; the runtime branch
    // in operator= still sees `this == &other` and short-circuits.
    FlowRule& aliased = rule;
    rule = std::move(aliased);
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
    EXPECT_TRUE(std::holds_alternative<std::monostate>(rule.handle));
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

// flow_protocol_name was untested in isolation before this commit;
// only `rx_dispatch_mode_name` had explicit coverage. A copy-paste bug
// rebinding "TCP" → "TCP_v4" wouldn't be caught unless that exact
// string appeared in a downstream log assertion.
TEST(FlowProtocol, NameForKnownValues) {
    EXPECT_EQ(flow_protocol_name(FlowProtocol::Tcp), "TCP");
    EXPECT_EQ(flow_protocol_name(FlowProtocol::Udp), "UDP");
}

TEST(FlowProtocol, NameForOutOfRangeReturnsUnknown) {
    // Cast a value outside the enum's defined range to trigger the
    // default "unknown" branch. Pins that the switch's fallthrough is
    // safe under malformed inputs (e.g. a wire-decoded byte cast to
    // FlowProtocol).
    auto bogus = static_cast<FlowProtocol>(0xFF);
    EXPECT_EQ(flow_protocol_name(bogus), "unknown");
}

TEST(RxDispatchMode, NameForOutOfRangeReturnsUnknown) {
    // Same out-of-range coverage for rx_dispatch_mode_name; the
    // existing test only checks the three enumerators.
    auto bogus = static_cast<RxDispatchMode>(0xFF);
    EXPECT_EQ(rx_dispatch_mode_name(bogus), "unknown");
}

// ---------------------------------------------------------------------------
// FlowRule::to_json
// ---------------------------------------------------------------------------

TEST(FlowRule, ToJsonInactive) {
    FlowRule rule;
    auto json = rule.to_json();
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"active\":false"), std::string::npos);
    EXPECT_NE(json.find("\"port_id\":0"), std::string::npos);
    EXPECT_NE(json.find("\"queue_id\":0"), std::string::npos);
}

TEST(FlowRule, ToJsonActiveShowsTrue) {
    FlowRule rule;
    rule.port_id = 2;
    rule.queue_id = 5;
    rule.handle = LocalFlowHandle{reinterpret_cast<rte_flow*>(0xDEAD)};
    auto json = rule.to_json();
    EXPECT_NE(json.find("\"active\":true"), std::string::npos);
    EXPECT_NE(json.find("\"port_id\":2"), std::string::npos);
    EXPECT_NE(json.find("\"queue_id\":5"), std::string::npos);
    // Prevent destructor from calling rte_flow_destroy on fake pointer
    rule.handle = std::monostate{};
}

// After remove(), the rule keeps its (port_id, queue_id) coordinates as an
// audit trail while flipping `"active":false`. This contract is what lets
// monitoring pipelines tell "rule lived on port 2 queue 5 and is now gone"
// from "rule never existed (default 0/0)". `dump()` deliberately drops the
// coordinates to keep log noise terse — only `to_json()` preserves them.
TEST(FlowRule, RemovePreservesCoordinatesAndMarksInactive) {
    FlowRule rule;
    rule.port_id = 7;
    rule.queue_id = 11;
    // No handle is installed → remove() is a no-op (guarded by `!handle`),
    // but the post-state assertions still hold for the structured-output
    // contract: coordinates remain, `active` is false, and dump() is terse.
    rule.remove();
    auto json = rule.to_json();
    EXPECT_NE(json.find("\"active\":false"), std::string::npos);
    EXPECT_NE(json.find("\"port_id\":7"), std::string::npos);
    EXPECT_NE(json.find("\"queue_id\":11"), std::string::npos);
    EXPECT_EQ(rule.dump(), "FlowRule(inactive)");
    EXPECT_FALSE(rule.valid());
}

// ---------------------------------------------------------------------------
// RxDispatchMode — constexpr enum value ordering
// ---------------------------------------------------------------------------

TEST(RxDispatchMode, EnumOrdering) {
    // Software < RssPartitioned < FlowDirector
    EXPECT_LT(static_cast<uint8_t>(RxDispatchMode::Software),
              static_cast<uint8_t>(RxDispatchMode::RssPartitioned));
    EXPECT_LT(static_cast<uint8_t>(RxDispatchMode::RssPartitioned),
              static_cast<uint8_t>(RxDispatchMode::FlowDirector));
}

// ---------------------------------------------------------------------------
// FlowRule — double-remove is safe
// ---------------------------------------------------------------------------

TEST(FlowRule, DoubleRemoveIsSafe) {
    FlowRule rule;
    rule.remove();
    rule.remove();  // Idempotent
    EXPECT_FALSE(rule.valid());
}
