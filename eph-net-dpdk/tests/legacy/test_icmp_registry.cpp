/// @file test_icmp_registry.cpp
/// Pure-unit coverage for `eph::dpdk::detail::IcmpRegistry` — the
/// Platform-owned, EAL-independent store for ICMP Frag Needed
/// dispatch targets. Phase 1 (`refactor(net-dpdk)!: unify ICMP
/// dispatch on Platform + drop create(cfg, poller)`) shipped the
/// registry with ~200 lines of new logic but no direct coverage;
/// the integration path needs NIC_B and lives in
/// test_dpdk_icmp_multiqueue (deferred).
///
/// This file tests the state machine in isolation: register /
/// unregister / duplicate rejection / registry-full / dispatch
/// match + miss / RAII handle move + destruction.
///
/// No DPDK EAL, no mempool, no NIC — runs on any dev box.

#include <cstdint>
#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/icmp_registry.hpp"
#include "eph/dpdk/packet_core.hpp"
#include "eph/dpdk/packet_parse.hpp"

using eph::dpdk::detail::IcmpRegistry;
using eph::dpdk::net::ConnectionTuple;
using eph::dpdk::net::ParsedIcmp;
using eph::dpdk::net::kIpProtoTcp;
using eph::dpdk::net::kIpProtoUdp;

namespace {

// Simple mock stream that the registered callback pokes on dispatch.
// Using a global because the MtuCallback is a raw C function pointer
// (noexcept) and cannot capture state. Each TEST resets the globals.
struct StreamMock {
    uint16_t last_mtu = 0;
    int      hits     = 0;
};
StreamMock g_mock_a;
StreamMock g_mock_b;
StreamMock g_mock_c;

void on_mtu_a(void* /*user*/, uint16_t mtu) noexcept {
    g_mock_a.last_mtu = mtu;
    ++g_mock_a.hits;
}
void on_mtu_b(void* /*user*/, uint16_t mtu) noexcept {
    g_mock_b.last_mtu = mtu;
    ++g_mock_b.hits;
}
void on_mtu_c(void* /*user*/, uint16_t mtu) noexcept {
    g_mock_c.last_mtu = mtu;
    ++g_mock_c.hits;
}

void reset_mocks() noexcept {
    g_mock_a = {};
    g_mock_b = {};
    g_mock_c = {};
}

constexpr ConnectionTuple make_tuple(uint32_t src_ip, uint16_t src_port,
                                       uint32_t dst_ip, uint16_t dst_port) noexcept {
    return ConnectionTuple{src_ip, dst_ip, src_port, dst_port};
}

// Build a ParsedIcmp "as if" parsed out of a Type 3 Code 4 frame
// whose embedded IP+TCP header described the given 4-tuple.
ParsedIcmp make_icmp(const ConnectionTuple& t, uint8_t proto,
                     uint16_t next_hop_mtu) noexcept {
    ParsedIcmp p{};
    p.type              = 3;
    p.code              = 4;
    p.next_hop_mtu      = next_hop_mtu;
    p.embedded_src_ip   = t.src_ip;
    p.embedded_dst_ip   = t.dst_ip;
    p.embedded_src_port = t.src_port;
    p.embedded_dst_port = t.dst_port;
    p.embedded_proto    = proto;
    p.embedded_valid    = true;
    return p;
}

constexpr ConnectionTuple kTupleA = {0x0A000001, 0x0A000002, 50000, 443};
constexpr ConnectionTuple kTupleB = {0x0A000003, 0x0A000004, 50001, 8443};
constexpr ConnectionTuple kTupleC = {0x0A000005, 0x0A000006, 50002, 9443};

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Register / unregister basics
// ═══════════════════════════════════════════════════════════════════════

TEST(IcmpRegistry, EmptyRegistryHasSizeZero) {
    IcmpRegistry r;
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(r.dispatched(), 0u);
}

TEST(IcmpRegistry, RegisterReturnsEngagedHandle) {
    IcmpRegistry r;
    reset_mocks();

    auto h = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h.has_value()) << h.error().detail;
    EXPECT_TRUE(h->engaged());
    EXPECT_EQ(r.size(), 1u);
}

