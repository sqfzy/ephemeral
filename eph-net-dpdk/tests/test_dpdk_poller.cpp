/// @file test_dpdk_poller.cpp
/// Unit tests for `eph::net::dpdk::DpdkPoller`.
///
///   - concept conformance static_asserts (`Poller<DpdkPoller<>>`)
///   - factory + create()/destroy without a real NIC bound
///   - empty poll() returns 0
///   - add/remove cycle with a synthetic Pollable
///   - P2: register heterogeneous Pollables (two different concrete
///     types) on the same Poller and verify they coexist
///
/// We do NOT exercise rte_eth_rx_burst here — that requires a real port
/// (vfio-pci or net_null vdev wired into a port_id), which is the
/// integration-test scope. The unit tests focus on the routing-table
/// and friend-hook plumbing, where the bugs live.

#include <cstdint>
#include <set>
#include <string_view>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

#include "eph/dpdk/packet_core.hpp"  // kIpProtoTcp / kIpProtoUdp
#include "eph/net/concepts.hpp"
#include "eph/net/dpdk/detail/mbuf_view.hpp"
#include "eph/net/dpdk/poller.hpp"

namespace edpk = eph::net::dpdk;

// ---------------------------------------------------------------------------
// Concept conformance
// ---------------------------------------------------------------------------

static_assert(eph::net::Poller<edpk::DpdkPoller<>>,
              "DpdkPoller<> must satisfy eph::net::Poller");

// ---------------------------------------------------------------------------
// Synthetic Pollables for unit testing the routing/friend-hook plumbing.
// ---------------------------------------------------------------------------

namespace {

struct SyntheticPollableA {
    using PacketView = edpk::detail::MbufView;

    edpk::DpdkPoller<void>* attached_to = nullptr;
    uint32_t src_ip   = 0x0A000001;  // 10.0.0.1
    uint32_t dst_ip   = 0x0A000002;  // 10.0.0.2
    uint16_t src_port = 12345;
    uint16_t dst_port = 443;
    uint8_t  proto    = eph::dpdk::net::kIpProtoTcp;  // default TCP
    int      burst_calls = 0;
    int      detach_calls = 0;

    // Pollable concept satisfaction (the static_asserts in the public
    // headers are tested elsewhere; here we only need DpdkPoller::add to
    // type-check, which uses the friend-hook surface, not the concept).
    std::size_t poll_once_() noexcept { return 0; }
    bool        is_attached_() const noexcept { return attached_to != nullptr; }
    void*       native_handle() noexcept { return this; }

    // DpdkPoller-extension friend hooks
    void notify_attached_(edpk::DpdkPoller<void>* p) noexcept { attached_to = p; }
    void notify_detached_() noexcept { attached_to = nullptr; ++detach_calls; }
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
};

// A second concrete type with a different tuple — exercises the P2
// heterogeneous-Pollable path.
struct SyntheticPollableB {
    using PacketView = edpk::detail::MbufView;

    edpk::DpdkPoller<void>* attached_to = nullptr;
    uint32_t src_ip   = 0x0A000003;  // 10.0.0.3
    uint32_t dst_ip   = 0x0A000004;  // 10.0.0.4
    uint16_t src_port = 30000;
    uint16_t dst_port = 30001;
    uint8_t  proto    = eph::dpdk::net::kIpProtoUdp;  // default UDP (distinct from A)
    int      detach_calls = 0;

    std::size_t poll_once_() noexcept { return 0; }
    bool        is_attached_() const noexcept { return attached_to != nullptr; }
    void*       native_handle() noexcept { return this; }

    void notify_attached_(edpk::DpdkPoller<void>* p) noexcept { attached_to = p; }
    void notify_detached_() noexcept { attached_to = nullptr; ++detach_calls; }
    void tuple_for_poller_(uint32_t* s_ip, uint32_t* d_ip,
                            uint16_t* s_port, uint16_t* d_port,
                            uint8_t* p) noexcept {
        *s_ip = src_ip; *d_ip = dst_ip;
        *s_port = src_port; *d_port = dst_port;
        *p = proto;
    }
    void process_burst_(rte_mbuf** /*mbufs*/, uint16_t /*n*/,
                         uint64_t /*tsc*/) noexcept {}
};

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(DpdkPoller, PollerConfigDefaults) {
    edpk::PollerConfig cfg{};
    EXPECT_EQ(cfg.port_id, 0);
    EXPECT_EQ(cfg.rx_queue_id, 0);
}

TEST(DpdkPoller, CreateDestroyEmpty) {
    auto p = edpk::DpdkPoller<>::create({});
    ASSERT_TRUE(p.has_value()) << p.error().detail;
    EXPECT_EQ((*p)->size(), 0u);
    // Empty poll: returns 0 without touching the NIC.
    EXPECT_EQ((*p)->poll(), 0u);
}

TEST(DpdkPoller, AddNullptrFails) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    auto r = p->add<SyntheticPollableA>(nullptr);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(DpdkPoller, RemoveNonRegisteredFails) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    SyntheticPollableA pa;
    auto r = p->remove<SyntheticPollableA>(&pa);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
}

