/// @file test_fd_ipc_handlers.cpp
/// Unit tests for `detail::RemoteFlowRulesMap` — primary-side
/// storage of FlowDir rules installed on behalf of secondaries via
/// the eph_fd_install / eph_fd_destroy IPC handlers (reshape
/// mp-icmp-flowdir milestone B stage 6).
///
/// Pure storage tests with `rte_flow*` faked as opaque pointers.
/// `destroy_by_id` and `destroy_all` end up calling
/// `rte_flow_destroy(port=0, fake_ptr, ...)` which fails cleanly
/// under the --no-pci EAL fixture (no port to operate on) — fine
/// for a unit-level smoke test of the map bookkeeping; full IPC
/// handler coverage including `install_flow_rule` round-trip
/// requires a real NIC and is exercised in stage 7's e2e.

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp"

#include "eph/net/dpdk/flow_steering.hpp"

using eph::net::dpdk::detail::RemoteFlowRulesMap;

namespace {

rte_flow* fake_flow(uintptr_t bits) {
    return reinterpret_cast<rte_flow*>(bits);
}

} // namespace

TEST(RemoteFlowRulesMap, InsertNullptrReturnsZero) {
    RemoteFlowRulesMap m;
    EXPECT_EQ(m.insert(/*port=*/0, nullptr), 0u);
    EXPECT_EQ(m.size_for_test(), 0u);
}

TEST(RemoteFlowRulesMap, InsertIncrementsId) {
    RemoteFlowRulesMap m;
    auto id1 = m.insert(0, fake_flow(0xCAFE));
    auto id2 = m.insert(0, fake_flow(0xBEEF));
    auto id3 = m.insert(1, fake_flow(0xF00D));
    EXPECT_GT(id1, 0u);
    EXPECT_GT(id2, id1);
    EXPECT_GT(id3, id2);
    EXPECT_EQ(m.size_for_test(), 3u);

    // Drain so dtor doesn't call rte_flow_destroy on the fakes a
    // second time later if the fixture decides to reset.
    (void)m.destroy_by_id(id1);
    (void)m.destroy_by_id(id2);
    (void)m.destroy_by_id(id3);
}

TEST(RemoteFlowRulesMap, DestroyByUnknownIdReturnsFalse) {
    RemoteFlowRulesMap m;
    EXPECT_FALSE(m.destroy_by_id(99999));
}

TEST(RemoteFlowRulesMap, DestroyByKnownIdRemovesEntry) {
    RemoteFlowRulesMap m;
    auto id = m.insert(0, fake_flow(0xDEAD));
    ASSERT_GT(id, 0u);
    EXPECT_EQ(m.size_for_test(), 1u);

    EXPECT_TRUE(m.destroy_by_id(id));
    EXPECT_EQ(m.size_for_test(), 0u);

    // Second destroy on the now-stale id is a no-op.
    EXPECT_FALSE(m.destroy_by_id(id));
}

TEST(RemoteFlowRulesMap, DestroyAllClearsMap) {
    RemoteFlowRulesMap m;
    (void)m.insert(0, fake_flow(0x1));
    (void)m.insert(0, fake_flow(0x2));
    (void)m.insert(1, fake_flow(0x3));
    EXPECT_EQ(m.size_for_test(), 3u);

    m.destroy_all();
    EXPECT_EQ(m.size_for_test(), 0u);
}

TEST(RemoteFlowRulesMap, ConcurrentInsertsAreThreadSafe) {
    // Modest stress: 4 threads × 100 inserts each. Verifies the
    // mutex + atomic counter combination doesn't drop entries under
    // contention.
    RemoteFlowRulesMap m;
    constexpr size_t kThreads = 4;
    constexpr size_t kPerThread = 100;
    std::vector<std::thread> ts;
    std::atomic<size_t> ok{0};
    for (size_t t = 0; t < kThreads; ++t) {
        ts.emplace_back([&m, &ok, t]{
            for (size_t i = 0; i < kPerThread; ++i) {
                auto id = m.insert(
                    0, fake_flow(uintptr_t{0x10000} | (t << 8) | i));
                if (id != 0) ok.fetch_add(1);
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(ok.load(), kThreads * kPerThread);
    EXPECT_EQ(m.size_for_test(), kThreads * kPerThread);

    m.destroy_all();
    EXPECT_EQ(m.size_for_test(), 0u);
}
