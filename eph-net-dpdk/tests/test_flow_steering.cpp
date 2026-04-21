/// @file test_flow_steering.cpp
/// Unit tests for flow_steering.hpp: RxDispatchMode, FlowRule lifecycle,
/// RSS configuration helpers.
///
/// NOTE: Actual NIC hardware detection requires DPDK EAL + real NIC.
/// These tests cover:
///   - Enum names and values
///   - FlowRule RAII semantics (move, default state)
///   - configure_rss validation paths that don't need a NIC

#include <format>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/net/dpdk/flow_steering.hpp"

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
    rule.handle = reinterpret_cast<rte_flow*>(0xDEAD);
    auto json = rule.to_json();
    EXPECT_NE(json.find("\"active\":true"), std::string::npos);
    EXPECT_NE(json.find("\"port_id\":2"), std::string::npos);
    EXPECT_NE(json.find("\"queue_id\":5"), std::string::npos);
    // Prevent destructor from calling rte_flow_destroy on fake pointer
    rule.handle = nullptr;
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

// ---------------------------------------------------------------------------
// kRssDefaultKey
// ---------------------------------------------------------------------------

TEST(RssDefaultKey, FortyBytes) {
    EXPECT_EQ(kRssDefaultKey.size(), 40u);
}

TEST(RssDefaultKey, MicrosoftFirstByte) {
    // Sanity: Microsoft / DPDK default key starts with 0x6D 0x5A 0x56 0xDA
    EXPECT_EQ(kRssDefaultKey[0], 0x6D);
    EXPECT_EQ(kRssDefaultKey[1], 0x5A);
    EXPECT_EQ(kRssDefaultKey[2], 0x56);
    EXPECT_EQ(kRssDefaultKey[3], 0xDA);
}

// ---------------------------------------------------------------------------
// toeplitz_hash — Microsoft RSS verification vectors (IPv4 + TCP, full L3+L4)
// Source: DPDK lib/eal test_thash.c (which itself follows Microsoft's
// "Verifying the RSS Hash Calculation" spec).
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t make_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t(a) << 24) | (uint32_t(b) << 16) |
           (uint32_t(c) <<  8) |  uint32_t(d);
}
}

TEST(ToeplitzHash, MicrosoftVector1_IPv4Tcp) {
    // src=66.9.149.187:2794, dst=161.142.100.80:1766 → 0x51ccc178
    const uint32_t h = toeplitz_hash_ipv4(
        std::span(kRssDefaultKey),
        make_ipv4(66, 9, 149, 187), 2794,
        make_ipv4(161, 142, 100, 80), 1766);
    EXPECT_EQ(h, 0x51ccc178u);
}

TEST(ToeplitzHash, MicrosoftVector2_IPv4Tcp) {
    // src=199.92.111.2:14230, dst=65.69.140.83:4739 → 0xc626b0ea
    const uint32_t h = toeplitz_hash_ipv4(
        std::span(kRssDefaultKey),
        make_ipv4(199, 92, 111, 2), 14230,
        make_ipv4(65, 69, 140, 83), 4739);
    EXPECT_EQ(h, 0xc626b0eau);
}

TEST(ToeplitzHash, MicrosoftVector3_IPv4Tcp) {
    // src=24.19.198.95:12898, dst=12.22.207.184:38024 → 0x5c2b394a
    const uint32_t h = toeplitz_hash_ipv4(
        std::span(kRssDefaultKey),
        make_ipv4(24, 19, 198, 95), 12898,
        make_ipv4(12, 22, 207, 184), 38024);
    EXPECT_EQ(h, 0x5c2b394au);
}

TEST(ToeplitzHash, MicrosoftVector4_IPv4Tcp) {
    // src=38.27.205.30:48228, dst=209.142.163.6:2217 → 0xafc7327f
    const uint32_t h = toeplitz_hash_ipv4(
        std::span(kRssDefaultKey),
        make_ipv4(38, 27, 205, 30), 48228,
        make_ipv4(209, 142, 163, 6), 2217);
    EXPECT_EQ(h, 0xafc7327fu);
}

TEST(ToeplitzHash, MicrosoftVector5_IPv4Tcp) {
    // src=153.39.163.191:44251, dst=202.188.127.2:1303 → 0x10e828a2
    const uint32_t h = toeplitz_hash_ipv4(
        std::span(kRssDefaultKey),
        make_ipv4(153, 39, 163, 191), 44251,
        make_ipv4(202, 188, 127, 2), 1303);
    EXPECT_EQ(h, 0x10e828a2u);
}

TEST(ToeplitzHash, Deterministic) {
    auto h1 = toeplitz_hash_ipv4(std::span(kRssDefaultKey),
                                  0x0a000001, 12345, 0x0a000002, 443);
    auto h2 = toeplitz_hash_ipv4(std::span(kRssDefaultKey),
                                  0x0a000001, 12345, 0x0a000002, 443);
    EXPECT_EQ(h1, h2);
}

TEST(ToeplitzHash, DifferentSrcPortDifferentHash) {
    auto h1 = toeplitz_hash_ipv4(std::span(kRssDefaultKey),
                                  0x0a000001, 12345, 0x0a000002, 443);
    auto h2 = toeplitz_hash_ipv4(std::span(kRssDefaultKey),
                                  0x0a000001, 12346, 0x0a000002, 443);
    EXPECT_NE(h1, h2);
}

TEST(ToeplitzHash, EmptyInputReturnsZero) {
    std::array<uint8_t, 0> empty{};
    auto h = toeplitz_hash(std::span(kRssDefaultKey),
                           std::span<const uint8_t>(empty));
    EXPECT_EQ(h, 0u);
}

// ---------------------------------------------------------------------------
// queue_for_hash
// ---------------------------------------------------------------------------

TEST(QueueForHash, MapsByLowBits) {
    // 4-queue RETA distributing round-robin
    std::array<uint16_t, 4> reta{0, 1, 2, 3};
    EXPECT_EQ(queue_for_hash(0u,           reta), 0u);
    EXPECT_EQ(queue_for_hash(0xFFFFFFFCu,  reta), 0u);
    EXPECT_EQ(queue_for_hash(0x12345671u,  reta), 1u);
    EXPECT_EQ(queue_for_hash(0xFFFFFFFFu,  reta), 3u);
}

TEST(QueueForHash, RoundRobin8Queues) {
    std::array<uint16_t, 8> reta{0, 1, 2, 3, 4, 5, 6, 7};
    for (uint32_t h = 0; h < 16; ++h) {
        EXPECT_EQ(queue_for_hash(h, reta), h & 7u)
            << "hash " << h;
    }
}

// ---------------------------------------------------------------------------
// find_src_port_for_queue — validation paths (NIC-independent)
// ---------------------------------------------------------------------------

TEST(FindSrcPortForQueue, RejectsInvertedRange) {
    auto r = find_src_port_for_queue(/*port_id=*/0, /*target=*/0,
                                     /*src_ip=*/0x0a000001,
                                     /*dst_ip=*/0x0a000002, /*dst_port=*/443,
                                     /*range_start=*/40000,
                                     /*range_end=*/30000);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("port_range_start"), std::string::npos);
}

// NOTE: any test that calls predict_rss_queue / find_src_port_for_queue
// against a real port_id requires DPDK EAL + a configured NIC. Those
// paths are exercised by the integration test in stage 3
// (test_dpdk_rss_platform), not here.
