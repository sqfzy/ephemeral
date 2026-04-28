/// @file eph-net-dpdk/tests/test_dpdk_udp_multicast_rss.cpp
///
/// Regression tests for stage 5 of the DPDK control-plane RSS multi-queue
/// blind-spot fix: `MulticastReceiver::start()` now fail-fasts when
/// `MulticastConfig::rss_active_multi_queue == true`, because the
/// inbound multicast 5-tuple is RSS-hashed across queues and the
/// receiver only polls one `rx_queue_id`.
///
/// These tests don't touch DPDK runtime — they exercise the start()
/// pre-check by constructing the receiver with no joined groups and no
/// callback, which still progresses far enough to hit the RSS safety
/// gate before bailing on those (unrelated) preconditions.

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "eph/dpdk/multicast.hpp"

namespace {

eph::dpdk::MulticastConfig make_cfg(uint16_t rx_queue_id, bool rss_active) {
    eph::dpdk::MulticastConfig cfg;
    cfg.port_id = 0;
    cfg.rx_queue_id = rx_queue_id;
    cfg.rx_burst = 32;
    cfg.rx_cpu = -1;
    cfg.rss_active_multi_queue = rss_active;
    return cfg;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Fail-fast: RSS multi-queue + any rx_queue_id is unsafe.
// ─────────────────────────────────────────────────────────────────────

TEST(MulticastRssCheck, RssActiveMultiQueueRejectedAtQueue0) {
    eph::dpdk::MulticastReceiver rx(make_cfg(/*rx_queue_id=*/0, /*rss_active=*/true));
    auto r = rx.start();
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("rss_active_multi_queue=true"), std::string::npos)
        << r.error();
}

TEST(MulticastRssCheck, RssActiveMultiQueueRejectedAtQueueN) {
    eph::dpdk::MulticastReceiver rx(make_cfg(/*rx_queue_id=*/3, /*rss_active=*/true));
    auto r = rx.start();
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("unsafe"), std::string::npos) << r.error();
    EXPECT_NE(r.error().find("rx_queue_id=3"), std::string::npos)
        << "error must surface the offending rx_queue_id: " << r.error();
}

// ─────────────────────────────────────────────────────────────────────
// Single-queue / non-RSS path: the RSS gate is silent, start() proceeds
// to its other pre-checks (no groups joined → fails for an unrelated
// reason). We assert the failure is NOT the RSS-safety message — that's
// the contract: the gate only fires when rss_active_multi_queue=true.
// ─────────────────────────────────────────────────────────────────────

TEST(MulticastRssCheck, NonRssPathSkipsSafetyGate) {
    eph::dpdk::MulticastReceiver rx(make_cfg(/*rx_queue_id=*/2, /*rss_active=*/false));
    auto r = rx.start();
    ASSERT_FALSE(r.has_value());  // fails for "no groups joined" / "no callback"
    EXPECT_EQ(r.error().find("rss_active_multi_queue"), std::string::npos)
        << "with rss_active_multi_queue=false the RSS gate must not fire; "
        << "actual error: " << r.error();
}

TEST(MulticastRssCheck, RssActiveFlagIsConfigDefaultFalse) {
    // Defensive: the default-constructed MulticastConfig must not
    // flip behavior. Pre-fix code paths that don't set the new flag
    // continue to work.
    eph::dpdk::MulticastConfig cfg;  // all defaults
    EXPECT_FALSE(cfg.rss_active_multi_queue)
        << "default MulticastConfig must keep rss_active_multi_queue=false "
        << "for backward-compat";
}
