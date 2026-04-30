/// @file test_flow_rule_variant.cpp
/// Unit tests for the `FlowRule::handle` variant evolution
/// (reshape mp-icmp-flowdir milestone B stage 5).
///
/// Pure value-type tests — no EAL needed. Cover: variant default
/// state (monostate), valid()/dump()/to_json() dispatch, move
/// transfer, and remove() visit dispatch reaching each branch
/// without crashing on null/zero internals.

#include <cstdint>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "eph/net/dpdk/flow_steering.hpp"

using eph::net::dpdk::FlowHandleVariant;
using eph::net::dpdk::FlowRule;
using eph::net::dpdk::LocalFlowHandle;
using eph::net::dpdk::RemoteFlowHandle;

// ─────────────────────────────────────────────────────────────────────────────
// Variant types — POD layout + equality
// ─────────────────────────────────────────────────────────────────────────────

TEST(FlowHandleVariant, MonostateDefault) {
    FlowRule r;
    EXPECT_FALSE(r.valid());
    EXPECT_FALSE(static_cast<bool>(r));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(r.handle));
}

TEST(FlowHandleVariant, LocalHandleEquality) {
    LocalFlowHandle a{nullptr};
    LocalFlowHandle b{nullptr};
    EXPECT_EQ(a, b);

    auto* fake = reinterpret_cast<rte_flow*>(0xCAFEBABE);
    LocalFlowHandle c{fake};
    EXPECT_NE(a, c);
}

TEST(FlowHandleVariant, RemoteHandleEquality) {
    RemoteFlowHandle a{.owner_proc = 0, .handle_id = 42};
    RemoteFlowHandle b{.owner_proc = 0, .handle_id = 42};
    EXPECT_EQ(a, b);
    RemoteFlowHandle c{.owner_proc = 1, .handle_id = 42};
    EXPECT_NE(a, c);
}

// ─────────────────────────────────────────────────────────────────────────────
// FlowRule::valid / dump / to_json — visit dispatch on each branch
// ─────────────────────────────────────────────────────────────────────────────

TEST(FlowRuleVariant, ValidLocalNonNull) {
    FlowRule r;
    r.port_id  = 0;
    r.queue_id = 3;
    r.handle   = LocalFlowHandle{reinterpret_cast<rte_flow*>(0xCAFE)};
    EXPECT_TRUE(r.valid());
    EXPECT_TRUE(static_cast<bool>(r));

    auto d = r.dump();
    EXPECT_NE(d.find("active"),  std::string::npos);
    EXPECT_NE(d.find("local"),   std::string::npos);
    EXPECT_NE(d.find("queue=3"), std::string::npos);

    auto j = r.to_json();
    EXPECT_NE(j.find("\"active\":true"),    std::string::npos);
    EXPECT_NE(j.find("\"origin\":\"local\""), std::string::npos);
    EXPECT_NE(j.find("\"queue_id\":3"),       std::string::npos);

    // Drop the handle so we don't try rte_flow_destroy on a fake
    // pointer in the dtor.
    r.handle = std::monostate{};
}

TEST(FlowRuleVariant, ValidLocalNullStaysInvalid) {
    FlowRule r;
    r.handle = LocalFlowHandle{nullptr};
    EXPECT_FALSE(r.valid());
    EXPECT_NE(r.dump().find("inactive"), std::string::npos);
    EXPECT_NE(r.to_json().find("\"active\":false"), std::string::npos);
}

TEST(FlowRuleVariant, ValidRemoteNonZero) {
    FlowRule r;
    r.port_id  = 0;
    r.queue_id = 5;
    r.handle   = RemoteFlowHandle{.owner_proc = 0, .handle_id = 0xDEAD};
    EXPECT_TRUE(r.valid());

    auto d = r.dump();
    EXPECT_NE(d.find("remote"),   std::string::npos);
    EXPECT_NE(d.find("owner=0"),  std::string::npos);
    EXPECT_NE(d.find("id=57005"), std::string::npos);

    auto j = r.to_json();
    EXPECT_NE(j.find("\"origin\":\"remote\""), std::string::npos);
}

TEST(FlowRuleVariant, ValidRemoteZeroStaysInvalid) {
    FlowRule r;
    r.handle = RemoteFlowHandle{.owner_proc = 1, .handle_id = 0};
    EXPECT_FALSE(r.valid());
}

TEST(FlowRuleVariant, RemoveReturnsToMonostate) {
    FlowRule r;
    r.handle = RemoteFlowHandle{.owner_proc = 0, .handle_id = 42};
    EXPECT_TRUE(r.valid());
    r.remove();
    EXPECT_FALSE(r.valid());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(r.handle));
}

// ─────────────────────────────────────────────────────────────────────────────
// Move semantics
// ─────────────────────────────────────────────────────────────────────────────

TEST(FlowRuleVariant, MoveCtorTransfersHandle) {
    FlowRule src;
    src.port_id  = 0;
    src.queue_id = 7;
    src.handle   = RemoteFlowHandle{.owner_proc = 1, .handle_id = 100};

    FlowRule dst = std::move(src);
    EXPECT_TRUE(dst.valid());
    EXPECT_EQ(dst.queue_id, 7);
    EXPECT_FALSE(src.valid());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(src.handle));
}

TEST(FlowRuleVariant, MoveAssignReleasesPrev) {
    FlowRule a;
    a.handle = RemoteFlowHandle{.owner_proc = 0, .handle_id = 1};

    FlowRule b;
    b.handle = RemoteFlowHandle{.owner_proc = 1, .handle_id = 2};

    a = std::move(b);
    // a now holds b's handle (owner=1, id=2); b is monostate.
    EXPECT_TRUE(a.valid());
    EXPECT_FALSE(b.valid());
    auto* held = std::get_if<RemoteFlowHandle>(&a.handle);
    ASSERT_NE(held, nullptr);
    EXPECT_EQ(held->owner_proc, 1);
    EXPECT_EQ(held->handle_id, 2u);
}
