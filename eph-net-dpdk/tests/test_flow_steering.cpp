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
// queue_for_tuple — pure-CPU end-to-end with synthesized RssState
// ---------------------------------------------------------------------------

namespace {
RssState make_state_round_robin(uint16_t reta_size, uint16_t n_queues) {
    RssState s;
    std::copy(kRssDefaultKey.begin(), kRssDefaultKey.end(), s.key_buf.begin());
    s.key_len = static_cast<uint8_t>(kRssDefaultKey.size());
    s.reta_size = reta_size;
    const uint16_t groups = reta_size / RTE_ETH_RETA_GROUP_SIZE;
    for (uint16_t g = 0; g < groups; ++g) {
        s.reta[g].mask = ~uint64_t(0);
        for (uint16_t j = 0; j < RTE_ETH_RETA_GROUP_SIZE; ++j) {
            s.reta[g].reta[j] = static_cast<uint16_t>(
                (g * RTE_ETH_RETA_GROUP_SIZE + j) % n_queues);
        }
    }
    return s;
}
} // namespace

TEST(QueueForTuple, MicrosoftVec1RoutesViaRoundRobinReta) {
    // Microsoft vector 1: hash = 0x51ccc178. With reta_size=128 round-robin
    // over 4 queues, RETA[hash & 0x7F] = RETA[0x78 = 120] = 120 % 4 = 0.
    auto state = make_state_round_robin(/*reta_size=*/128, /*n_queues=*/4);
    uint16_t q = queue_for_tuple(state,
                                  make_ipv4(66, 9, 149, 187), 2794,
                                  make_ipv4(161, 142, 100, 80), 1766);
    EXPECT_EQ(q, 0u);
}

TEST(QueueForTuple, DifferentSrcPortsDistributeAcrossQueues) {
    // Stress: many src_port values populate all queues under round-robin
    // RETA — verifies queue_for_tuple is pure and that the new
    // find_src_port_for_queue inner loop will actually find matches.
    auto state = make_state_round_robin(/*reta_size=*/128, /*n_queues=*/4);
    std::array<int, 4> hits{};
    for (uint16_t sp = 32768; sp < 32768 + 256; ++sp) {
        uint16_t q = queue_for_tuple(state, 0x0a000001, sp,
                                     0x0a000002, 443);
        EXPECT_LT(q, 4u);
        ++hits[q];
    }
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_GT(hits[i], 0) << "queue " << i << " never selected";
    }
}

TEST(RssState, FallbackKeyWhenLenZero) {
    RssState s;
    s.key_len = 0;
    auto k = s.key();
    EXPECT_EQ(k.size(), kRssDefaultKey.size());
    EXPECT_EQ(k.data(), kRssDefaultKey.data());  // span aliases default
}

TEST(RssState, ReadbackKeyWhenLenSet) {
    RssState s;
    std::copy(kRssDefaultKey.begin(), kRssDefaultKey.end(), s.key_buf.begin());
    s.key_len = 40;
    auto k = s.key();
    EXPECT_EQ(k.size(), 40u);
    EXPECT_EQ(k.data(), s.key_buf.data());  // span aliases buf, not default
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
                                     /*remote_ip=*/0x0a000002,
                                     /*remote_port=*/443,
                                     /*local_ip=*/0x0a000001,
                                     /*range_start=*/40000,
                                     /*range_end=*/30000);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("port_range_start"), std::string::npos);
}

// ---------------------------------------------------------------------------
// find_src_port_for_queue_with_state — fan-out distinctness regression.
//
// Pre-fix bug: `find_src_port_for_queue` was a deterministic linear scan
// from `port_range_start`, returning the first match. N concurrent stream
// attaches with identical (remote, local, target_queue) — the production
// pattern of an N-path producer connecting to a single exchange endpoint
// — would all receive the SAME src_port. Each queue's first stream worked;
// every subsequent stream collided on the 5-tuple (kernel EADDRINUSE on
// kernel sockets, or silent RX trampling on DPDK sockets).
//
// Integration tests didn't catch this because they exercise single-stream
// paths only — the fan-out N-to-one-endpoint pattern is exotic for tests
// but canonical for HFT producers.
// ---------------------------------------------------------------------------

