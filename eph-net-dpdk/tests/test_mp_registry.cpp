/// @file test_mp_registry.cpp
/// Unit tests for `eph::dpdk::detail::MpRegistryHandle` — the
/// hugepage-backed cross-process registry.
///
/// **Run-time status (reshape stage 2)**: this test binary is currently
/// build-only because it boots EAL via `dpdk_test_env` and would
/// collide with any other DPDK process on the host that's holding
/// hugepages or the runtime dir. The tests will be exercised in stage 7
/// once the host's NIC is released. Compilation of this file is the
/// stage-2 gate: it pins the registry header's POD layout, the public
/// factory signatures, and the cross-validation paths at compile time
/// even when the test runner is parked.
///
/// All cases below are pure attach/lookup logic — none of them need a
/// vfio-pci NIC. The shared `dpdk_test_env` runs EAL with
/// `--no-pci --no-huge` so the only host requirement is the EAL
/// runtime dir not being held by a sibling process.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp"

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/mp_registry.hpp"
#include "eph/dpdk/mp_topology.hpp"

using eph::core::Error;
using eph::dpdk::MpTopology;
using eph::dpdk::ProcSpec;
using eph::dpdk::detail::build_mp_registry_name;
using eph::dpdk::detail::kMpRegistryFilePrefixMax;
using eph::dpdk::detail::kMpRegistryMagic;
using eph::dpdk::detail::kMpRegistryVersion;
using eph::dpdk::detail::MpRegistryHandle;

// ─────────────────────────────────────────────────────────────────────────────
// build_mp_registry_name — pure logic, no EAL touch
// ─────────────────────────────────────────────────────────────────────────────

TEST(MpRegistryName, PrefixedAndNullTerminated) {
    auto r = build_mp_registry_name("demo");
    ASSERT_TRUE(r.has_value());
    EXPECT_STREQ(r->data(), "eph_mp/demo");
}

TEST(MpRegistryName, EmptyFilePrefix_Rejected) {
    auto r = build_mp_registry_name("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(MpRegistryName, MaxLengthAccepted) {
    // 24 bytes — exactly kMpRegistryFilePrefixMax
    std::string fp(kMpRegistryFilePrefixMax, 'x');
    auto r = build_mp_registry_name(fp);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(std::strlen(r->data()),
              std::string("eph_mp/").size() + kMpRegistryFilePrefixMax);
}

TEST(MpRegistryName, OverMaxLengthRejected) {
    std::string fp(kMpRegistryFilePrefixMax + 1, 'x');
    auto r = build_mp_registry_name(fp);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

// ─────────────────────────────────────────────────────────────────────────────
// create_primary / attach_secondary — full-cycle tests (need EAL)
// ─────────────────────────────────────────────────────────────────────────────

namespace {
MpTopology make_topo(uint8_t self_index, uint8_t total_procs = 2) {
    return MpTopology::uniform(self_index, total_procs, /*nb_rx_queues=*/4);
}
} // namespace

TEST(MpRegistry, CreatePrimary_WritesHeader) {
    auto h = MpRegistryHandle::create_primary("rgcrhdr", make_topo(0));
    ASSERT_TRUE(h.has_value()) << h.error();
    ASSERT_TRUE(static_cast<bool>(*h));

    const auto* hdr = h->header();
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->magic, kMpRegistryMagic);
    EXPECT_EQ(hdr->version, kMpRegistryVersion);
    EXPECT_EQ(hdr->total_procs, 2u);
    EXPECT_STREQ(hdr->file_prefix, "rgcrhdr");

    // Self slot claimed; peer slot still free.
    EXPECT_EQ(h->self().claimed.load(std::memory_order_acquire), 1);
    EXPECT_EQ(hdr->procs[1].claimed.load(std::memory_order_acquire), 0);
}

TEST(MpRegistry, AttachSecondary_LookupMatchesPrimary) {
    auto p = MpRegistryHandle::create_primary("rgsec", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto s = MpRegistryHandle::attach_secondary("rgsec", make_topo(1));
    ASSERT_TRUE(s.has_value()) << s.error();
    EXPECT_EQ(s->self_index(), 1);
    EXPECT_EQ(s->self().claimed.load(std::memory_order_acquire), 1);
    // Same memzone → same backing header.
    EXPECT_EQ(p->header(), s->header());
}

TEST(MpRegistry, AttachSecondary_NoPrimary_NotFound) {
    auto s = MpRegistryHandle::attach_secondary("rgnoprimary", make_topo(1));
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().code, Error::NotFound);
}

TEST(MpRegistry, AttachSecondary_TopoMismatch_Rejected) {
    auto p = MpRegistryHandle::create_primary("rgtmisma", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    // Secondary declares 3 procs while primary wrote 2 → total_procs mismatch.
    auto bad = MpRegistryHandle::attach_secondary(
        "rgtmisma",
        MpTopology::uniform(/*self_index=*/2, /*total_procs=*/3,
                            /*nb_rx_queues=*/6));
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, Error::InvalidConfig);
}

TEST(MpRegistry, AttachSecondary_SelfSpecMismatch_Rejected) {
    auto p = MpRegistryHandle::create_primary("rgsspm", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    // Same total_procs but secondary's own spec disagrees on port range.
    MpTopology topo = make_topo(1);
    topo.procs[1].port_lo += 100;  // shift our self spec
    auto bad = MpRegistryHandle::attach_secondary("rgsspm", topo);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, Error::InvalidConfig);
}

TEST(MpRegistry, AttachSecondary_DoubleClaim_Rejected) {
    auto p = MpRegistryHandle::create_primary("rgdbl", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto s1 = MpRegistryHandle::attach_secondary("rgdbl", make_topo(1));
    ASSERT_TRUE(s1.has_value()) << s1.error();

    // Second secondary with same self_index fails on CAS.
    auto s2 = MpRegistryHandle::attach_secondary("rgdbl", make_topo(1));
    ASSERT_FALSE(s2.has_value());
    EXPECT_EQ(s2.error().code, Error::InvalidConfig);
}

TEST(MpRegistry, Destroy_ReleasesClaimedSlot) {
    auto p = MpRegistryHandle::create_primary("rgrel", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    {
        auto s = MpRegistryHandle::attach_secondary("rgrel", make_topo(1));
        ASSERT_TRUE(s.has_value()) << s.error();
        EXPECT_EQ(p->header()->procs[1].claimed.load(std::memory_order_acquire), 1);
    }
    // Secondary's RAII released the slot — primary can see it as free.
    EXPECT_EQ(p->header()->procs[1].claimed.load(std::memory_order_acquire), 0);

    // And another secondary can claim again now.
    auto s2 = MpRegistryHandle::attach_secondary("rgrel", make_topo(1));
    ASSERT_TRUE(s2.has_value()) << s2.error();
}

TEST(MpRegistry, MoveSemantics_TransferOwnership) {
    auto p = MpRegistryHandle::create_primary("rgmov", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();
    const auto* hdr_before = p->header();

    MpRegistryHandle moved = std::move(*p);
    EXPECT_FALSE(static_cast<bool>(*p));
    EXPECT_TRUE(static_cast<bool>(moved));
    EXPECT_EQ(moved.header(), hdr_before);
    EXPECT_EQ(moved.self().claimed.load(std::memory_order_acquire), 1);
}

TEST(MpRegistry, InvalidTopology_Rejected) {
    MpTopology bad;  // empty procs → !valid()
    auto p = MpRegistryHandle::create_primary("rgbadt", bad);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error().code, Error::InvalidConfig);
}
