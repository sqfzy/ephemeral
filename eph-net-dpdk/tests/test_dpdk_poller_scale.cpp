/// @file test_dpdk_poller_scale.cpp
/// Scale + O(1)-routing tests for `eph::net::dpdk::DpdkPoller`.
///
///   - cap=64 honoured when explicitly requested via PollerConfig
///   - 65th add returns CapacityFull (mapped to OutOfMemory in the public
///     enum surface)
///   - O(1) routing via the open-addressed hash table: every registered
///     stream receives only its own packets even at full capacity
///   - tombstone reuse: remove + re-add on the same tuple routes to the
///     new object
///   - partial-tuple mismatch: packets that differ in any one of the 5
///     key fields do NOT match
///   - perf smoke (BENCH_PERF=1 only): 1M lookups average < 50 ns
///
/// We forge raw Ethernet/IPv4/TCP mbufs on the stack so the routing path
/// is exercised end-to-end without any rte_eth_rx_burst dependency. This
/// follows the pattern in test_packet_parse_adversarial.cpp.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/dpdk/packet_core.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"
#include "eph/net/dpdk/poller.hpp"

namespace edpk = eph::net::dpdk;
using namespace eph::dpdk::net;

namespace {

// ---------------------------------------------------------------------------
// Synthetic Pollable — minimal surface to satisfy DpdkPollable.
// ---------------------------------------------------------------------------

struct ScalePollable {
    using PacketView = edpk::detail::MbufView;

    edpk::DpdkPoller<void>* attached_to = nullptr;
    uint32_t src_ip   = 0x0A000001;
    uint32_t dst_ip   = 0x0A000002;
    uint16_t src_port = 0;
    uint16_t dst_port = 443;
    uint8_t  proto    = kIpProtoTcp;

    int      burst_calls = 0;

    // Auto-detach on destruction — mirrors `~DpdkTcpStream`. Without this,
    // a test that declares `Pollable arr` BEFORE the Poller's destructor
    // runs (vector destructed first → Poller destructed second) triggers
    // a heap-use-after-free as `~DpdkPoller` walks `entries_` and calls
    // `notify_detached_` on the freed objects. ASan caught it on the
    // initial run; this matches production semantics and removes the
    // declaration-order trap from every test that holds Pollables in a
    // vector or local object.
    ~ScalePollable() {
        if (attached_to != nullptr) {
            (void)attached_to->remove<ScalePollable>(this);
        }
    }

    ScalePollable() = default;
    ScalePollable(const ScalePollable&)            = delete;
    ScalePollable& operator=(const ScalePollable&) = delete;
    ScalePollable(ScalePollable&&)                 = delete;
    ScalePollable& operator=(ScalePollable&&)      = delete;

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
    void process_burst_(rte_mbuf** /*mbufs*/, uint16_t /*n*/,
                         uint64_t /*tsc*/) noexcept {
        ++burst_calls;
    }
    void on_poll_tick_(uint64_t /*tsc*/) noexcept {}
};

// ---------------------------------------------------------------------------
// Packet forging — minimal Ethernet/IPv4/TCP with an arbitrary 5-tuple.
//
// `src_ip / dst_ip / src_port / dst_port` are the *peer-side* values an
// incoming packet would carry. The Poller's lookup swaps them to compare
// against the registered local-view tuple, so passing the registered
// stream's tuple here would NOT route — see the call sites below where we
// swap explicitly.
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

} // namespace

// ===========================================================================
// Tests
// ===========================================================================

// max_connections=64 must be accepted by create().
TEST(DpdkPollerScale, ConfigAccepts64) {
    edpk::PollerConfig cfg{};
    cfg.max_connections = 64;
    auto p = edpk::DpdkPoller<>::create(cfg);
    ASSERT_TRUE(p.has_value()) << p.error().detail;
    EXPECT_EQ((*p)->size(), 0u);
}

// max_connections=0 / >kMaxConnHard must be rejected.
TEST(DpdkPollerScale, ConfigRejectsZero) {
    edpk::PollerConfig cfg{};
    cfg.max_connections = 0;
    auto p = edpk::DpdkPoller<>::create(cfg);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error().code, eph::core::Error::InvalidConfig);
}