TEST(IcmpRegistry, NullStreamOrCallbackRejected) {
    IcmpRegistry r;

    auto h_null_stream = r.register_target(kTupleA, kIpProtoTcp, nullptr, &on_mtu_a);
    ASSERT_FALSE(h_null_stream.has_value());
    EXPECT_EQ(h_null_stream.error().code, eph::core::Error::InvalidConfig);

    auto h_null_cb = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, nullptr);
    ASSERT_FALSE(h_null_cb.has_value());
    EXPECT_EQ(h_null_cb.error().code, eph::core::Error::InvalidConfig);

    EXPECT_EQ(r.size(), 0u);
}

TEST(IcmpRegistry, DuplicateTupleProtoRejected) {
    IcmpRegistry r;
    reset_mocks();

    auto h1 = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h1.has_value());

    auto h2 = r.register_target(kTupleA, kIpProtoTcp, &g_mock_b, &on_mtu_b);
    ASSERT_FALSE(h2.has_value());
    EXPECT_EQ(h2.error().code, eph::core::Error::InvalidConfig);

    // Same tuple but different proto — allowed (TCP vs UDP coexist).
    auto h3 = r.register_target(kTupleA, kIpProtoUdp, &g_mock_c, &on_mtu_c);
    ASSERT_TRUE(h3.has_value()) << h3.error().detail;

    EXPECT_EQ(r.size(), 2u);
}

TEST(IcmpRegistry, RegistryFullReturnsOutOfMemory) {
    IcmpRegistry r;
    reset_mocks();

    // Fill up to kMaxTargets. Each call needs a unique tuple.
    std::vector<IcmpRegistry::Handle> holders;
    holders.reserve(IcmpRegistry::kMaxTargets);
    for (std::size_t i = 0; i < IcmpRegistry::kMaxTargets; ++i) {
        ConnectionTuple t{static_cast<uint32_t>(0x0A000000 + i),
                          0x0A00FFFF,
                          static_cast<uint16_t>(40000 + i),
                          443};
        auto h = r.register_target(t, kIpProtoTcp, &g_mock_a, &on_mtu_a);
        ASSERT_TRUE(h.has_value()) << "i=" << i << ": " << h.error().detail;
        holders.push_back(std::move(*h));
    }
    EXPECT_EQ(r.size(), IcmpRegistry::kMaxTargets);

    // One more — rejected with OutOfMemory.
    ConnectionTuple overflow{0x0BADCAFE, 0x0A00FFFF, 65000, 443};
    auto h = r.register_target(overflow, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_FALSE(h.has_value());
    EXPECT_EQ(h.error().code, eph::core::Error::OutOfMemory);
}

TEST(IcmpRegistry, UnregisterByHandleDestructionShrinksRegistry) {
    IcmpRegistry r;
    reset_mocks();

    {
        auto h = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
        ASSERT_TRUE(h.has_value());
        EXPECT_EQ(r.size(), 1u);
    }  // handle dtor fires unregister
    EXPECT_EQ(r.size(), 0u);
}

TEST(IcmpRegistry, UnregisterUnknownTupleIsNoop) {
    IcmpRegistry r;
    r.unregister(kTupleA, kIpProtoTcp);  // nothing registered; should not crash
    EXPECT_EQ(r.size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// Dispatch matching
// ═══════════════════════════════════════════════════════════════════════

TEST(IcmpRegistry, DispatchHitsRegisteredTarget) {
    IcmpRegistry r;
    reset_mocks();

    auto h = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h.has_value());

    r.dispatch(make_icmp(kTupleA, kIpProtoTcp, 1400));

    EXPECT_EQ(g_mock_a.hits, 1);
    EXPECT_EQ(g_mock_a.last_mtu, 1400u);
    EXPECT_EQ(r.dispatched(), 1u);
}

TEST(IcmpRegistry, DispatchWithUnmatchedTupleIsNoop) {
    IcmpRegistry r;
    reset_mocks();

    auto h = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h.has_value());

    // Same src_ip / dst_ip but different src_port — should not match.
    ConnectionTuple wrong_port = kTupleA;
    wrong_port.src_port = 65535;
    r.dispatch(make_icmp(wrong_port, kIpProtoTcp, 1200));

    EXPECT_EQ(g_mock_a.hits, 0);
    EXPECT_EQ(r.dispatched(), 0u);
}

TEST(IcmpRegistry, DispatchWithDifferentProtoIsNoop) {
    IcmpRegistry r;
    reset_mocks();

    auto h = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h.has_value());

    // TCP target registered; UDP packet with same 4-tuple must NOT match.
    r.dispatch(make_icmp(kTupleA, kIpProtoUdp, 1400));

    EXPECT_EQ(g_mock_a.hits, 0);
    EXPECT_EQ(r.dispatched(), 0u);
}