TEST(DpdkPoller, AddRemoveCycleClearsAttached) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    SyntheticPollableA pa;

    auto a = p->add<SyntheticPollableA>(&pa);
    ASSERT_TRUE(a.has_value()) << a.error().detail;
    EXPECT_EQ(p->size(), 1u);
    EXPECT_EQ(pa.attached_to, p.get());

    auto r = p->remove<SyntheticPollableA>(&pa);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_EQ(p->size(), 0u);
    EXPECT_EQ(pa.attached_to, nullptr);
    EXPECT_EQ(pa.detach_calls, 1);
}

TEST(DpdkPoller, AddDuplicateFails) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    SyntheticPollableA pa;

    auto a1 = p->add<SyntheticPollableA>(&pa);
    ASSERT_TRUE(a1.has_value());
    auto a2 = p->add<SyntheticPollableA>(&pa);
    ASSERT_FALSE(a2.has_value());
    EXPECT_EQ(a2.error().code, eph::core::Error::InvalidConfig);
}

TEST(DpdkPoller, P2HeterogeneousRegistration) {
    // The crux of the P2 design: a single DpdkPoller<> instance can
    // host arbitrary Pollable types, mixed in the same entries_ table,
    // with type-specific dispatch via the captured function-pointer
    // thunks.
    auto p = edpk::DpdkPoller<>::create({}).value();

    SyntheticPollableA pa;
    SyntheticPollableB pb;

    auto a = p->add<SyntheticPollableA>(&pa);
    ASSERT_TRUE(a.has_value()) << a.error().detail;
    auto b = p->add<SyntheticPollableB>(&pb);
    ASSERT_TRUE(b.has_value()) << b.error().detail;

    EXPECT_EQ(p->size(), 2u);
    EXPECT_EQ(pa.attached_to, p.get());
    EXPECT_EQ(pb.attached_to, p.get());

    // Cleanup via Poller dtor — both notify_detached_ hooks should fire.
    p.reset();
    EXPECT_EQ(pa.attached_to, nullptr);
    EXPECT_EQ(pb.attached_to, nullptr);
    EXPECT_GE(pa.detach_calls, 1);
    EXPECT_GE(pb.detach_calls, 1);
}

TEST(DpdkPoller, AddRejectsDuplicate5Tuple) {
    // Two distinct Pollable objects exposing the exact same 5-tuple (same
    // 4-tuple AND same IP protocol) must NOT both register — the routing
    // table would become ambiguous.
    auto p = edpk::DpdkPoller<>::create({}).value();

    SyntheticPollableA pa;  // default proto = kIpProtoTcp
    SyntheticPollableA pb;  // distinct object, same 5-tuple as pa

    auto a1 = p->add<SyntheticPollableA>(&pa);
    ASSERT_TRUE(a1.has_value()) << a1.error().detail;

    auto a2 = p->add<SyntheticPollableA>(&pb);
    ASSERT_FALSE(a2.has_value());
    EXPECT_EQ(a2.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{a2.error().detail}.find("5-tuple"),
              std::string_view::npos)
        << "detail should mention '5-tuple': " << a2.error().detail;

    // First registration must still be intact.
    EXPECT_EQ(p->size(), 1u);
    EXPECT_EQ(pa.attached_to, p.get());
    EXPECT_EQ(pb.attached_to, nullptr);  // rejected: never attached
}

TEST(DpdkPoller, CrossProtocolSame4TupleCoexists) {
    // TCP and UDP Pollables sharing the exact same (src_ip, dst_ip,
    // src_port, dst_port) live in independent L4 namespaces and must
    // both register successfully. The routing key is a 5-tuple, so the
    // IP protocol field disambiguates them on incoming packets.
    //
    // Pre-regression: before the 4→5-tuple upgrade, add() rejected the
    // second registration with "4-tuple already registered", incorrectly
    // conflating TCP and UDP scopes.
    auto p = edpk::DpdkPoller<>::create({}).value();

    SyntheticPollableA pa_tcp;
    pa_tcp.proto = eph::dpdk::net::kIpProtoTcp;

    SyntheticPollableA pa_udp;  // identical 4-tuple as pa_tcp
    pa_udp.proto = eph::dpdk::net::kIpProtoUdp;

    auto a1 = p->add<SyntheticPollableA>(&pa_tcp);
    ASSERT_TRUE(a1.has_value()) << a1.error().detail;

    auto a2 = p->add<SyntheticPollableA>(&pa_udp);
    ASSERT_TRUE(a2.has_value()) << a2.error().detail;

    EXPECT_EQ(p->size(), 2u);
    EXPECT_EQ(pa_tcp.attached_to, p.get());
    EXPECT_EQ(pa_udp.attached_to, p.get());
}

