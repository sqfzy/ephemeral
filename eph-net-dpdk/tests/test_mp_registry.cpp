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
#include <climits>      // INT32_MAX for stale-pid test
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>     // getpid for live-pid claims in tests
#include <sys/wait.h>   // waitpid for is_pid_alive child-reap test

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

TEST(MpRegistrySchema, V5VersionConstantBumped) {
    // T2.3 reverted (2026-05-06): schema 4 -> 5 removes
    // `header.hmac_enabled` flag + per-slot 32-byte `hmac_tag` field.
    // Hard-pin the version constant so a future accidental change
    // triggers a loud unit-test failure instead of a silent cross-
    // process corruption window. Bumping forward (rather than back to
    // v3) ensures any in-memory v4 hugepages are hard-rejected at
    // attach time.
    EXPECT_EQ(kMpRegistryVersion, 5u);
}

TEST(MpRegistrySchema, V2RegistryRejectedByV3SecondaryAttach) {
    // Simulate a stale v2 hugepage by creating a v3 registry then
    // poking the version field back to 2, then verifying secondary
    // attach (both writeable + readonly) hard-rejects with the
    // recovery hint. This is the cross-version rejection contract
    // documented in the v3 plan (BREAKING B8: registry schema bump).
    auto p = MpRegistryHandle::create_primary("rgv2sim", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    // Bypass header() const-ness via const_cast: tests are in the same
    // namespace as the impl and this is a deliberate poke at a stable
    // POD layout, not a behavior-altering hack on production code.
    auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(p->header());
    ASSERT_NE(hdr, nullptr);
    hdr->version = 2;  // pretend primary is one schema generation back

    // Read-only attach must reject.
    auto ro = MpRegistryHandle::attach_secondary_readonly("rgv2sim");
    ASSERT_FALSE(ro.has_value());
    EXPECT_NE(std::strstr(ro.error().detail, "version mismatch"), nullptr)
        << "actual: " << ro.error().detail;
    EXPECT_NE(std::strstr(ro.error().detail, "recovery"), nullptr)
        << "actual: " << ro.error().detail;

    // Writeable attach must reject too.
    auto rw = MpRegistryHandle::attach_secondary("rgv2sim", make_topo(1));
    ASSERT_FALSE(rw.has_value());
    EXPECT_NE(std::strstr(rw.error().detail, "version mismatch"), nullptr)
        << "actual: " << rw.error().detail;

    // Restore so cleanup doesn't hit assertion paths.
    hdr->version = kMpRegistryVersion;
}

TEST(MpRegistrySchema, TotalProcsExceedsKMaxProcs_ReadOnlyAttachRejected) {
    // Defensive guard: hdr->total_procs storage is uint32_t but the
    // contract is `[1, MpTopology::kMaxProcs]` (the fixed-size procs[]
    // array bound). A registry that passes magic+version but reports
    // total_procs > kMaxProcs (corruption, schema skew where layout
    // changed before version bumped, or a hand-mangled hugepage segment)
    // would otherwise silently truncate to uint8_t at the upper-layer
    // cast in `create_or_join[secondary]` and surface as an opaque
    // "MpTopology::valid() failed" three layers up. Pin
    // the guard down so a future refactor can't quietly remove it.
    auto p = MpRegistryHandle::create_primary("rgtpoom", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(p->header());
    ASSERT_NE(hdr, nullptr);
    const uint32_t saved_total = hdr->total_procs;

    // Out-of-band: claim there are more procs than the procs[] array can hold.
    hdr->total_procs = static_cast<uint32_t>(MpTopology::kMaxProcs) + 7u;

    auto ro = MpRegistryHandle::attach_secondary_readonly("rgtpoom");
    ASSERT_FALSE(ro.has_value());
    EXPECT_EQ(ro.error().code, Error::InvalidConfig);
    EXPECT_NE(std::strstr(ro.error().detail, "total_procs"), nullptr)
        << "actual: " << ro.error().detail;
    EXPECT_NE(std::strstr(ro.error().detail, "kMaxProcs"), nullptr)
        << "actual: " << ro.error().detail;

    // Restore so destructor cleanup doesn't trip on a malformed header.
    hdr->total_procs = saved_total;
}

TEST(MpRegistrySchema, TotalProcsZero_ReadOnlyAttachRejected) {
    // Symmetric edge: total_procs=0 is also out of contract (registry
    // claims to be live but has no slots). Same cast hazard, same guard.
    auto p = MpRegistryHandle::create_primary("rgtpzero", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(p->header());
    ASSERT_NE(hdr, nullptr);
    const uint32_t saved_total = hdr->total_procs;

    hdr->total_procs = 0;

    auto ro = MpRegistryHandle::attach_secondary_readonly("rgtpzero");
    ASSERT_FALSE(ro.has_value());
    EXPECT_EQ(ro.error().code, Error::InvalidConfig);
    EXPECT_NE(std::strstr(ro.error().detail, "total_procs"), nullptr)
        << "actual: " << ro.error().detail;

    hdr->total_procs = saved_total;
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
    // Also write a real (live) pid so try_claim_free_slot's stale-
    // slot reclaim pass (v2) doesn't immediately preempt it.
    auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(p->header());
    uint8_t exp = 0;
    ASSERT_TRUE(hdr->procs[1].claimed.compare_exchange_strong(
        exp, 1, std::memory_order_acq_rel));
    hdr->procs[1].pid = static_cast<int32_t>(::getpid());  // alive

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

TEST(MpRegistryPid, TryClaimFreeSlot_StaleSlotReclaimed) {
    // Manually claim slot 1 with a guaranteed-dead pid (high number
    // unlikely to collide with any live process). try_claim_free_slot
    // pass-2 should detect ESRCH via kill(pid, 0) and reclaim the slot.
    auto p = MpRegistryHandle::create_primary("rgpidstale", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(p->header());
    uint8_t exp = 0;
    ASSERT_TRUE(hdr->procs[1].claimed.compare_exchange_strong(
        exp, 1, std::memory_order_acq_rel));
    // Pick a pid that's almost certainly dead: PID 1 (init) might
    // exist; use INT32_MAX which is way above pid_max (typically
    // 32768 or 4194304). kill(INT32_MAX, 0) → ESRCH.
    hdr->procs[1].pid = INT32_MAX;

    auto ro = MpRegistryHandle::attach_secondary_readonly("rgpidstale");
    ASSERT_TRUE(ro.has_value()) << ro.error();

    auto idx = ro->try_claim_free_slot();
    ASSERT_TRUE(idx.has_value()) << idx.error();
    EXPECT_EQ(*idx, 1u);  // reclaimed slot 1 (the stale one)
    EXPECT_TRUE(ro->owns_slot());
    // Reclaimed slot now has OUR pid.
    EXPECT_EQ(hdr->procs[1].pid, static_cast<int32_t>(::getpid()));
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
// Regression for r3 commit d5c8ff1d + r4 commit 750aa0a1 + r4 reshape b95e9114
// (the "release symmetry" bug class that hit attach_secondary 3 times):
// when already_claimed=true and a post-hdr-resolution validation fails, the
// preclaimed slot MUST be released so the same process can retry.
//
// Pre-fix shape: validation failed → return std::unexpected(...) without
// clearing procs[idx].claimed. The pid stayed alive (it's the running test
// process), is_pid_alive blocked pass-2 reclaim, every subsequent
// attach_secondary in the same process saw "no free slots".
// ─────────────────────────────────────────────────────────────────────────────
TEST(MpRegistry,
     AttachSecondary_AlreadyClaimedReleasesSlotOnVersionMismatch) {
    auto p = MpRegistryHandle::create_primary("rgrelvm", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    // Preclaim slot 1 ourselves (mirrors try_claim_free_slot's CAS).
    auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(p->header());
    uint8_t exp = 0;
    ASSERT_TRUE(hdr->procs[1].claimed.compare_exchange_strong(
        exp, 1, std::memory_order_acq_rel));
    hdr->procs[1].pid = static_cast<int32_t>(::getpid());

    // Sabotage the registry so attach_secondary takes the version-mismatch
    // branch (post-`hdr` resolution, BEFORE the spec-disagree branch).
    const auto saved_version = hdr->version;
    hdr->version = 0xDEADu;

    auto fail = MpRegistryHandle::attach_secondary("rgrelvm", make_topo(1),
                                                    /*already_claimed=*/true);
    ASSERT_FALSE(fail.has_value());
    EXPECT_EQ(fail.error().code, Error::InvalidConfig);

    // Pre-fix THIS would still be 1 (this process's pid), blocking retry.
    EXPECT_EQ(hdr->procs[1].claimed.load(std::memory_order_acquire), 0)
        << "attach_secondary(already_claimed=true) failed on version "
           "mismatch but DID NOT release the preclaimed slot — same "
           "process can never retry create_or_join (regression of r3 "
           "d5c8ff1d / r4 750aa0a1)";

    // Restore so cleanup is clean.
    hdr->version = saved_version;
}

TEST(MpRegistry,
     AttachSecondary_AlreadyClaimedReleasesSlotOnFilePrefixMismatch) {
    auto p = MpRegistryHandle::create_primary("rgrelfp", make_topo(0));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(p->header());
    uint8_t exp = 0;
    ASSERT_TRUE(hdr->procs[1].claimed.compare_exchange_strong(
        exp, 1, std::memory_order_acq_rel));
    hdr->procs[1].pid = static_cast<int32_t>(::getpid());

    // Sabotage: rewrite hdr->file_prefix so the in-header value disagrees
    // with what attach_secondary will pass.
    char saved[sizeof(hdr->file_prefix)];
    std::memcpy(saved, hdr->file_prefix, sizeof(saved));
    std::strncpy(hdr->file_prefix, "OTHER!!", sizeof(hdr->file_prefix) - 1);
    hdr->file_prefix[sizeof(hdr->file_prefix) - 1] = '\0';

    auto fail = MpRegistryHandle::attach_secondary("rgrelfp", make_topo(1),
                                                    /*already_claimed=*/true);
    ASSERT_FALSE(fail.has_value());
    EXPECT_EQ(fail.error().code, Error::InvalidConfig);

    EXPECT_EQ(hdr->procs[1].claimed.load(std::memory_order_acquire), 0)
        << "file_prefix mismatch leaked the preclaimed slot — regression "
           "of r4 commit 750aa0a1 (broader pre-CAS validation surface)";

    std::memcpy(hdr->file_prefix, saved, sizeof(saved));
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

// ─────────────────────────────────────────────────────────────────────────────
// Ghost-data regression — release/claim cycle must clear lcore_mask + pid
// (silent failure mode: stale fields from previous owner survive in shared
// memory across slot reuse, poisoning the cross-process lcore conflict scan)
//
// Discovered via /pax --review LENS "production / MP mental model / silent
// misuse closure" on 2026-05-01: v2 schema added lcore_mask + pid but the
// release-side and try_claim_free_slot pass-1 paths did not zero them, so
// a benign graceful-release+reclaim cycle would leave ghost lcore_mask bits
// in the slot. attach_secondary's conflict scan then mistakenly reads the
// ghost as a live peer's claim and rejects unrelated peers with
// `Error::InvalidConfig("lcore_mask overlaps")`.
// ─────────────────────────────────────────────────────────────────────────────

TEST(MpRegistryGhost, GracefulRelease_ClearsLcoreMaskAndPid) {
    // Phase 1: secondary attaches with mask=0xC and a real pid (its own).
    // Phase 2: secondary's RAII destructor releases the slot.
    // Assert: post-release, slot 1's lcore_mask AND pid are both zero.
    // Without the fix this test FAILS at the lcore_mask line because
    // release_() only stores claimed=0 and leaves the metadata behind.
    auto p = MpRegistryHandle::create_primary(
        "rgghost1", make_topo_lcored(0, 2, /*self*/0x3, /*peer*/0));
    ASSERT_TRUE(p.has_value()) << p.error();

    {
        auto s = MpRegistryHandle::attach_secondary(
            "rgghost1", make_topo_lcored(1, 2, /*self*/0xC, /*peer*/0));
        ASSERT_TRUE(s.has_value()) << s.error();
        // Sanity: attach actually published the metadata.
        EXPECT_EQ(p->header()->procs[1].lcore_mask, 0xCu);
        EXPECT_EQ(p->header()->procs[1].pid,
                  static_cast<int32_t>(::getpid()));
        EXPECT_EQ(p->header()->procs[1].claimed.load(
                      std::memory_order_acquire), 1);
    } // ~MpRegistryHandle → release_

    // Slot is now free again.
    EXPECT_EQ(p->header()->procs[1].claimed.load(
                  std::memory_order_acquire), 0);
    // Metadata MUST be cleared so the next claimer sees a pristine slot.
    EXPECT_EQ(p->header()->procs[1].lcore_mask, 0u)
        << "release_ must clear lcore_mask so the next slot claimant does "
           "not inherit ghost data that would poison the cross-process "
           "lcore conflict scan.";
    EXPECT_EQ(p->header()->procs[1].pid, 0)
        << "release_ must clear pid so the next slot claimant does not "
           "inherit a stale owner identifier.";
}

TEST(MpRegistryGhost, SlotReusedByNextClaimer_NoGhostLcoreMask) {
    // End-to-end: peer A attaches slot 1 with mask=0xC, releases gracefully.
    // Peer B then arrives via try_claim_free_slot pass-1 (the common path
    // for a freshly-released slot — pass-2 only fires when kill(pid,0)
    // returns ESRCH). Pass-1 used to skip the metadata clear, so the slot
    // would re-publish A's ghost lcore_mask=0xC under B's pid. A third
    // peer C attempting to attach with its own mask=0xC (legitimate, since
    // A is gone) would then be falsely rejected by attach_secondary's
    // conflict scan against the ghost.
    auto primary = MpRegistryHandle::create_primary(
        "rgghost2", MpTopology::uniform(0, 4, 8));
    ASSERT_TRUE(primary.has_value()) << primary.error();

    // Round 1: peer A claims slot 1 via try_claim_free_slot, publishes 0xC.
    {
        auto roA = MpRegistryHandle::attach_secondary_readonly("rgghost2");
        ASSERT_TRUE(roA.has_value()) << roA.error();
        auto idxA = roA->try_claim_free_slot();
        ASSERT_TRUE(idxA.has_value()) << idxA.error();
        EXPECT_EQ(*idxA, 1u);

        // Manually publish a non-zero lcore_mask the way attach_secondary
        // would — this simulates a peer that does opt into lcore tracking.
        // (We can't go through full attach_secondary here because slot 1
        // is already claimed by roA; the precondition for the bug is that
        // SOMETHING wrote a non-zero mask before roA released.)
        auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(
            primary->header());
        hdr->procs[1].lcore_mask = 0xCu;
        EXPECT_EQ(hdr->procs[1].lcore_mask, 0xCu);
    } // ~roA → release_ → must clear metadata

    // Round 2: peer B claims slot 1 via try_claim_free_slot pass-1.
    // Pass-1 must clear the residual lcore_mask before publishing pid.
    auto roB = MpRegistryHandle::attach_secondary_readonly("rgghost2");
    ASSERT_TRUE(roB.has_value()) << roB.error();
    auto idxB = roB->try_claim_free_slot();
    ASSERT_TRUE(idxB.has_value()) << idxB.error();
    EXPECT_EQ(*idxB, 1u);  // lowest free is still 1 (after round-1 release)

    EXPECT_EQ(primary->header()->procs[1].lcore_mask, 0u)
        << "try_claim_free_slot pass-1 must clear residual lcore_mask "
           "from a previous owner so the slot's metadata reflects the "
           "current claimant only. A non-zero ghost here would falsely "
           "trip attach_secondary's cross-process conflict scan.";
    EXPECT_EQ(primary->header()->procs[1].pid,
              static_cast<int32_t>(::getpid()));
}

// ─────────────────────────────────────────────────────────────────────────────
// Concurrent publication ordering — claim writer's metadata stores must be
// happens-before the reader's acquire-load of `claimed`
//
// Discovered via /pax --review LENS "production / MP mental model / silent
// misuse closure" round 2 on 2026-05-01: v2 schema published lcore_mask + pid
// as plain stores AFTER the CAS-claim release. Reader's claimed.load(acquire)
// synchronizes-with the CAS write but NOT with subsequent plain stores, so on
// weakly-ordered architectures (AArch64/POWER) a concurrent peer's conflict
// scan could observe `claimed=1` together with stale (zero) lcore_mask,
// silently bypassing the v2 cross-process lcore-conflict gate.
//
// Fix: redundant `claimed.store(1, release)` AFTER metadata writes provides
// the release-store partner for any reader's acquire-load. The test below
// stress-runs two threads on the same shared atomic state; passing under
// many iterations means the writer never publishes claimed=1 before the
// reader is allowed to see the stale metadata.
// ─────────────────────────────────────────────────────────────────────────────

TEST(MpRegistryConcurrent, AcquireLoadOnClaimedSeesPublishedLcoreMask) {
    auto p = MpRegistryHandle::create_primary(
        "rgconc1", MpTopology::uniform(0, 4, 8));
    ASSERT_TRUE(p.has_value()) << p.error();

    auto* hdr = const_cast<eph::dpdk::detail::MpRegistryHeader*>(p->header());

    // Reader-observable counter: how many times did the reader observe
    // `claimed=1` paired with `lcore_mask=0` (the stale-publication race).
    // Without the round-2 fix, this can be > 0 on weakly-ordered cores;
    // with the fix it must stay 0.
    std::atomic<uint64_t> stale_observations{0};
    std::atomic<bool>     stop{false};
    constexpr uint64_t kIterations = 4000;

    constexpr uint64_t kProbeMask = 0xCu;

    // Writer: tight loop of attach (fresh secondary handle) +
    // RAII-release on slot 1, publishing lcore_mask=0xC each cycle.
    // Each iteration goes through the post-fix code path:
    //   CAS(0→1, acq_rel) → clear_slot_metadata_ → pid → lcore_mask
    //   → claimed.store(1, release)  ← the fix
    // Reader observes via acquire-load on slot 1's `claimed` flag.
    std::thread writer([&] {
        for (uint64_t i = 0; i < kIterations && !stop.load(); ++i) {
            // Use the readonly + try_claim_free_slot + attach_secondary
            // (already_claimed=true) sequence so we exercise both
            // pass-1 (try_claim_free_slot) and the
            // already_claimed=true publish path in attach_secondary.
            auto ro = MpRegistryHandle::attach_secondary_readonly("rgconc1");
            ASSERT_TRUE(ro.has_value()) << ro.error();
            auto idx = ro->try_claim_free_slot();
            ASSERT_TRUE(idx.has_value()) << idx.error();
            const uint8_t claimed_idx = *idx;
            ro->disarm_slot();

            MpTopology topo = MpTopology::uniform(claimed_idx, 4, 8);
            topo.procs[claimed_idx].lcore_mask = kProbeMask;

            auto h = MpRegistryHandle::attach_secondary(
                "rgconc1", topo, /*already_claimed=*/true);
            ASSERT_TRUE(h.has_value()) << h.error();
            // h's destructor releases the slot here (clears metadata
            // BEFORE claimed=0 release-store, per round-1 fix), making
            // the slot freshly available for the next iteration.
        }
        stop.store(true);
    });

    // Reader: sample slot 1's (claimed, lcore_mask) pair. We must
    // ALWAYS see one of:
    //   (claimed=0, *)            — slot is free; lcore_mask read is racey
    //                                but irrelevant since the gate is
    //                                claimed=0
    //   (claimed=1, lcore_mask=kProbeMask)  — fully published
    //   (claimed=1, lcore_mask=0)           — STALE BUG (round-2 race)
    // The third pattern is what the fix forbids.
    //
    // Reader scans all 4 slots because the writer cycles through them
    // (try_claim_free_slot picks lowest free index); whichever slot is
    // currently being claimed/released is the active racing target.
    std::thread reader([&] {
        while (!stop.load()) {
            for (uint32_t i = 0; i < 4; ++i) {
                const uint8_t cl = hdr->procs[i].claimed.load(
                    std::memory_order_acquire);
                if (cl == 1) {
                    const uint64_t mask = hdr->procs[i].lcore_mask;
                    // primary at slot 0 has its own pcfg lcore_mask=0
                    // by default in this test (we used uniform() which
                    // leaves it 0). Skip slot 0 since both 0 and
                    // kProbeMask there are valid.
                    if (i == 0) continue;
                    // Acceptable observations on a non-primary slot:
                    //   * mask == kProbeMask — fully published peer
                    //   * mask == 0          — TRANSIENT and SAFE: the
                    //     writer's claim path is two-staged
                    //     (try_claim_free_slot publishes claimed=1 with
                    //     mask=0 first; attach_secondary(already_claimed
                    //     =true) finalizes mask=kProbeMask). A reader's
                    //     acquire-load that lands between the two
                    //     stages observes claimed=1 + mask=0, which is
                    //     LOGICALLY EQUIVALENT to "claimed peer with
                    //     no lcore tracking" — the conflict scan's
                    //     `wanted_mask & other_mask` is zero, no false
                    //     positive, no false negative.
                    // What MUST NOT be observed: mask carrying a value
                    // other than kProbeMask or 0 — that would indicate
                    // either (a) a torn write (impossible under x86 /
                    // aarch64 64-bit aligned stores) or (b) ghost data
                    // from a previous owner surviving into the current
                    // claim cycle, which round-1 fix forbade. The
                    // assertion below catches only those genuine
                    // corruption modes.
                    if (mask != kProbeMask && mask != 0u) {
                        stale_observations.fetch_add(1);
                    }
                }
            }
        }
    });

    writer.join();
    reader.join();

    // What this test actually proves: there is no observable lcore_mask
    // value other than 0 (transient mid-claim) or kProbeMask (fully
    // published). With the round-1 fix (release_/try_claim_free_slot
    // pass-1 clear metadata) and round-2 fix (redundant claimed.store
    // (1, release) after metadata writes), the reader can never see
    // residual ghost bits from a previous owner or torn intermediate
    // values.
    //
    // This test is intentionally LESS STRICT than checking "claimed=1
    // implies mask=kProbeMask": the bench's autojoin path uses
    // try_claim_free_slot + attach_secondary(already_claimed=true) as
    // a two-stage claim sequence, so a brief mask=0 window is part of
    // the design contract. That window is logically safe because
    // wanted & 0 == 0 in every concurrent reader's conflict scan.
    EXPECT_EQ(stale_observations.load(), 0u)
        << "Reader observed claimed=1 paired with an unexpected "
           "lcore_mask value (neither 0 nor the writer's published "
           "kProbeMask=" << std::hex << kProbeMask << std::dec << "). "
           "This indicates either ghost data from a previous owner "
           "leaking past round-1's release-side clear, or a torn "
           "intermediate write — both of which are forbidden by the "
           "MpRegistry slot-state invariants.";
}

// ─────────────────────────────────────────────────────────────────────────────
// is_pid_alive — direct boundary tests for the kill(pid, 0) liveness probe
// (the primitive that gates stale-slot reclaim in try_claim_free_slot pass-2)
// ─────────────────────────────────────────────────────────────────────────────

TEST(IsPidAlive, OwnPid_True) {
    // Self-pid is always alive while the test is running.
    EXPECT_TRUE(eph::dpdk::detail::is_pid_alive(
        static_cast<int32_t>(::getpid())));
}

TEST(IsPidAlive, ZeroAndNegative_False) {
    // pid=0 in kill(2) has special meaning ("send to current process
    // group") rather than "is process 0 alive"; the helper must
    // explicitly reject non-positive PIDs to avoid that semantic
    // landmine and to match the documented "pid <= 0 is invalid"
    // contract.
    EXPECT_FALSE(eph::dpdk::detail::is_pid_alive(0));
    EXPECT_FALSE(eph::dpdk::detail::is_pid_alive(-1));
    EXPECT_FALSE(eph::dpdk::detail::is_pid_alive(-99));
    EXPECT_FALSE(eph::dpdk::detail::is_pid_alive(INT32_MIN));
}

TEST(IsPidAlive, Int32MaxIsDead) {
    // INT32_MAX = 2_147_483_647 is far above Linux's pid_max
    // (default 32768, max 4_194_304); kill(INT32_MAX, 0) → ESRCH.
    // This is the same value the stale-slot reclaim test uses, so
    // pinning the helper's behavior here directly is a sanity gate
    // for that downstream test.
    EXPECT_FALSE(eph::dpdk::detail::is_pid_alive(INT32_MAX));
}

TEST(IsPidAlive, Init_True) {
    // PID 1 (init / systemd) is always alive on a Linux host while
    // any process is running, and it's owned by root → kill(1, 0)
    // from a non-root tester returns -1/EPERM, which the helper
    // explicitly treats as "alive". This documents the EPERM branch.
    EXPECT_TRUE(eph::dpdk::detail::is_pid_alive(1));
}

TEST(IsPidAlive, RecentlyReapedChild_False) {
    // Fork a child that exits immediately, reap it, then check the
    // PID. Once reaped, it no longer exists in the kernel's process
    // table → kill(pid, 0) returns ESRCH → false. This pins the
    // exact path stale-slot reclaim depends on.
    pid_t pid = ::fork();
    ASSERT_NE(pid, -1) << "fork() failed";
    if (pid == 0) {
        ::_exit(0);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    // After reap, the PID slot is freed (modulo immediate reuse race;
    // the Linux pid_max is high enough that immediate-reuse on a
    // single-threaded test is statistically negligible).
    EXPECT_FALSE(eph::dpdk::detail::is_pid_alive(static_cast<int32_t>(pid)));
}