TEST(DpdkPollerScale, ConfigRejectsAboveHardCap) {
    edpk::PollerConfig cfg{};
    cfg.max_connections = edpk::DpdkPoller<void>::kMaxConnHard + 1;
    auto p = edpk::DpdkPoller<>::create(cfg);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error().code, eph::core::Error::InvalidConfig);
}

// Default cap stays 16 (backwards-compat).
TEST(DpdkPollerScale, DefaultCapStaysAt16) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    constexpr std::size_t kDefault = edpk::DpdkPoller<void>::kMaxConn;  // 16
    ASSERT_EQ(kDefault, 16u);

    std::vector<ScalePollable> arr(kDefault + 1);
    for (std::size_t i = 0; i < arr.size(); ++i) {
        arr[i].src_port = static_cast<uint16_t>(20000 + i);
    }
    for (std::size_t i = 0; i < kDefault; ++i) {
        ASSERT_TRUE(p->add<ScalePollable>(&arr[i]).has_value());
    }
    auto last = p->add<ScalePollable>(&arr[kDefault]);
    ASSERT_FALSE(last.has_value());
    EXPECT_EQ(last.error().code, eph::core::Error::OutOfMemory);
}

// Register 64 distinct streams, route a forged packet for each, verify
// each goes to the right stream.
TEST(DpdkPollerScale, AddRemove64Streams) {
    edpk::PollerConfig cfg{};
    cfg.max_connections = 64;
    auto p = edpk::DpdkPoller<>::create(cfg).value();

    constexpr std::size_t kN = 64;
    std::vector<ScalePollable> arr(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        arr[i].src_ip   = 0x0A000001;       // local
        arr[i].dst_ip   = 0x0A000010;       // peer
        arr[i].src_port = static_cast<uint16_t>(40000 + i);
        arr[i].dst_port = 443;
        arr[i].proto    = kIpProtoTcp;
        ASSERT_TRUE(p->add<ScalePollable>(&arr[i]).has_value())
            << "i=" << i;
    }
    EXPECT_EQ(p->size(), kN);

    // Forge the on-wire view for each registered stream — the peer has
    // src_ip/src_port = our dst, and dst_ip/dst_port = our src.
    for (std::size_t i = 0; i < kN; ++i) {
        ForgedPacket pkt;
        forge_tcp_pkt(pkt,
                       /*src_ip_host=*/arr[i].dst_ip,
                       /*dst_ip_host=*/arr[i].src_ip,
                       /*src_port_host=*/arr[i].dst_port,
                       /*dst_port_host=*/arr[i].src_port);
        void* hit = p->route_for_test_(&pkt.mbuf);
        ASSERT_NE(hit, nullptr) << "i=" << i;
        EXPECT_EQ(hit, static_cast<void*>(&arr[i]))
            << "routed to wrong stream at i=" << i;
    }
    // Hash collision counter must be 0 — distinct tuples should not pile up.
    EXPECT_EQ(p->hash_collision_drops(), 0u);
}

// 65th add against a 64-cap Poller must return OutOfMemory (CapacityFull
// in the spec wording — the codebase maps "no slot" to OutOfMemory).
TEST(DpdkPollerScale, OverflowAt65thRejected) {
    edpk::PollerConfig cfg{};
    cfg.max_connections = 64;
    auto p = edpk::DpdkPoller<>::create(cfg).value();

    constexpr std::size_t kN = 64;
    std::vector<ScalePollable> arr(kN + 1);
    for (std::size_t i = 0; i < arr.size(); ++i) {
        arr[i].src_port = static_cast<uint16_t>(40000 + i);
    }
    for (std::size_t i = 0; i < kN; ++i) {
        ASSERT_TRUE(p->add<ScalePollable>(&arr[i]).has_value());
    }
    auto over = p->add<ScalePollable>(&arr[kN]);
    ASSERT_FALSE(over.has_value());
    EXPECT_EQ(over.error().code, eph::core::Error::OutOfMemory);
    EXPECT_EQ(p->size(), kN);
}

