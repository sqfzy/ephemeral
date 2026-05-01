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
using eph::dpdk::detail::kMpRegistrySelfIndexUnset;
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
    // kMpRegistryFilePrefixMax - 1 bytes — the largest size that fits in
    // MpRegistryHeader::file_prefix[kMpRegistryFilePrefixMax] *with* a NUL
    // terminator. The validate now reserves the last byte for NUL so the
    // strncmp(min(size, max-1)) read paths and the SPDLOG `{}` formatter
    // can both treat the field as a C string safely.
    std::string fp(kMpRegistryFilePrefixMax - 1, 'x');
    auto r = build_mp_registry_name(fp);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(std::strlen(r->data()),
              std::string("eph_mp/").size() + (kMpRegistryFilePrefixMax - 1));
}

TEST(MpRegistryName, OverMaxLengthRejected) {
    // kMpRegistryFilePrefixMax bytes — was accepted previously but
    // would silently lose the last byte during init_mp_registry_header
    // and could let two distinct prefixes that share the first
    // kMpRegistryFilePrefixMax-1 bytes hash-collide on strncmp.
    std::string fp(kMpRegistryFilePrefixMax, 'x');
    auto r = build_mp_registry_name(fp);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(MpRegistryName, FarOverMaxLengthRejected) {
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

// ─────────────────────────────────────────────────────────────────────────────
// Mode 2 helpers — attach_secondary_readonly + try_claim_free_slot +
// attach_secondary(already_claimed=true)
// ─────────────────────────────────────────────────────────────────────────────

TEST(MpRegistry, AttachSecondaryReadonly_LookupSucceedsNoClaim) {
    auto p = MpRegistryHandle::create_primary("rgrdonly", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto ro = MpRegistryHandle::attach_secondary_readonly("rgrdonly");
    ASSERT_TRUE(ro.has_value()) << ro.error();
    ASSERT_TRUE(static_cast<bool>(*ro));
    EXPECT_FALSE(ro->owns_slot());
    EXPECT_EQ(ro->self_index(), kMpRegistrySelfIndexUnset);

    // Header is the primary's: same memzone, same magic / total_procs.
    ASSERT_NE(ro->header(), nullptr);
    EXPECT_EQ(ro->header(), p->header());
    EXPECT_EQ(ro->header()->total_procs, 2u);

    // Critically: peer slot still free — readonly attach must not have
    // CAS-claimed anything.
    EXPECT_EQ(ro->header()->procs[1].claimed.load(std::memory_order_acquire),
              0);
}

TEST(MpRegistry, AttachSecondaryReadonly_NoPrimary_NotFound) {
    auto ro = MpRegistryHandle::attach_secondary_readonly("rgrdmiss");
    ASSERT_FALSE(ro.has_value());
    EXPECT_EQ(ro.error().code, Error::NotFound);
}

TEST(MpRegistry, TryClaimFreeSlot_ClaimsLowestFree) {
    // total_procs=4, primary at index 0 → readonly handle should be
    // able to claim index 1 first.
    auto p = MpRegistryHandle::create_primary(
        "rgtcfree", MpTopology::uniform(0, 4, 8));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto ro = MpRegistryHandle::attach_secondary_readonly("rgtcfree");
    ASSERT_TRUE(ro.has_value()) << ro.error();
    ASSERT_FALSE(ro->owns_slot());

    auto idx = ro->try_claim_free_slot();
    ASSERT_TRUE(idx.has_value()) << idx.error();
    EXPECT_EQ(*idx, 1u);  // primary owns 0 → lowest free is 1
    EXPECT_TRUE(ro->owns_slot());
    EXPECT_EQ(ro->self_index(), 1u);
    EXPECT_EQ(ro->header()->procs[1].claimed.load(std::memory_order_acquire),
              1);

    // Calling again on the same handle should error: handle already
    // owns a slot, no double-claim.
    auto again = ro->try_claim_free_slot();
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code, Error::InvalidConfig);
}

TEST(MpRegistry, TryClaimFreeSlot_AllClaimedReturnsOom) {
    // total_procs=2, primary at 0 → only index 1 free; manually claim
    // it so the next try_claim_free_slot must report OOM.
    auto p = MpRegistryHandle::create_primary("rgtcoom", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    // Claim slot 1 by hand to fully saturate the registry.
    auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(p->header());
    uint8_t exp = 0;
    ASSERT_TRUE(hdr->procs[1].claimed.compare_exchange_strong(
        exp, 1, std::memory_order_acq_rel));

    auto ro = MpRegistryHandle::attach_secondary_readonly("rgtcoom");
    ASSERT_TRUE(ro.has_value()) << ro.error();

    auto idx = ro->try_claim_free_slot();
    ASSERT_FALSE(idx.has_value());
    EXPECT_EQ(idx.error().code, Error::OutOfMemory);
    EXPECT_FALSE(ro->owns_slot());

    // Restore for clean shutdown — primary's destructor needs slot 0,
    // and we touched slot 1 by hand outside of any handle.
    hdr->procs[1].claimed.store(0, std::memory_order_release);
}

TEST(MpRegistry, AttachSecondary_AlreadyClaimedSkipsCAS) {
    // Reproduces the Mode 2 fragment: a try_claim_free_slot has
    // preclaimed slot 1; subsequent attach_secondary(already_claimed=
    // true) must succeed (without already_claimed it would fail with
    // "already claimed").
    auto p = MpRegistryHandle::create_primary("rgalrcl", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(p->header());
    uint8_t exp = 0;
    ASSERT_TRUE(hdr->procs[1].claimed.compare_exchange_strong(
        exp, 1, std::memory_order_acq_rel));

    // Without the bypass: the second attach would refuse on CAS.
    auto without = MpRegistryHandle::attach_secondary("rgalrcl", make_topo(1),
                                                      /*already_claimed=*/false);
    ASSERT_FALSE(without.has_value());
    EXPECT_EQ(without.error().code, Error::InvalidConfig);

    // With the bypass: slot is taken, but attach_secondary trusts the
    // caller and produces an owning handle.
    auto with = MpRegistryHandle::attach_secondary("rgalrcl", make_topo(1),
                                                   /*already_claimed=*/true);
    ASSERT_TRUE(with.has_value()) << with.error();
    EXPECT_EQ(with->self_index(), 1u);
    EXPECT_TRUE(with->owns_slot());

    // Sanity: dropping the bypass handle releases the slot via RAII so
    // the primary doesn't trip on stale state at teardown.
}

TEST(MpRegistry, AttachSecondary_AlreadyClaimedRequiresActualClaim) {
    // Caller contract: already_claimed=true must be paired with a real
    // preclaim. Pass it on a free slot → the helper detects the
    // mismatch and refuses to take ownership of an unclaimed slot.
    auto p = MpRegistryHandle::create_primary("rgalrnk", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    EXPECT_EQ(p->header()->procs[1].claimed.load(std::memory_order_acquire), 0);

    auto bad = MpRegistryHandle::attach_secondary("rgalrnk", make_topo(1),
                                                  /*already_claimed=*/true);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, Error::InvalidConfig);
}

// ─────────────────────────────────────────────────────────────────────────────
// count_alive_procs / is_last_alive_proc — gate API for primary teardown
// (introduced to fix the DPDK MP teardown-protocol violation in
//  Platform::Impl::cleanup() — see eph-net-dpdk/docs/dpdk-mp-teardown-protocol.md)
// ─────────────────────────────────────────────────────────────────────────────

TEST(MpRegistryAlive, InertHandle_CountIsZero) {
    MpRegistryHandle h;  // default-constructed inert
    EXPECT_FALSE(h.owns_slot());
    EXPECT_EQ(h.count_alive_procs(), 0u);
    EXPECT_FALSE(h.is_last_alive_proc());
}

TEST(MpRegistryAlive, PrimaryAlone_IsLast) {
    auto p = MpRegistryHandle::create_primary("rgalive1", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    EXPECT_EQ(p->count_alive_procs(), 1u);
    EXPECT_TRUE(p->is_last_alive_proc());
}

TEST(MpRegistryAlive, TwoProcs_NeitherIsLast) {
    auto p = MpRegistryHandle::create_primary("rgalive2", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto s = MpRegistryHandle::attach_secondary("rgalive2", make_topo(1));
    ASSERT_TRUE(s.has_value()) << s.error();

    EXPECT_EQ(p->count_alive_procs(), 2u);
    EXPECT_EQ(s->count_alive_procs(), 2u);
    EXPECT_FALSE(p->is_last_alive_proc());
    EXPECT_FALSE(s->is_last_alive_proc());
}

TEST(MpRegistryAlive, SecondaryReleased_PrimaryBecomesLast) {
    auto p = MpRegistryHandle::create_primary("rgalive3", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    {
        auto s = MpRegistryHandle::attach_secondary("rgalive3", make_topo(1));
        ASSERT_TRUE(s.has_value()) << s.error();
        EXPECT_EQ(p->count_alive_procs(), 2u);
        EXPECT_FALSE(p->is_last_alive_proc());
    }
    // RAII drop of `s` released its slot.
    EXPECT_EQ(p->count_alive_procs(), 1u);
    EXPECT_TRUE(p->is_last_alive_proc());
}

TEST(MpRegistryAlive, ThreeProcsTangledRelease_CountAccurate) {
    // 4-slot registry: primary at 0, secondaries at 1, 2, 3.
    auto p = MpRegistryHandle::create_primary(
        "rgalive4", MpTopology::uniform(0, 4, 8));
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_EQ(p->count_alive_procs(), 1u);

    {
        auto s1 = MpRegistryHandle::attach_secondary(
            "rgalive4", MpTopology::uniform(1, 4, 8));
        ASSERT_TRUE(s1.has_value()) << s1.error();
        EXPECT_EQ(p->count_alive_procs(), 2u);

        {
            auto s2 = MpRegistryHandle::attach_secondary(
                "rgalive4", MpTopology::uniform(2, 4, 8));
            ASSERT_TRUE(s2.has_value()) << s2.error();
            EXPECT_EQ(p->count_alive_procs(), 3u);

            {
                auto s3 = MpRegistryHandle::attach_secondary(
                    "rgalive4", MpTopology::uniform(3, 4, 8));
                ASSERT_TRUE(s3.has_value()) << s3.error();
                EXPECT_EQ(p->count_alive_procs(), 4u);
                EXPECT_FALSE(p->is_last_alive_proc());
            }
            // s3 released
            EXPECT_EQ(p->count_alive_procs(), 3u);
        }
        // s2 released — out-of-order release relative to s3
        EXPECT_EQ(p->count_alive_procs(), 2u);
    }
    // s1 released — primary is now alone again
    EXPECT_EQ(p->count_alive_procs(), 1u);
    EXPECT_TRUE(p->is_last_alive_proc());
}

// ─────────────────────────────────────────────────────────────────────────────
// lcore_mask cross-process conflict detection (v2 schema)
// ─────────────────────────────────────────────────────────────────────────────
//
// ProcSpec gained `uint64_t lcore_mask` in v2 to catch the silent
// "two procs same lcore" mental model gap. The PRIMARY gate is
// MpTopology::valid() pairwise overlap check — primary creation
// rejects an invalid topology before any cross-proc state is
// established. attach_secondary then enforces self-spec match
// (declared mask in header == secondary's own declared mask),
// which transitively prevents any peer from claiming a slot whose
// lcore declaration differs from the primary-written header.
//
// The tests below verify both layers.

namespace {
MpTopology make_topo_lcored(uint8_t self_index, uint8_t total_procs,
                            uint64_t self_mask, uint64_t peer_mask) {
    MpTopology t = MpTopology::uniform(self_index, total_procs,
                                       /*nb_rx_queues=*/4);
    // Symmetric: every non-self slot gets `peer_mask`. Self gets
    // `self_mask`. Tests can craft any overlap / disjoint scenario.
    for (uint8_t i = 0; i < total_procs; ++i) {
        t.procs[i].lcore_mask = (i == self_index) ? self_mask : peer_mask;
    }
    return t;
}
} // namespace

TEST(MpRegistryLcore, OptOut_BothZero_NoCheck) {
    // Default uniform() leaves lcore_mask=0; valid() skips lcore check.
    auto p = MpRegistryHandle::create_primary("rglc_opt", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto s = MpRegistryHandle::attach_secondary("rglc_opt", make_topo(1));
    ASSERT_TRUE(s.has_value()) << s.error();
}

TEST(MpRegistryLcore, DisjointMasks_Allowed) {
    // proc 0 = 0|1 (0x3), proc 1 = 2|3 (0xC). No overlap.
    auto p = MpRegistryHandle::create_primary(
        "rglc_disj", make_topo_lcored(0, 2, /*self*/0x3, /*peer*/0xC));
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_EQ(p->header()->procs[0].lcore_mask, 0x3u);
    EXPECT_EQ(p->header()->procs[1].lcore_mask, 0xCu);

    // Secondary declares matching topology (its own slot 0xC, peer 0x3).
    auto s = MpRegistryHandle::attach_secondary(
        "rglc_disj", make_topo_lcored(1, 2, /*self*/0xC, /*peer*/0x3));
    ASSERT_TRUE(s.has_value()) << s.error();
}

TEST(MpRegistryLcore, ValidRejectsFullOverlap) {
    // proc 0 and proc 1 both declare 0x3 → MpTopology::valid() fails.
    MpTopology t = make_topo_lcored(0, 2, /*self*/0x3, /*peer*/0x3);
    EXPECT_FALSE(t.valid());

    auto p = MpRegistryHandle::create_primary("rglc_dup", t);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error().code, Error::InvalidConfig);
}

TEST(MpRegistryLcore, ValidRejectsPartialOverlap) {
    // proc 0 = 0x7 (bits 0,1,2), proc 1 = 0xC (bits 2,3) → bit 2 collides.
    MpTopology t = make_topo_lcored(0, 2, /*self*/0x7, /*peer*/0xC);
    EXPECT_FALSE(t.valid());

    auto p = MpRegistryHandle::create_primary("rglc_par", t);
    ASSERT_FALSE(p.has_value());
}

TEST(MpRegistryLcore, ValidAllowsAsymmetricOptOut) {
    // proc 0 declares 0x3, proc 1 opts out (0). Mixed-mode is OK —
    // valid() only fires the lcore check when at least one slot is
    // tracked, but doesn't require all slots to be tracked.
    MpTopology t = make_topo_lcored(0, 2, /*self*/0x3, /*peer*/0);
    EXPECT_TRUE(t.valid());

    auto p = MpRegistryHandle::create_primary("rglc_asy", t);
    ASSERT_TRUE(p.has_value()) << p.error();

    // Secondary opts out (mask=0); declares matching peer view.
    auto s = MpRegistryHandle::attach_secondary(
        "rglc_asy", make_topo_lcored(1, 2, /*self*/0, /*peer*/0x3));
    ASSERT_TRUE(s.has_value()) << s.error();
}

TEST(MpRegistryLcore, AttachConflictWithLiveSlot_Rejected) {
    // Primary publishes its own slot's mask=0x3 to header. Secondary
    // attaches declaring its own (proc 1) mask=0x3 too — overlaps with
    // primary's published mask → cross-proc check fires.
    // (Self-spec match no longer compares lcore_mask, so secondary's
    // disagreement with primary's view of peer slots' masks doesn't
    // trigger spec mismatch — the cross-proc liveness scan does.)
    auto p_topo = make_topo_lcored(0, 2, /*self*/0x3, /*peer*/0);
    auto p = MpRegistryHandle::create_primary("rglc_lc", p_topo);
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_EQ(p->header()->procs[0].lcore_mask, 0x3u);

    // Secondary topo: own slot mask=0x3 (collides with primary's live mask)
    auto s_topo = make_topo_lcored(1, 2, /*self*/0x3, /*peer*/0);
    auto s = MpRegistryHandle::attach_secondary("rglc_lc", s_topo);
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().code, Error::InvalidConfig);
    EXPECT_NE(std::string{s.error().detail}.find("lcore_mask"),
              std::string::npos);
}

TEST(MpRegistryLcore, AttachPublishesOwnMask) {
    // Primary's topo only declares its own slot's mask. Secondary
    // attaches with its own mask; verify the slot's lcore_mask was
    // written to header by attach.
    auto p = MpRegistryHandle::create_primary(
        "rglc_pub", make_topo_lcored(0, 2, /*self*/0x3, /*peer*/0));
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_EQ(p->header()->procs[1].lcore_mask, 0u);  // peer slot empty

    auto s = MpRegistryHandle::attach_secondary(
        "rglc_pub", make_topo_lcored(1, 2, /*self*/0xC, /*peer*/0));
    ASSERT_TRUE(s.has_value()) << s.error();
    // After attach, secondary's mask is published to its own slot.
    EXPECT_EQ(p->header()->procs[1].lcore_mask, 0xCu);
}

TEST(MpRegistryLcore, AfterRelease_SlotReusable) {
    // Proc 1 takes lcores 2|3, releases. Re-attach OK.
    auto p = MpRegistryHandle::create_primary(
        "rglc_reu", make_topo_lcored(0, 2, /*self*/0x3, /*peer*/0xC));
    ASSERT_TRUE(p.has_value()) << p.error();

    {
        auto s = MpRegistryHandle::attach_secondary(
            "rglc_reu", make_topo_lcored(1, 2, /*self*/0xC, /*peer*/0x3));
        ASSERT_TRUE(s.has_value()) << s.error();
    }
    auto s2 = MpRegistryHandle::attach_secondary(
        "rglc_reu", make_topo_lcored(1, 2, /*self*/0xC, /*peer*/0x3));
    ASSERT_TRUE(s2.has_value()) << s2.error();
}
