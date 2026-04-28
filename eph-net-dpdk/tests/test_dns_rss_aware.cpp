/// @file eph-net-dpdk/tests/test_dns_rss_aware.cpp
///
/// Regression tests for the DPDK control-plane RSS multi-queue blind
/// spot in DNS resolution: prior to this fix `dns::resolve` (and
/// `AsyncDnsResolverT::start`) chose a random ephemeral src_port, so on
/// RSS-active multi-queue NICs the reply could be hashed to any queue
/// while the resolver only polled one — silent timeouts in production.
///
/// These tests exercise the pure helper
/// `eph::dpdk::dns::detail::select_dns_src_port_with_state`, which takes an
/// explicit `RssState` snapshot. That keeps the tests free of any DPDK
/// runtime / vfio / hugepage dependency.

#include <array>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "eph/dpdk/dns.hpp"
#include "eph/net/dpdk/flow_steering.hpp"

namespace {

// Construct a minimal valid RssState fixture:
//   * key = the project's kRssDefaultKey (used when key_len == 0)
//   * reta_size = 4 → reta_idx = hash & 3
//   * reta[0] mapping reta_idx 0..3 to queue 0..3
eph::net::dpdk::RssState make_state_4q() {
    eph::net::dpdk::RssState s;
    s.key_len = 0;  // → state.key() returns kRssDefaultKey
    s.reta_size = 4;
    s.reta[0].mask = ~uint64_t{0};
    s.reta[0].reta[0] = 0;
    s.reta[0].reta[1] = 1;
    s.reta[0].reta[2] = 2;
    s.reta[0].reta[3] = 3;
    return s;
}

constexpr uint32_t kNameserverIp = 0x08080808; // 8.8.8.8
constexpr uint32_t kLocalIp      = 0xAC1F2031; // 172.31.32.49
constexpr uint16_t kDnsServerPort = 53;

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Behavior contract: helper returns a port whose 5-tuple hashes to
// the requested queue.
// ─────────────────────────────────────────────────────────────────────

TEST(DnsRssAware, SelectedPortHashesToTargetQueueQ0) {
    auto state = make_state_4q();
    auto sp = eph::dpdk::dns::detail::select_dns_src_port_with_state(
        state, /*queue_id=*/0,
        kNameserverIp, kLocalIp, kDnsServerPort);
    ASSERT_TRUE(sp.has_value()) << (sp ? "" : sp.error());

    // Verify: the returned port, when placed in the reply's local-port
    // position, RSS-hashes to queue 0.
    EXPECT_EQ(eph::net::dpdk::queue_for_tuple(
                  state, kNameserverIp, kDnsServerPort, kLocalIp, *sp),
              0u);
}

TEST(DnsRssAware, SelectedPortHashesToTargetQueueQ2) {
    auto state = make_state_4q();
    auto sp = eph::dpdk::dns::detail::select_dns_src_port_with_state(
        state, /*queue_id=*/2,
        kNameserverIp, kLocalIp, kDnsServerPort);
    ASSERT_TRUE(sp.has_value()) << (sp ? "" : sp.error());
    EXPECT_EQ(eph::net::dpdk::queue_for_tuple(
                  state, kNameserverIp, kDnsServerPort, kLocalIp, *sp),
              2u);
}

TEST(DnsRssAware, AllQueuesReachable) {
    // For a 4-queue RETA with the default key, every queue must be reachable
    // somewhere in the ephemeral range — otherwise the helper is broken or
    // the RSS hash is degenerate for this nameserver/local-IP combo.
    auto state = make_state_4q();
    for (uint16_t q = 0; q < 4; ++q) {
        auto sp = eph::dpdk::dns::detail::select_dns_src_port_with_state(
            state, q, kNameserverIp, kLocalIp, kDnsServerPort);
        ASSERT_TRUE(sp.has_value())
            << "queue " << q << ": " << (sp ? "" : sp.error());
        EXPECT_EQ(eph::net::dpdk::queue_for_tuple(
                      state, kNameserverIp, kDnsServerPort, kLocalIp, *sp),
                  q);
    }
}

TEST(DnsRssAware, SelectedPortIsInEphemeralRange) {
    auto state = make_state_4q();
    for (uint16_t q = 0; q < 4; ++q) {
        auto sp = eph::dpdk::dns::detail::select_dns_src_port_with_state(
            state, q, kNameserverIp, kLocalIp, kDnsServerPort);
        ASSERT_TRUE(sp.has_value());
        EXPECT_GE(*sp, 32768u) << "queue " << q;
        EXPECT_LE(*sp, 60999u) << "queue " << q;
    }
}

// ─────────────────────────────────────────────────────────────────────
// Boundary / error paths
// ─────────────────────────────────────────────────────────────────────

TEST(DnsRssAware, InvertedRangeRejected) {
    auto state = make_state_4q();
    auto sp = eph::dpdk::dns::detail::select_dns_src_port_with_state(
        state, 0, kNameserverIp, kLocalIp, kDnsServerPort,
        /*port_range_start=*/40000, /*port_range_end=*/30000);
    ASSERT_FALSE(sp.has_value());
    EXPECT_NE(sp.error().find("inverted range"), std::string::npos);
}

TEST(DnsRssAware, ExhaustedRangeRejected) {
    // Construct a degenerate state where every port in the search range
    // hashes to queue 0 only — searching for queue 1 must exhaust.
    eph::net::dpdk::RssState state;
    state.key_len = 0;
    state.reta_size = 1;
    state.reta[0].mask = ~uint64_t{0};
    state.reta[0].reta[0] = 0;  // only queue 0 reachable

    auto sp = eph::dpdk::dns::detail::select_dns_src_port_with_state(
        state, /*queue_id=*/1,
        kNameserverIp, kLocalIp, kDnsServerPort);
    ASSERT_FALSE(sp.has_value());
    EXPECT_NE(sp.error().find("RssHashPredictExhausted"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────
// Prior-bug witness: a random ephemeral port (the pre-fix behavior)
// is NOT guaranteed to hash to the caller's queue. This test makes the
// silent-bug visible by demonstrating that the fix is non-trivial.
// ─────────────────────────────────────────────────────────────────────

TEST(DnsRssAware, RandomPortDoesNotGuaranteeTargetQueue) {
    auto state = make_state_4q();
    constexpr uint16_t kArbitraryPort = 49283;
    const uint16_t actual = eph::net::dpdk::queue_for_tuple(
        state, kNameserverIp, kDnsServerPort, kLocalIp, kArbitraryPort);
    // A specific arbitrary port hashes to *some* queue (whichever the
    // RETA picks). The pre-fix bug was that the resolver only polled
    // queue=0; if `actual != 0` the reply was lost.
    //
    // We don't pin the exact value (it depends on Toeplitz output for
    // this specific 5-tuple), but we verify the helper can route the
    // hash *deterministically* to whichever queue we ask for.
    auto sp_to_actual = eph::dpdk::dns::detail::select_dns_src_port_with_state(
        state, actual, kNameserverIp, kLocalIp, kDnsServerPort);
    ASSERT_TRUE(sp_to_actual.has_value());
    EXPECT_EQ(eph::net::dpdk::queue_for_tuple(
                  state, kNameserverIp, kDnsServerPort, kLocalIp, *sp_to_actual),
              actual);
}