// Tombstone reuse: remove a stream, register a new (different) object on
// the same tuple, verify routing goes to the new object.
TEST(DpdkPollerScale, RemoveTombstoneReusable) {
    auto p = edpk::DpdkPoller<>::create({}).value();

    ScalePollable pa;
    pa.src_port = 50001;

    ASSERT_TRUE(p->add<ScalePollable>(&pa).has_value());

    ForgedPacket pkt;
    forge_tcp_pkt(pkt, pa.dst_ip, pa.src_ip, pa.dst_port, pa.src_port);
    EXPECT_EQ(p->route_for_test_(&pkt.mbuf), static_cast<void*>(&pa));

    ASSERT_TRUE(p->remove<ScalePollable>(&pa).has_value());
    // After remove, the tuple must NOT route.
    EXPECT_EQ(p->route_for_test_(&pkt.mbuf), nullptr);

    // Re-add a DIFFERENT object on the same tuple — must succeed and
    // routing must point to the new object.
    ScalePollable pb;
    pb.src_port = 50001;
    ASSERT_TRUE(p->add<ScalePollable>(&pb).has_value());
    EXPECT_EQ(p->route_for_test_(&pkt.mbuf), static_cast<void*>(&pb))
        << "tombstone slot was not reusable; routing returned stale or null";
}

// Partial-tuple mismatch: registered stream T; packet differs in one
// field (each of the 5). Routing must return nullptr in every case.
TEST(DpdkPollerScale, RouteByPartialTupleMismatch) {
    auto p = edpk::DpdkPoller<>::create({}).value();

    ScalePollable pa;
    pa.src_ip   = 0x0A000001;
    pa.dst_ip   = 0x0A000002;
    pa.src_port = 12345;
    pa.dst_port = 443;
    pa.proto    = kIpProtoTcp;
    ASSERT_TRUE(p->add<ScalePollable>(&pa).has_value());

    // Reference: matching packet routes correctly.
    {
        ForgedPacket pkt;
        forge_tcp_pkt(pkt, pa.dst_ip, pa.src_ip, pa.dst_port, pa.src_port);
        EXPECT_EQ(p->route_for_test_(&pkt.mbuf), static_cast<void*>(&pa));
    }

    // src_ip differs by one byte.
    {
        ForgedPacket pkt;
        forge_tcp_pkt(pkt, pa.dst_ip ^ 0x00000100u, pa.src_ip,
                            pa.dst_port, pa.src_port);
        EXPECT_EQ(p->route_for_test_(&pkt.mbuf), nullptr);
    }
    // dst_ip differs.
    {
        ForgedPacket pkt;
        forge_tcp_pkt(pkt, pa.dst_ip, pa.src_ip ^ 0x00000100u,
                            pa.dst_port, pa.src_port);
        EXPECT_EQ(p->route_for_test_(&pkt.mbuf), nullptr);
    }
    // src_port differs.
    {
        ForgedPacket pkt;
        forge_tcp_pkt(pkt, pa.dst_ip, pa.src_ip,
                            static_cast<uint16_t>(pa.dst_port ^ 1),
                            pa.src_port);
        EXPECT_EQ(p->route_for_test_(&pkt.mbuf), nullptr);
    }
    // dst_port differs.
    {
        ForgedPacket pkt;
        forge_tcp_pkt(pkt, pa.dst_ip, pa.src_ip, pa.dst_port,
                            static_cast<uint16_t>(pa.src_port ^ 1));
        EXPECT_EQ(p->route_for_test_(&pkt.mbuf), nullptr);
    }
    // proto differs (TCP-vs-UDP). Forge a UDP packet manually — routing
    // must return nullptr because the registration is TCP-only.
    {
        ForgedPacket pkt;
        std::memset(pkt.buf, 0, sizeof(pkt.buf));
        constexpr size_t eth_len = kEtherHeaderLen;
        constexpr size_t ip_len  = 20;
        constexpr size_t udp_len = kUdpHeaderLen;

        auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt.buf);
        eth->ether_type = hton16(kEtherTypeIpv4);

        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(pkt.buf + eth_len);
        ip->version_ihl   = static_cast<uint8_t>((4 << 4) | 5);
        ip->total_length  = hton16(static_cast<uint16_t>(ip_len + udp_len));
        ip->next_proto_id = kIpProtoUdp;
        ip->src_addr      = hton32(pa.dst_ip);
        ip->dst_addr      = hton32(pa.src_ip);

        auto* udp = reinterpret_cast<UdpHeader*>(pkt.buf + eth_len + ip_len);
        udp->src_port = hton16(pa.dst_port);
        udp->dst_port = hton16(pa.src_port);
        udp->length   = hton16(static_cast<uint16_t>(udp_len));
        udp->checksum = 0;

        pkt.mbuf.buf_addr = pkt.buf;
        pkt.mbuf.data_off = 0;
        pkt.mbuf.data_len = static_cast<uint16_t>(eth_len + ip_len + udp_len);
        pkt.mbuf.pkt_len  = static_cast<uint32_t>(eth_len + ip_len + udp_len);

        EXPECT_EQ(p->route_for_test_(&pkt.mbuf), nullptr);
    }
}

