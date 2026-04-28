/// @file test_dpdk_poller_timeout.cpp
/// Unit tests for `eph::net::dpdk::DpdkPoller::poll(std::chrono::nanoseconds)`
/// — the CPU-yielding debugging form added in T3.15.
///
/// The five required cases:
///   - ZeroTimeoutBehavesLikeBurst   — `poll(0ns)` does a single burst and
///                                      returns the same value as `poll()`.
///   - TimeoutEnforced               — `poll(50ms)` against an idle Poller
///                                      returns within ~2x the deadline,
///                                      with return value 0.
///   - EarlyExitOnPacket             — synthesised packet via the
///                                      `inject_mbuf_for_test_` seam: a
///                                      1s deadline returns within a few ms
///                                      and the packet was processed.
///   - NegativeTimeoutTreatedAsZero  — `poll(-1ms)` does not hang; falls
///                                      through to single-burst behaviour.
///   - ReturnValueMatchesPacketCount — drive 5 injected mbufs through
///                                      multiple `poll(timeout)` calls;
///                                      summed return values == 5.
///
/// We do NOT exercise `rte_eth_rx_burst` (no real port bound under
/// --no-pci), so the EarlyExit / ReturnValue tests reach the dispatch
/// path via the test-only injection seam, mirroring the routing-table
/// approach in `test_dpdk_poller_scale.cpp` (forge mbuf on the stack,
/// hand to a `_for_test_` entry point).

#include <chrono>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/dpdk/packet_core.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"
#include "eph/net/dpdk/poller.hpp"

namespace edpk = eph::net::dpdk;
using namespace eph::dpdk::net;

// ---------------------------------------------------------------------------
// Synthetic Pollable — minimal surface to satisfy DpdkPollable. Mirrors
// SyntheticPollableA in test_dpdk_poller.cpp; copied locally so this TU
// stays self-contained.
// ---------------------------------------------------------------------------

namespace {

struct TimeoutPollable {
    using PacketView = edpk::detail::MbufView;

    edpk::DpdkPoller<void>* attached_to = nullptr;
    uint32_t src_ip   = 0x0A000001;  // 10.0.0.1 (local)
    uint32_t dst_ip   = 0x0A000002;  // 10.0.0.2 (peer)
    uint16_t src_port = 12345;
    uint16_t dst_port = 443;
    uint8_t  proto    = kIpProtoTcp;

    int burst_calls = 0;
    int packets_seen = 0;
    int tick_calls = 0;

    // Auto-detach on destruction — matches production semantics and
    // removes the declaration-order trap from tests that hold the
    // Pollable on the stack and the Poller in a unique_ptr above it
    // (the Poller's dtor walks entries_ on its own teardown; the
    // Pollable's dtor would otherwise be a no-op heap-use-after-free
    // hazard).
    ~TimeoutPollable() {
        if (attached_to != nullptr) {
            (void)attached_to->remove<TimeoutPollable>(this);
        }
    }

    TimeoutPollable() = default;
    TimeoutPollable(const TimeoutPollable&)            = delete;
    TimeoutPollable& operator=(const TimeoutPollable&) = delete;
    TimeoutPollable(TimeoutPollable&&)                 = delete;
    TimeoutPollable& operator=(TimeoutPollable&&)      = delete;

    std::size_t poll_once_() noexcept { return 0; }
    bool        is_attached_() const noexcept { return attached_to != nullptr; }
    void*       native_handle() noexcept { return this; }