TEST(DpdkPoller, AddAcceptsDistinctFourTuplesOnSameDst) {
    // Sanity guard: the new 4-tuple check must NOT over-reject. Two
    // streams to the same (dst_ip, dst_port) but with different src_port
    // are legitimate concurrent client connections and must both add.
    auto p = edpk::DpdkPoller<>::create({}).value();

    SyntheticPollableA pa;
    SyntheticPollableA pb;
    pa.src_port = 40001;
    pb.src_port = 40002;  // only src_port differs
    // (src_ip, dst_ip, dst_port all identical)

    auto a1 = p->add<SyntheticPollableA>(&pa);
    ASSERT_TRUE(a1.has_value()) << a1.error().detail;

    auto a2 = p->add<SyntheticPollableA>(&pb);
    ASSERT_TRUE(a2.has_value()) << a2.error().detail;

    EXPECT_EQ(p->size(), 2u);
}

// ---------------------------------------------------------------------------
// pick_src_port tests
// ---------------------------------------------------------------------------
//
// pick_src_port is an advisory query that returns an unused source port for
// a new TCP client connection. Covered below:
//
//   - range validation (inverted, privileged, preferred out-of-range)
//   - empty Poller fast-path
//   - preferred-accepted / preferred-downgraded
//   - skip conflict with registered 4-tuple
//   - different dst_port => no conflict (4-tuple scoping)
//   - range exhaustion => OutOfMemory
//   - end-to-end pick + add confirmation
//
// Thread safety is not tested — pick_src_port is explicitly documented as
// "advisory"; DpdkPoller itself is non-MT-safe.

TEST(DpdkPoller, PickSrcPort_EmptyPollerReturnsInRange) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    auto r = p->pick_src_port(/*src_ip=*/0x0A000001,
                              /*dst_ip=*/0x0A000002,
                              /*dst_port=*/443);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_GE(*r, 32768);
    EXPECT_LE(*r, 60999);
}

TEST(DpdkPoller, PickSrcPort_PreferredAcceptedIfFree) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    auto r = p->pick_src_port(0x0A000001, 0x0A000002, 443,
                              /*range_begin=*/32768,
                              /*range_end=*/60999,
                              /*preferred=*/40001);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_EQ(*r, 40001);
}

TEST(DpdkPoller, PickSrcPort_PreferredOutOfRangeFails) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    auto r = p->pick_src_port(0x0A000001, 0x0A000002, 443,
                              32768, 60999, /*preferred=*/100);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{r.error().detail}.find("preferred"),
              std::string_view::npos);
}

TEST(DpdkPoller, PickSrcPort_InvertedRangeFails) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    auto r = p->pick_src_port(0x0A000001, 0x0A000002, 443,
                              /*range_begin=*/50000,
                              /*range_end=*/40000);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{r.error().detail}.find("range_begin"),
              std::string_view::npos);
}

TEST(DpdkPoller, PickSrcPort_PrivilegedRangeFails) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    auto r = p->pick_src_port(0x0A000001, 0x0A000002, 443,
                              /*range_begin=*/100, /*range_end=*/200);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{r.error().detail}.find("1024"),
              std::string_view::npos);
}

TEST(DpdkPoller, PickSrcPort_SkipsRegisteredFourTuple) {
    auto p = edpk::DpdkPoller<>::create({}).value();

    SyntheticPollableA pa;
    pa.src_ip   = 0x0A000001;
    pa.dst_ip   = 0x0A000002;
    pa.dst_port = 443;
    pa.src_port = 40001;
    ASSERT_TRUE(p->add<SyntheticPollableA>(&pa).has_value());

    // Preferred port is taken — must NOT return 40001.
    auto r = p->pick_src_port(0x0A000001, 0x0A000002, 443,
                              32768, 60999, /*preferred=*/40001);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_NE(*r, 40001);
    EXPECT_GE(*r, 32768);
    EXPECT_LE(*r, 60999);
}

