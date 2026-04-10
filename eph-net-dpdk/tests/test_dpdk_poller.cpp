/// @file test_dpdk_poller.cpp
/// Unit tests for `eph::net::dpdk::DpdkPoller`. Phase 4 scope:
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

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep

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
                            uint16_t* s_port, uint16_t* d_port) noexcept {
        *s_ip = src_ip; *d_ip = dst_ip;
        *s_port = src_port; *d_port = dst_port;
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
    int      detach_calls = 0;

    std::size_t poll_once_() noexcept { return 0; }
    bool        is_attached_() const noexcept { return attached_to != nullptr; }
    void*       native_handle() noexcept { return this; }

    void notify_attached_(edpk::DpdkPoller<void>* p) noexcept { attached_to = p; }
    void notify_detached_() noexcept { attached_to = nullptr; ++detach_calls; }
    void tuple_for_poller_(uint32_t* s_ip, uint32_t* d_ip,
                            uint16_t* s_port, uint16_t* d_port) noexcept {
        *s_ip = src_ip; *d_ip = dst_ip;
        *s_port = src_port; *d_port = dst_port;
    }
    void process_burst_(rte_mbuf** /*mbufs*/, uint16_t /*n*/,
                         uint64_t /*tsc*/) noexcept {}
};

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

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