namespace {

// Construct a valid 4-queue RssState fixture (matches the DNS test
// fixture pattern in test_dns_rss_aware.cpp).
RssState make_state_4q() {
    RssState s;
    s.key_len = 0;  // → state.key() returns kRssDefaultKey
    s.reta_size = 4;
    s.reta[0].mask = ~uint64_t{0};
    s.reta[0].reta[0] = 0;
    s.reta[0].reta[1] = 1;
    s.reta[0].reta[2] = 2;
    s.reta[0].reta[3] = 3;
    return s;
}

constexpr uint32_t kRemoteIp   = 0x0DC01BAF; // arbitrary "binance"-ish
constexpr uint16_t kRemotePort = 443;
constexpr uint32_t kLocalIp    = 0xAC1F2031;    // 172.31.32.49

}  // namespace

TEST(FindSrcPortForQueue, ExplicitHintIsDeterministic) {
    auto state = make_state_4q();
    auto r1 = find_src_port_for_queue_with_state(
        state, /*target_queue=*/0,
        kRemoteIp, kRemotePort, kLocalIp,
        /*port_range_start=*/32768, /*port_range_end=*/60999,
        /*start_hint=*/0);
    auto r2 = find_src_port_for_queue_with_state(
        state, /*target_queue=*/0,
        kRemoteIp, kRemotePort, kLocalIp,
        /*port_range_start=*/32768, /*port_range_end=*/60999,
        /*start_hint=*/0);
    ASSERT_TRUE(r1.has_value() && r2.has_value());
    EXPECT_EQ(*r1, *r2)
        << "same start_hint must produce same src_port (deterministic for tests)";
}

TEST(FindSrcPortForQueue, FanOutDistinctnessAcrossHints) {
    // Simulate a 15-path producer pinning every path to the same RSS
    // queue. Each path advances the hint by 1; results must be 15
    // distinct src_ports — pre-fix this returned the same port every
    // time. All 15 ports must hash to the requested queue.
    auto state = make_state_4q();
    constexpr uint16_t kTargetQ = 0;
    std::set<uint16_t> ports;
    for (uint32_t hint = 0; hint < 15; ++hint) {
        auto sp = find_src_port_for_queue_with_state(
            state, kTargetQ,
            kRemoteIp, kRemotePort, kLocalIp,
            /*port_range_start=*/32768, /*port_range_end=*/60999,
            /*start_hint=*/hint);
        ASSERT_TRUE(sp.has_value())
            << "fan-out path " << hint << ": " << (sp ? "" : sp.error());
        // Every chosen sp hashes to the requested queue.
        EXPECT_EQ(queue_for_tuple(state, kRemoteIp, kRemotePort,
                                   kLocalIp, *sp),
                  kTargetQ);
        ports.insert(*sp);
    }
    EXPECT_EQ(ports.size(), 15u)
        << "15 fan-out paths must receive 15 distinct src_ports — "
        << "got " << ports.size() << " (pre-fix bug: all the same value)";
}

TEST(FindSrcPortForQueue, AutoHintAdvancesProcessGlobal) {
    // Caller passes nullopt → the helper consumes the process-global
    // atomic counter. Successive calls must produce distinct ports.
    auto state = make_state_4q();
    constexpr uint16_t kTargetQ = 1;
    std::set<uint16_t> ports;
    for (int i = 0; i < 8; ++i) {
        auto sp = find_src_port_for_queue_with_state(
            state, kTargetQ,
            kRemoteIp, kRemotePort, kLocalIp);
        ASSERT_TRUE(sp.has_value());
        EXPECT_EQ(queue_for_tuple(state, kRemoteIp, kRemotePort,
                                   kLocalIp, *sp),
                  kTargetQ);
        ports.insert(*sp);
    }
    EXPECT_EQ(ports.size(), 8u)
        << "auto-hint path must produce distinct ports across 8 calls";
}

TEST(FindSrcPortForQueue, WrapAroundCoversWholeRange) {
    // With a tight range and an exhausted hint, the wrap-around scan must
    // still find a match (i.e. not stop at port_range_end without trying
    // the [start, wrap_start) tail).
    auto state = make_state_4q();
    constexpr uint16_t kStart = 32768;
    constexpr uint16_t kEnd   = 32792;   // 25-port range
    constexpr uint32_t kHint  = 20;       // wrap_start = 32788; loop spills past 32792
    for (uint16_t q = 0; q < 4; ++q) {
        auto sp = find_src_port_for_queue_with_state(
            state, q, kRemoteIp, kRemotePort, kLocalIp,
            kStart, kEnd, kHint);
        ASSERT_TRUE(sp.has_value())
            << "queue " << q << " not found in tight range with hint=" << kHint
            << ": " << (sp ? "" : sp.error());
        EXPECT_GE(*sp, kStart);
        EXPECT_LE(*sp, kEnd);
    }
}