TEST(DpdkPoller, PickSrcPort_DifferentDstDoesNotConflict) {
    // A 4-tuple is scoped by (src_ip, dst_ip, dst_port, src_port). If the
    // caller is connecting to a DIFFERENT dst_port, the registered stream
    // on 40001 does not conflict and pick_src_port may return 40001.
    auto p = edpk::DpdkPoller<>::create({}).value();

    SyntheticPollableA pa;
    pa.src_ip   = 0x0A000001;
    pa.dst_ip   = 0x0A000002;
    pa.dst_port = 443;
    pa.src_port = 40001;
    ASSERT_TRUE(p->add<SyntheticPollableA>(&pa).has_value());

    auto r = p->pick_src_port(0x0A000001, 0x0A000002, /*dst_port=*/8080,
                              32768, 60999, /*preferred=*/40001);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    EXPECT_EQ(*r, 40001);
}

TEST(DpdkPoller, PickSrcPort_ExhaustionReturnsOutOfMemory) {
    // Tiny range of 3 ports; fill all three with registered streams and
    // confirm pick_src_port reports exhaustion rather than hanging or
    // returning a bogus value.
    auto p = edpk::DpdkPoller<>::create({}).value();

    SyntheticPollableA p1, p2, p3;
    for (auto* s : {&p1, &p2, &p3}) {
        s->src_ip = 0x0A000001;
        s->dst_ip = 0x0A000002;
        s->dst_port = 443;
    }
    p1.src_port = 50000;
    p2.src_port = 50001;
    p3.src_port = 50002;
    ASSERT_TRUE(p->add<SyntheticPollableA>(&p1).has_value());
    ASSERT_TRUE(p->add<SyntheticPollableA>(&p2).has_value());
    ASSERT_TRUE(p->add<SyntheticPollableA>(&p3).has_value());

    auto r = p->pick_src_port(0x0A000001, 0x0A000002, 443,
                              /*range_begin=*/50000,
                              /*range_end=*/50002);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, eph::core::Error::OutOfMemory);
    EXPECT_NE(std::string_view{r.error().detail}.find("no free port"),
              std::string_view::npos);
}

TEST(DpdkPoller, PickSrcPort_ConfirmedByAdd) {
    // End-to-end: pick a port, stamp it onto a Pollable, add to Poller,
    // verify add succeeds. This is the contract pick_src_port promises
    // to uphold under single-threaded use.
    auto p = edpk::DpdkPoller<>::create({}).value();

    SyntheticPollableA existing;
    existing.src_ip = 0x0A000001;
    existing.dst_ip = 0x0A000002;
    existing.dst_port = 443;
    existing.src_port = 40001;
    ASSERT_TRUE(p->add<SyntheticPollableA>(&existing).has_value());

    auto picked = p->pick_src_port(0x0A000001, 0x0A000002, 443);
    ASSERT_TRUE(picked.has_value()) << picked.error().detail;
    EXPECT_NE(*picked, 40001);

    SyntheticPollableA fresh;
    fresh.src_ip   = 0x0A000001;
    fresh.dst_ip   = 0x0A000002;
    fresh.dst_port = 443;
    fresh.src_port = *picked;

    auto add_r = p->add<SyntheticPollableA>(&fresh);
    EXPECT_TRUE(add_r.has_value()) << add_r.error().detail;
    EXPECT_EQ(p->size(), 2u);
}

TEST(DpdkPoller, PickSrcPort_RandomStartSpreadsPicks) {
    // Smoke test for distribution: repeatedly pick on an empty poller;
    // the returned ports should not all be identical. Weak check —
    // strict distribution testing is out of scope.
    auto p = edpk::DpdkPoller<>::create({}).value();
    std::set<uint16_t> seen;
    for (int i = 0; i < 32; ++i) {
        auto r = p->pick_src_port(0x0A000001, 0x0A000002, 443);
        ASSERT_TRUE(r.has_value()) << r.error().detail;
        seen.insert(*r);
    }
    // With getrandom-seeded start across 32 calls on a 28k range the
    // odds of collapse to a single value are astronomically small.
    EXPECT_GT(seen.size(), 1u);
}

TEST(DpdkPoller, FillToCapacity) {
    auto p = edpk::DpdkPoller<>::create({}).value();
    constexpr std::size_t kMax = edpk::DpdkPoller<void>::kMaxConn;
    SyntheticPollableA arr[kMax];
    // Make each tuple unique so the routing keys do not collide.
    for (std::size_t i = 0; i < kMax; ++i) {
        arr[i].src_port = static_cast<uint16_t>(10000 + i);
    }
    for (std::size_t i = 0; i < kMax; ++i) {
        auto a = p->add<SyntheticPollableA>(&arr[i]);
        ASSERT_TRUE(a.has_value()) << "i=" << i << ": " << a.error().detail;
    }
    EXPECT_EQ(p->size(), kMax);

    // One more should be rejected with OutOfMemory.
    SyntheticPollableA overflow;
    overflow.src_port = 65000;
    auto a = p->add<SyntheticPollableA>(&overflow);
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().code, eph::core::Error::OutOfMemory);
}
