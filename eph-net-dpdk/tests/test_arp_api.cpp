/// @file eph-net-dpdk/tests/test_arp_api.cpp
///
/// Compile-time contract test for the stage-4 ARP API hardening.
/// Verifies that `eph::dpdk::arp::resolve` no longer accepts a
/// `queue_id` parameter — internally it is hardcoded to queue 0
/// because EtherType 0x0806 (ARP) is not in the project's RSS hash
/// set and always falls to the NIC's default RX queue.
///
/// The pre-fix signature took `(port_id, queue_id, pool, src_mac, ...)`
/// which let RSS multi-queue Platforms silently time out when a caller
/// passed `queue_id != 0` (the resolver polled the wrong queue while
/// the reply went to queue 0). Removing the parameter makes that
/// mistake unrepresentable.
///
/// Tests do NOT call resolve() — that requires DPDK runtime. They use
/// `is_invocable_v` to verify the new signature compiles and the old
/// signature does NOT.

#include <chrono>
#include <cstdint>
#include <optional>
#include <type_traits>

#include <gtest/gtest.h>
#include <rte_ether.h>
#include <rte_mempool.h>

#include "eph/dpdk/arp.hpp"

namespace {

// SFINAE detector: does `arp::resolve(args...)` compile?
// `is_invocable_v` over a function-pointer type doesn't honor default
// arguments, so we use a lambda + decltype trick that exercises the
// actual call expression — defaults are honored, overload resolution
// runs as in real code.
template <class... Args>
concept ArpResolveInvocable = requires(Args&&... args) {
    eph::dpdk::arp::resolve(std::forward<Args>(args)...);
};

template <class Io, class... Args>
concept ArpResolveWithIoInvocable = requires(Args&&... args) {
    eph::dpdk::arp::resolve_with_io<Io>(std::forward<Args>(args)...);
};

const rte_ether_addr kZeroMac{};

} // namespace

// ─────────────────────────────────────────────────────────────────────
// New (post-fix) signature: queue_id removed.
// ─────────────────────────────────────────────────────────────────────

TEST(ArpApiHardening, NewMinSignatureCompiles) {
    constexpr bool ok = ArpResolveInvocable<
        uint16_t, rte_mempool*, const rte_ether_addr&, uint32_t, uint32_t>;
    static_assert(ok,
        "arp::resolve(port_id, pool, src_mac, src_ip, target_ip) "
        "must be invocable");
    EXPECT_TRUE(ok);
}

TEST(ArpApiHardening, NewFullSignatureCompiles) {
    constexpr bool ok = ArpResolveInvocable<
        uint16_t, rte_mempool*, const rte_ether_addr&, uint32_t, uint32_t,
        std::chrono::milliseconds, std::optional<rte_ether_addr>>;
    static_assert(ok,
        "arp::resolve full signature with timeout + expected_mac "
        "must be invocable");
    EXPECT_TRUE(ok);
}

// ─────────────────────────────────────────────────────────────────────
// Old (pre-fix) signature with queue_id at slot 1 — MUST be rejected.
// ─────────────────────────────────────────────────────────────────────

TEST(ArpApiHardening, OldQueueIdSignatureRejected) {
    constexpr bool ok = ArpResolveInvocable<
        uint16_t, uint16_t, rte_mempool*, const rte_ether_addr&,
        uint32_t, uint32_t>;
    static_assert(!ok,
        "arp::resolve must NOT accept a queue_id parameter post-fix "
        "(stage 4 hardening: removed to prevent RSS-multi-queue silent "
        "timeouts on non-zero queues — ARP packets never get hashed by "
        "RSS so they always land on the NIC's default queue 0)");
    EXPECT_FALSE(ok);
}

// ─────────────────────────────────────────────────────────────────────
// resolve_with_io retains queue_id for testability (unit tests inject
// fake Io shims that record (port, queue) pairs).
// ─────────────────────────────────────────────────────────────────────

TEST(ArpApiHardening, ResolveWithIoStillTakesQueueId) {
    constexpr bool ok = ArpResolveWithIoInvocable<
        eph::dpdk::arp::RealNicIoArp,
        uint16_t, uint16_t, rte_mempool*, const rte_ether_addr&,
        uint32_t, uint32_t>;
    static_assert(ok,
        "resolve_with_io should retain queue_id for unit-test fakes");
    EXPECT_TRUE(ok);
}