    void notify_attached_(edpk::DpdkPoller<void>* p) noexcept { attached_to = p; }
    void notify_detached_() noexcept { attached_to = nullptr; }
    void tuple_for_poller_(uint32_t* s_ip, uint32_t* d_ip,
                            uint16_t* s_port, uint16_t* d_port,
                            uint8_t* p) noexcept {
        *s_ip = src_ip; *d_ip = dst_ip;
        *s_port = src_port; *d_port = dst_port;
        *p = proto;
    }
    void process_burst_(rte_mbuf** /*mbufs*/, uint16_t n,
                         uint64_t /*tsc*/) noexcept {
        ++burst_calls;
        packets_seen += n;
    }
    void on_poll_tick_(uint64_t /*tsc*/) noexcept { ++tick_calls; }
};

// ---------------------------------------------------------------------------
// Packet forging — mirrors test_dpdk_poller_scale.cpp::forge_tcp_pkt with
// minimal Ethernet/IPv4/TCP headers. The on-wire view has src/dst swapped
// relative to the registered Pollable's tuple (the Poller swaps them again
// during routing-table lookup).
// ---------------------------------------------------------------------------

struct ForgedPacket {
    alignas(8) uint8_t  buf[128]{};
    rte_mbuf            mbuf{};
};

void forge_tcp_pkt(ForgedPacket& pkt,
                    uint32_t src_ip_host,
                    uint32_t dst_ip_host,
                    uint16_t src_port_host,
                    uint16_t dst_port_host) noexcept {
    constexpr size_t eth_len = kEtherHeaderLen;
    constexpr size_t ip_len  = 20;
    constexpr size_t tcp_len = 20;
    constexpr size_t total   = eth_len + ip_len + tcp_len;

    std::memset(pkt.buf, 0, sizeof(pkt.buf));

    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt.buf);
    eth->ether_type = hton16(kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt.buf + eth_len);
    ip->version_ihl   = static_cast<uint8_t>((4 << 4) | 5);
    ip->total_length  = hton16(static_cast<uint16_t>(ip_len + tcp_len));
    ip->next_proto_id = kIpProtoTcp;
    ip->src_addr      = hton32(src_ip_host);
    ip->dst_addr      = hton32(dst_ip_host);

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(pkt.buf + eth_len + ip_len);
    tcp->data_off  = static_cast<uint8_t>(5 << 4);
    tcp->src_port  = hton16(src_port_host);
    tcp->dst_port  = hton16(dst_port_host);
    tcp->tcp_flags = kTcpAck;

    pkt.mbuf            = rte_mbuf{};
    pkt.mbuf.buf_addr   = pkt.buf;
    pkt.mbuf.data_off   = 0;
    pkt.mbuf.data_len   = static_cast<uint16_t>(total);
    pkt.mbuf.pkt_len    = static_cast<uint32_t>(total);
}

// Build a ForgedPacket whose on-wire 4-tuple corresponds to the registered
// Pollable's tuple — swapping src/dst so the Poller's lookup_by_5tuple_
// re-swap routes back to `pa`.
void forge_tcp_pkt_for(ForgedPacket& pkt, const TimeoutPollable& pa) noexcept {
    forge_tcp_pkt(pkt,
                   /*src_ip_host=*/pa.dst_ip,
                   /*dst_ip_host=*/pa.src_ip,
                   /*src_port_host=*/pa.dst_port,
                   /*dst_port_host=*/pa.src_port);
}

} // namespace

// ===========================================================================
// Tests
// ===========================================================================

// Phase 1 of the algorithm always runs a single burst regardless of the
// timeout argument. With timeout == 0 we must collapse to "exactly one
// burst" — same observable result as calling the no-arg poll().
TEST(DpdkPollerTimeout, ZeroTimeoutBehavesLikeBurst) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    TimeoutPollable pa;
    ASSERT_TRUE(p->add<TimeoutPollable>(&pa).has_value());

    // No NIC traffic, no injection — both calls return 0 and tick once.
    EXPECT_EQ(p->poll(std::chrono::nanoseconds(0)), 0u);
    EXPECT_EQ(pa.tick_calls, 1);

    EXPECT_EQ(p->poll(), 0u);
    EXPECT_EQ(pa.tick_calls, 2);
}