TEST(IcmpRegistry, DispatchWithEmbeddedInvalidIsNoop) {
    IcmpRegistry r;
    reset_mocks();

    auto h = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h.has_value());

    // Simulate a poorly-formed ICMP whose embedded header didn't parse.
    ParsedIcmp p = make_icmp(kTupleA, kIpProtoTcp, 1400);
    p.embedded_valid = false;
    r.dispatch(p);

    EXPECT_EQ(g_mock_a.hits, 0);
    EXPECT_EQ(r.dispatched(), 0u);
}

TEST(IcmpRegistry, DispatchRoutesToCorrectTargetAmongMany) {
    IcmpRegistry r;
    reset_mocks();

    auto ha = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    auto hb = r.register_target(kTupleB, kIpProtoTcp, &g_mock_b, &on_mtu_b);
    auto hc = r.register_target(kTupleC, kIpProtoUdp, &g_mock_c, &on_mtu_c);
    ASSERT_TRUE(ha.has_value());
    ASSERT_TRUE(hb.has_value());
    ASSERT_TRUE(hc.has_value());

    r.dispatch(make_icmp(kTupleB, kIpProtoTcp, 1450));

    EXPECT_EQ(g_mock_a.hits, 0);
    EXPECT_EQ(g_mock_b.hits, 1);
    EXPECT_EQ(g_mock_c.hits, 0);
    EXPECT_EQ(g_mock_b.last_mtu, 1450u);
    EXPECT_EQ(r.dispatched(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
// RAII Handle semantics
// ═══════════════════════════════════════════════════════════════════════

TEST(IcmpRegistryHandle, DefaultConstructedIsDisengaged) {
    IcmpRegistry::Handle h;
    EXPECT_FALSE(h.engaged());
    // Destruction must be a no-op (no registry to unregister from).
}

TEST(IcmpRegistryHandle, MoveTransfersEngagementAndOldIsDisengaged) {
    IcmpRegistry r;
    reset_mocks();

    auto h1_or = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h1_or.has_value());
    IcmpRegistry::Handle h1 = std::move(*h1_or);
    EXPECT_TRUE(h1.engaged());
    EXPECT_EQ(r.size(), 1u);

    IcmpRegistry::Handle h2 = std::move(h1);
    EXPECT_TRUE(h2.engaged());
    EXPECT_FALSE(h1.engaged());
    EXPECT_EQ(r.size(), 1u);  // still registered

    // Destroy h2 → unregister
}

TEST(IcmpRegistryHandle, MoveAssignmentUnregistersOldTarget) {
    IcmpRegistry r;
    reset_mocks();

    auto h1_or = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    auto h2_or = r.register_target(kTupleB, kIpProtoTcp, &g_mock_b, &on_mtu_b);
    ASSERT_TRUE(h1_or.has_value());
    ASSERT_TRUE(h2_or.has_value());
    EXPECT_EQ(r.size(), 2u);

    IcmpRegistry::Handle h1 = std::move(*h1_or);
    IcmpRegistry::Handle h2 = std::move(*h2_or);

    // Move-assigning h2 onto h1 must unregister the A-target (h1's prev)
    // and take ownership of the B-target (h2's).
    h1 = std::move(h2);
    EXPECT_TRUE(h1.engaged());
    EXPECT_FALSE(h2.engaged());
    EXPECT_EQ(r.size(), 1u);  // A gone, B remains

    // Verify by dispatching — A should miss, B should hit.
    r.dispatch(make_icmp(kTupleA, kIpProtoTcp, 1400));
    r.dispatch(make_icmp(kTupleB, kIpProtoTcp, 1500));
    EXPECT_EQ(g_mock_a.hits, 0);
    EXPECT_EQ(g_mock_b.hits, 1);
}

TEST(IcmpRegistryHandle, SelfMoveAssignIsSafe) {
    IcmpRegistry r;
    reset_mocks();

    auto h_or = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    ASSERT_TRUE(h_or.has_value());
    IcmpRegistry::Handle h = std::move(*h_or);

    // Self-move-assign — must not unregister.
    h = std::move(h);
    EXPECT_TRUE(h.engaged());
    EXPECT_EQ(r.size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
// Re-register after unregister
// ═══════════════════════════════════════════════════════════════════════

TEST(IcmpRegistry, ReRegisterAfterUnregisterSucceeds) {
    IcmpRegistry r;
    reset_mocks();

    {
        auto h = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
        ASSERT_TRUE(h.has_value());
        EXPECT_EQ(r.size(), 1u);
    }  // unregister via dtor
    EXPECT_EQ(r.size(), 0u);

    // The slot should be reusable for a fresh registration with the
    // same tuple. Verifies swap-with-last removal fully clears the
    // slot (otherwise duplicate-detection might wrongly flag it).
    auto h2 = r.register_target(kTupleA, kIpProtoTcp, &g_mock_b, &on_mtu_b);
    ASSERT_TRUE(h2.has_value()) << h2.error().detail;

    r.dispatch(make_icmp(kTupleA, kIpProtoTcp, 1200));
    EXPECT_EQ(g_mock_a.hits, 0) << "stale entry should not fire";
    EXPECT_EQ(g_mock_b.hits, 1);
}

TEST(IcmpRegistry, SwapWithLastCompactionPreservesOtherEntries) {
    IcmpRegistry r;
    reset_mocks();

    auto ha_or = r.register_target(kTupleA, kIpProtoTcp, &g_mock_a, &on_mtu_a);
    auto hb_or = r.register_target(kTupleB, kIpProtoTcp, &g_mock_b, &on_mtu_b);
    auto hc_or = r.register_target(kTupleC, kIpProtoTcp, &g_mock_c, &on_mtu_c);
    ASSERT_TRUE(ha_or.has_value());
    ASSERT_TRUE(hb_or.has_value());
    ASSERT_TRUE(hc_or.has_value());

    // Drop the middle one (B) — swap-with-last should move C into B's slot.
    {
        IcmpRegistry::Handle hb = std::move(*hb_or);
        (void)hb;  // unregister at scope end
    }
    EXPECT_EQ(r.size(), 2u);

    // A and C must still dispatch correctly.
    r.dispatch(make_icmp(kTupleA, kIpProtoTcp, 1400));
    r.dispatch(make_icmp(kTupleC, kIpProtoTcp, 1500));
    r.dispatch(make_icmp(kTupleB, kIpProtoTcp, 9999));  // unregistered — noop

    EXPECT_EQ(g_mock_a.hits, 1);
    EXPECT_EQ(g_mock_b.hits, 0);
    EXPECT_EQ(g_mock_c.hits, 1);
    EXPECT_EQ(g_mock_a.last_mtu, 1400u);
    EXPECT_EQ(g_mock_c.last_mtu, 1500u);
}