// Mid-fill remove must keep routing intact for the surviving entries.
// Catches bugs in the route_table_ entry_idx adjustment after entries_
// is shifted left.
TEST(DpdkPollerScale, RemoveMiddleKeepsRoutingIntact) {
    edpk::PollerConfig cfg{};
    cfg.max_connections = 64;
    auto p = edpk::DpdkPoller<>::create(cfg).value();

    constexpr std::size_t kN = 16;
    std::vector<ScalePollable> arr(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        arr[i].src_port = static_cast<uint16_t>(60000 + i);
        ASSERT_TRUE(p->add<ScalePollable>(&arr[i]).has_value());
    }

    // Remove the middle one.
    ASSERT_TRUE(p->remove<ScalePollable>(&arr[8]).has_value());
    EXPECT_EQ(p->size(), kN - 1);

    // Surviving entries must still route to themselves.
    for (std::size_t i = 0; i < kN; ++i) {
        if (i == 8) continue;
        ForgedPacket pkt;
        forge_tcp_pkt(pkt, arr[i].dst_ip, arr[i].src_ip,
                            arr[i].dst_port, arr[i].src_port);
        EXPECT_EQ(p->route_for_test_(&pkt.mbuf), static_cast<void*>(&arr[i]))
            << "i=" << i;
    }
    // The removed tuple must not route.
    {
        ForgedPacket pkt;
        forge_tcp_pkt(pkt, arr[8].dst_ip, arr[8].src_ip,
                            arr[8].dst_port, arr[8].src_port);
        EXPECT_EQ(p->route_for_test_(&pkt.mbuf), nullptr);
    }
}

// Optional perf smoke: measure 1M lookups, expect avg < 50 ns.
// Default-skipped because CI hosts have variable timing; flip BENCH_PERF=1
// to opt in.
TEST(DpdkPollerScale, PerformanceSmoke) {
    if (!std::getenv("BENCH_PERF")) {
        GTEST_SKIP() << "PerformanceSmoke is opt-in; set BENCH_PERF=1 to run";
    }
    edpk::PollerConfig cfg{};
    cfg.max_connections = 64;
    auto p = edpk::DpdkPoller<>::create(cfg).value();

    constexpr std::size_t kN = 64;
    std::vector<ScalePollable> arr(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        arr[i].src_port = static_cast<uint16_t>(40000 + i);
        ASSERT_TRUE(p->add<ScalePollable>(&arr[i]).has_value());
    }

    // Pre-build mbufs for each registered tuple so the loop body is just
    // the routing call.
    std::vector<ForgedPacket> pkts(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        forge_tcp_pkt(pkts[i],
                       arr[i].dst_ip, arr[i].src_ip,
                       arr[i].dst_port, arr[i].src_port);
    }

    constexpr std::size_t kIters = 1'000'000;
    std::size_t hit_sink = 0;

    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    for (std::size_t i = 0; i < kIters; ++i) {
        // Cycle through all 64 tuples — exercises every probe chain
        // length the table is going to see in practice.
        void* hit = p->route_for_test_(&pkts[i & (kN - 1)].mbuf);
        // Defeat dead-code elimination — asm clobber is the canonical
        // way under both GCC and Clang.
        asm volatile("" : : "r"(hit) : "memory");
        if (hit) ++hit_sink;
    }
    auto t1 = clk::now();
    const double ns_per_op =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
        / static_cast<double>(kIters);

    std::cerr << "[PerformanceSmoke] avg lookup: "
              << ns_per_op << " ns/op (" << kIters << " iters, "
              << hit_sink << " hits)\n";
    EXPECT_LT(ns_per_op, 50.0)
        << "avg lookup exceeded 50 ns target — investigate hash distribution";
}