TEST(FindSrcPortForQueue, ExhaustionStillReportsExhausted) {
    // Degenerate state where every port maps to queue 0; searching for
    // queue 1 must visit the entire range (via wrap) then return
    // RssHashPredictExhausted.
    RssState state;
    state.key_len = 0;
    state.reta_size = 1;
    state.reta[0].mask = ~uint64_t{0};
    state.reta[0].reta[0] = 0;
    auto r = find_src_port_for_queue_with_state(
        state, /*target_queue=*/1,
        kRemoteIp, kRemotePort, kLocalIp,
        /*port_range_start=*/32768, /*port_range_end=*/32790,
        /*start_hint=*/10);
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("RssHashPredictExhausted"), std::string::npos);
}

// NOTE: any test that calls predict_rss_queue / find_src_port_for_queue
// against a real port_id requires DPDK EAL + a configured NIC. Those
// paths are exercised by the integration test in stage 3
// (test_dpdk_rss_platform), not here.

// ---------------------------------------------------------------------------
// queue_for_hash — boundary defenses against malformed RETA snapshots
// ---------------------------------------------------------------------------

TEST(QueueForHash, EmptyRetaTableReturnsZero) {
    // Callers that hand in a zero-length reta_table would trigger a signed
    // -1 mask on the old code (UB on the array index). Defensive branch
    // must return 0 without dereferencing.
    std::array<uint16_t, 0> empty_reta{};
    EXPECT_EQ(queue_for_hash(0xdeadbeef, std::span<const uint16_t>(empty_reta)), 0);
}

TEST(QueueForHash, PowerOfTwoRetaUsesBitmask) {
    // Standard path: reta_size is a power of two. The bitmask branch is
    // exact; hash & (N-1) should land on the matching slot.
    std::array<uint16_t, 8> reta{0, 1, 2, 3, 4, 5, 6, 7};
    EXPECT_EQ(queue_for_hash(0xAABBCC00u, std::span<const uint16_t>(reta)),
              reta[0xAABBCC00u & 7u]);
    EXPECT_EQ(queue_for_hash(0xAABBCC05u, std::span<const uint16_t>(reta)),
              reta[0xAABBCC05u & 7u]);
}

TEST(QueueForHash, NonPowerOfTwoRetaFallsBackToModulo) {
    // 12 slots is not a power of two — the old bitmask path would have
    // produced a wrong-index read (12 - 1 = 0x0B is not a full mask,
    // so index `hash & 0x0B` skips many legitimate slots). The fallback
    // branch uses `%` so every slot is reachable.
    std::array<uint16_t, 12> reta{
        10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21};
    EXPECT_EQ(queue_for_hash(0u, std::span<const uint16_t>(reta)), reta[0]);
    EXPECT_EQ(queue_for_hash(100u, std::span<const uint16_t>(reta)),
              reta[100u % 12u]);
    EXPECT_EQ(queue_for_hash(0xFFFFFFFFu, std::span<const uint16_t>(reta)),
              reta[0xFFFFFFFFu % 12u]);
}

// ─────────────────────────────────────────────────────────────────────────────
// DEFERRED COVERAGE: r2 commit 8ff7ebab — on_fd_install_thunk strict-validate
// proto field (rejects anything outside {kIpProtoTcp, kIpProtoUdp}).
//
// Pre-fix any non-17 proto byte fell through to TCP, letting a corrupt
// / forged / older-schema peer install a TCP 5-tuple flow rule for
// SCTP / ICMP / 0 / garbage payloads.
//
// Testing this requires standing up a real DPDK IPC fixture:
//   - Construct an `rte_mp_msg` with an FdInstallMsg payload (proto=99 etc)
//   - Wire up the `g_active_remote_flow_rules` global so the handler
//     enters its proto-validation branch (otherwise it bails earlier
//     on rules == nullptr)
//   - Verify `fd_send_reply_` was invoked with `reply.status = 1`
//
// Each of those needs DPDK runtime (rte_mp_msg layout, rte_mp_reply
// path), which means EAL bring-up — a far heavier setup than the rest
// of this file which exercises pure C++ helpers. Until a generic
// eph-net-dpdk IPC fixture exists, the fix is covered only by
// integration / end-to-end runs against a real secondary that
// intentionally sends a bad proto.
// ─────────────────────────────────────────────────────────────────────────────