// The contract: poll(timeout) must return within ~timeout + one burst
// when no traffic arrives. We use a 2x slack to absorb scheduler jitter
// — the actual bound is much tighter (within a few hundred µs in
// practice on this Graviton box).
TEST(DpdkPollerTimeout, TimeoutEnforced) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    TimeoutPollable pa;
    ASSERT_TRUE(p->add<TimeoutPollable>(&pa).has_value());

    constexpr auto kTimeout = std::chrono::milliseconds(50);
    constexpr auto kSlack   = std::chrono::milliseconds(100);  // ~2x

    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t n = p->poll(kTimeout);
    const auto t1 = std::chrono::steady_clock::now();

    EXPECT_EQ(n, 0u);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    EXPECT_LE(elapsed, kSlack)
        << "poll(50ms) took " << elapsed.count() << "ms (expected <= 100ms)";
    // Lower bound: must have actually waited (at least most of the timeout).
    EXPECT_GE(elapsed, std::chrono::milliseconds(40))
        << "poll(50ms) returned in " << elapsed.count() << "ms — should wait near the deadline";
}

// Inject a packet and confirm poll(1s) returns within a few ms with the
// packet processed. The early-exit short-circuit (Phase 1: drain
// injection + single burst → return on hit) means we never enter the
// spin / yield loop at all when the seam is non-empty.
TEST(DpdkPollerTimeout, EarlyExitOnPacket) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    TimeoutPollable pa;
    ASSERT_TRUE(p->add<TimeoutPollable>(&pa).has_value());

    ForgedPacket pkt;
    forge_tcp_pkt_for(pkt, pa);
    p->inject_mbuf_for_test_(&pkt.mbuf);

    constexpr auto kDeadline = std::chrono::seconds(1);
    constexpr auto kSlack    = std::chrono::milliseconds(100);

    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t n = p->poll(kDeadline);
    const auto t1 = std::chrono::steady_clock::now();

    EXPECT_EQ(n, 1u);
    EXPECT_EQ(pa.packets_seen, 1);
    EXPECT_EQ(pa.burst_calls, 1);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    EXPECT_LT(elapsed, kSlack)
        << "EarlyExit took " << elapsed.count() << "ms (expected << 1s)";
}

// Negative timeouts surface from defensive caller code that computes
// `deadline - now()` after now() drifted past the deadline. Treat them
// as zero — single burst, no hang. Without the guard the spin/yield
// loops would be entered with a stale deadline and never exit.
TEST(DpdkPollerTimeout, NegativeTimeoutTreatedAsZero) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    TimeoutPollable pa;
    ASSERT_TRUE(p->add<TimeoutPollable>(&pa).has_value());

    const auto t0 = std::chrono::steady_clock::now();
    const std::size_t n = p->poll(std::chrono::milliseconds(-1));
    const auto t1 = std::chrono::steady_clock::now();

    EXPECT_EQ(n, 0u);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    EXPECT_LT(elapsed, std::chrono::milliseconds(50))
        << "negative timeout took " << elapsed.count() << "ms (expected single-burst fast path)";
}

// Drive 5 packets through one at a time: each `poll(timeout)` call
// drains the injection seam, returns 1 (early exit), and we re-inject
// for the next iteration. Sum of return values must equal 5 — catches
// off-by-one errors in the early-exit return path.
TEST(DpdkPollerTimeout, ReturnValueMatchesPacketCount) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    TimeoutPollable pa;
    ASSERT_TRUE(p->add<TimeoutPollable>(&pa).has_value());

    constexpr int kN = 5;
    ForgedPacket pkt;  // re-used; only one in flight at a time.
    forge_tcp_pkt_for(pkt, pa);

    std::size_t total = 0;
    for (int i = 0; i < kN; ++i) {
        p->inject_mbuf_for_test_(&pkt.mbuf);
        const std::size_t n = p->poll(std::chrono::milliseconds(50));
        EXPECT_EQ(n, 1u) << "iteration i=" << i;
        total += n;
    }

    EXPECT_EQ(total, static_cast<std::size_t>(kN));
    EXPECT_EQ(pa.packets_seen, kN);
    EXPECT_EQ(pa.burst_calls, kN);
}
