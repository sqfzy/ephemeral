/// @file test_icmp_directory.cpp
/// Unit tests for `detail::IcmpDirectoryHandle` — hugepage-backed
/// cross-process ICMP target directory.
///
/// Boots EAL via `dpdk_test_env` (--no-pci --no-huge); single-process
/// tests cover the POD layout invariants, register/lookup/unregister
/// round-trip, generation bumps, full-table behaviour, and RAII.
/// Cross-process behaviour is exercised by the stage-3 e2e binary.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp"

#include "eph/core/error.hpp"
#include "eph/dpdk/detail/icmp_directory.hpp"
#include "eph/dpdk/packet_core.hpp"
#include "eph/net/hmac.hpp"

using eph::core::Error;
using eph::dpdk::net::ConnectionTuple;
using eph::dpdk::detail::build_icmp_directory_name;
using eph::dpdk::detail::IcmpDirectoryEntry;
using eph::dpdk::detail::IcmpDirectoryHandle;
using eph::dpdk::detail::IcmpDirectoryHeader;
using eph::dpdk::detail::kIcmpDirectoryFilePrefixMax;
using eph::dpdk::detail::kIcmpDirectoryMagic;
using eph::dpdk::detail::kIcmpDirectoryMaxEntries;
using eph::dpdk::detail::kIcmpDirectoryNoOwner;
using eph::dpdk::detail::kIcmpDirectoryVersion;

// ─────────────────────────────────────────────────────────────────────────────
// POD layout — pure compile-time + value tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(IcmpDirectoryLayout, EntryIsTriviallyCopyable) {
    static_assert(std::is_trivially_copyable_v<IcmpDirectoryEntry>);
    static_assert(std::is_trivially_copyable_v<IcmpDirectoryHeader>);
    SUCCEED();
}

TEST(IcmpDirectoryLayout, HeaderCachelineAligned) {
    static_assert(alignof(IcmpDirectoryHeader) >= 64);
    SUCCEED();
}

TEST(IcmpDirectoryLayout, MaxEntriesIs1024) {
    EXPECT_EQ(kIcmpDirectoryMaxEntries, 1024u);
}

// ─────────────────────────────────────────────────────────────────────────────
// build_icmp_directory_name
// ─────────────────────────────────────────────────────────────────────────────

TEST(IcmpDirectoryName, PrefixedAndNullTerminated) {
    auto r = build_icmp_directory_name("demo");
    ASSERT_TRUE(r.has_value());
    EXPECT_STREQ(r->data(), "eph_mp_icmp/demo");
}

TEST(IcmpDirectoryName, EmptyFilePrefix_Rejected) {
    auto r = build_icmp_directory_name("");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(IcmpDirectoryName, OverMaxLengthRejected) {
    std::string fp(kIcmpDirectoryFilePrefixMax + 1, 'x');
    auto r = build_icmp_directory_name(fp);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(IcmpDirectoryName, AtMaxLengthAccepted) {
    std::string fp(kIcmpDirectoryFilePrefixMax, 'x');
    auto r = build_icmp_directory_name(fp);
    ASSERT_TRUE(r.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// IcmpDirectoryHandle — full lifecycle (needs EAL)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

ConnectionTuple make_tuple(uint16_t base) {
    return ConnectionTuple{
        .src_ip   = 0x0A000001,                // 10.0.0.1
        .dst_ip   = 0x0A000002,                // 10.0.0.2
        .src_port = static_cast<uint16_t>(base),
        .dst_port = static_cast<uint16_t>(base + 1),
    };
}

constexpr uint8_t kProtoTcp = 6;
constexpr uint8_t kProtoUdp = 17;

} // namespace

TEST(IcmpDirectory, CreatePrimary_WritesHeader) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_a");
    ASSERT_TRUE(h.has_value()) << h.error();
    ASSERT_TRUE(static_cast<bool>(*h));

    const auto* hdr = h->header();
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->magic, kIcmpDirectoryMagic);
    EXPECT_EQ(hdr->version, kIcmpDirectoryVersion);
    EXPECT_EQ(hdr->max_entries, kIcmpDirectoryMaxEntries);
    EXPECT_STREQ(hdr->file_prefix, "idtest_a");

    // All entries should start free.
    for (size_t i = 0; i < hdr->max_entries; ++i) {
        EXPECT_EQ(hdr->entries[i].claimed.load(std::memory_order_acquire), 0);
        EXPECT_EQ(hdr->entries[i].owner_proc, kIcmpDirectoryNoOwner);
    }
}

TEST(IcmpDirectory, AttachSecondary_LookupSucceeds) {
    auto p = IcmpDirectoryHandle::create_primary("idtest_b");
    ASSERT_TRUE(p.has_value()) << p.error();

    auto s = IcmpDirectoryHandle::attach_secondary("idtest_b");
    ASSERT_TRUE(s.has_value()) << s.error();
    EXPECT_EQ(p->header(), s->header());   // same backing memzone
}

TEST(IcmpDirectory, AttachSecondary_NoPrimary_NotFound) {
    auto s = IcmpDirectoryHandle::attach_secondary("idtest_noprim");
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().code, Error::NotFound);
}

TEST(IcmpDirectorySchema, MaxEntriesDisagrees_AttachSecondaryRejected) {
    // Defensive guard: hdr->max_entries storage is uint32_t but the
    // contract is the compile-time constant kIcmpDirectoryMaxEntries
    // (also the static `entries[]` array bound). Past magic+version,
    // attach_secondary previously had no range check; multiple
    // mutators (register_target Pass 1/2, post-CAS rescan, list_targets)
    // and slot-index guards (release_slot, lookup_owner) use this field
    // as a bound, so a corrupt bigger value would turn into an out-of-
    // range read on the fixed-size array. Pin the strict-equality
    // contract down so a future refactor can't quietly soften it.
    auto p = IcmpDirectoryHandle::create_primary("idtest_meqcorrupt");
    ASSERT_TRUE(p.has_value()) << p.error();

    auto* hdr = const_cast<eph::dpdk::detail::IcmpDirectoryHeader*>(p->header());
    ASSERT_NE(hdr, nullptr);
    const uint32_t saved = hdr->max_entries;
    hdr->max_entries = static_cast<uint32_t>(kIcmpDirectoryMaxEntries) + 1024u;

    auto s = IcmpDirectoryHandle::attach_secondary("idtest_meqcorrupt");
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().code, Error::InvalidConfig);
    EXPECT_NE(std::strstr(s.error().detail, "max_entries"), nullptr)
        << "actual: " << s.error().detail;
    EXPECT_NE(std::strstr(s.error().detail, "kIcmpDirectoryMaxEntries"),
              nullptr)
        << "actual: " << s.error().detail;

    // Restore so cleanup doesn't trip on a malformed header.
    hdr->max_entries = saved;
}

TEST(IcmpDirectorySchema, MaxEntriesZero_AttachSecondaryRejected) {
    auto p = IcmpDirectoryHandle::create_primary("idtest_meqzero");
    ASSERT_TRUE(p.has_value()) << p.error();

    auto* hdr = const_cast<eph::dpdk::detail::IcmpDirectoryHeader*>(p->header());
    ASSERT_NE(hdr, nullptr);
    const uint32_t saved = hdr->max_entries;
    hdr->max_entries = 0;

    auto s = IcmpDirectoryHandle::attach_secondary("idtest_meqzero");
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().code, Error::InvalidConfig);
    EXPECT_NE(std::strstr(s.error().detail, "max_entries"), nullptr)
        << "actual: " << s.error().detail;

    hdr->max_entries = saved;
}

TEST(IcmpDirectory, RegisterTarget_AllocatesEntry_LookupHits) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_reg");
    ASSERT_TRUE(h.has_value()) << h.error();

    auto t = make_tuple(50000);
    auto idx = h->register_target(t, kProtoTcp, /*owner=*/0);
    ASSERT_TRUE(idx.has_value()) << idx.error();
    EXPECT_LT(*idx, kIcmpDirectoryMaxEntries);

    auto found = h->lookup(t, kProtoTcp);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->owner_proc, 0);
    EXPECT_EQ(found->slot_idx, *idx);
    EXPECT_EQ(found->generation, 0u);   // fresh slot
}

TEST(IcmpDirectory, RegisterTarget_DuplicateRejected) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_dup");
    ASSERT_TRUE(h.has_value()) << h.error();

    auto t = make_tuple(50100);
    ASSERT_TRUE(h->register_target(t, kProtoTcp, 0).has_value());

    // same tuple+proto → InvalidConfig
    auto dup = h->register_target(t, kProtoTcp, 1);
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, Error::InvalidConfig);

    // same tuple but different proto → distinct slot, succeeds
    auto udp_idx = h->register_target(t, kProtoUdp, 0);
    EXPECT_TRUE(udp_idx.has_value()) << udp_idx.error();
}

TEST(IcmpDirectory, RegisterTarget_NoOwnerSentinel_Rejected) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_sentinel");
    ASSERT_TRUE(h.has_value()) << h.error();

    auto t = make_tuple(50200);
    auto bad = h->register_target(t, kProtoTcp, kIcmpDirectoryNoOwner);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, Error::InvalidConfig);
}

TEST(IcmpDirectory, Unregister_BumpsGeneration_LookupMisses) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_unreg");
    ASSERT_TRUE(h.has_value()) << h.error();

    auto t = make_tuple(50300);
    auto idx = h->register_target(t, kProtoTcp, 0);
    ASSERT_TRUE(idx.has_value());
    EXPECT_EQ(h->lookup(t, kProtoTcp)->generation, 0u);

    h->unregister(*idx);

    EXPECT_FALSE(h->lookup(t, kProtoTcp).has_value());
    EXPECT_FALSE(h->is_slot_alive(*idx, /*expected_gen=*/0));

    // Re-register same tuple → reuses some slot, but gen on the
    // previously-released slot has bumped to 1.
    auto re_idx = h->register_target(t, kProtoTcp, 1);
    ASSERT_TRUE(re_idx.has_value());
    // Slot may be reused (linear-scan picks first free); check the
    // released slot specifically.
    EXPECT_EQ(h->header()->entries[*idx].generation.load(
                  std::memory_order_acquire),
              1u);
}

TEST(IcmpDirectory, Unregister_OnFreeSlot_IsIdempotentNoGenBump) {
    // Regression: a caller (test path / future introspection tool /
    // RAII guard double-release after move) calling unregister on a
    // slot that is already free MUST NOT bump generation or clobber
    // fields. The naive (pre-2026-05-03) implementation bumped gen
    // unconditionally, so a stale double-unregister would silently
    // invalidate a future re-registration into the same slot — the
    // legitimate owner's in-flight IPC dispatches would then drop as
    // "stale gen" with no diagnostic.
    //
    // Pin shape: register → unregister (gen: 0 → 1) → unregister
    // again on the same now-free slot → assert gen still 1.
    auto h = IcmpDirectoryHandle::create_primary("idtest_unreg_idem");
    ASSERT_TRUE(h.has_value()) << h.error();

    auto t = make_tuple(50301);
    auto idx = h->register_target(t, kProtoTcp, 0);
    ASSERT_TRUE(idx.has_value());

    // First unregister: gen 0 → 1.
    h->unregister(*idx);
    const uint32_t gen_after_first =
        h->header()->entries[*idx].generation.load(std::memory_order_acquire);
    EXPECT_EQ(gen_after_first, 1u);

    // Repeat unregister on the now-free slot — must be no-op (free-
    // slot guard returns early before the ++gen).
    h->unregister(*idx);
    EXPECT_EQ(h->header()->entries[*idx].generation.load(
                  std::memory_order_acquire),
              gen_after_first)
        << "unregister on already-free slot bumped generation; the "
           "next re-registration would observe a non-zero baseline gen "
           "and any older IPC dispatch carrying gen==1 would be dropped";

    // Idempotent re-call x3 to make the no-op shape unambiguous.
    h->unregister(*idx);
    h->unregister(*idx);
    h->unregister(*idx);
    EXPECT_EQ(h->header()->entries[*idx].generation.load(
                  std::memory_order_acquire),
              gen_after_first);
}

TEST(IcmpDirectory, IsSlotAlive_GenMatchAndMismatch) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_alive");
    ASSERT_TRUE(h.has_value()) << h.error();

    auto t = make_tuple(50400);
    auto idx = h->register_target(t, kProtoTcp, 2);
    ASSERT_TRUE(idx.has_value());
    EXPECT_TRUE(h->is_slot_alive(*idx, 0u));
    EXPECT_FALSE(h->is_slot_alive(*idx, 99u));   // wrong gen
    EXPECT_FALSE(h->is_slot_alive(99999, 0u));   // OOB slot
}

TEST(IcmpDirectory, RegisterFull_ReturnsOom) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_full");
    ASSERT_TRUE(h.has_value()) << h.error();

    // Fill the directory. Use distinct src_port to dodge dup check.
    for (size_t i = 0; i < kIcmpDirectoryMaxEntries; ++i) {
        ConnectionTuple t{
            .src_ip   = 0x0A000001,
            .dst_ip   = 0x0A000002,
            .src_port = static_cast<uint16_t>(40000 + i),
            .dst_port = 80,
        };
        auto idx = h->register_target(t, kProtoTcp, 0);
        ASSERT_TRUE(idx.has_value())
            << "iter=" << i << " err=" << idx.error();
    }

    // One more → OutOfMemory
    ConnectionTuple t_extra{
        .src_ip   = 0x0A000001,
        .dst_ip   = 0x0A000002,
        .src_port = 41024,
        .dst_port = 80,
    };
    auto extra = h->register_target(t_extra, kProtoTcp, 0);
    ASSERT_FALSE(extra.has_value());
    EXPECT_EQ(extra.error().code, Error::OutOfMemory);
}

TEST(IcmpDirectory, MoveSemantics_TransferOwnership) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_mv");
    ASSERT_TRUE(h.has_value()) << h.error();
    const auto* hdr_before = h->header();

    IcmpDirectoryHandle moved = std::move(*h);
    EXPECT_FALSE(static_cast<bool>(*h));
    EXPECT_TRUE(static_cast<bool>(moved));
    EXPECT_EQ(moved.header(), hdr_before);
}

// ─────────────────────────────────────────────────────────────────────────────
// TOCTOU race regression — concurrent register_target() with the same
// (tuple, proto) MUST converge to a single owner. Pre-fix, the dup-check
// in Pass 1 and CAS-claim in Pass 2 had no synchronisation between them,
// so two concurrent callers could both pass Pass 1 and both write the
// same payload to disjoint slots, leaving the directory with two entries
// owning the same tuple. The post-claim re-scan added in the fix yields
// the higher-index slot back to the OutOfMemory pool and reports the dup
// to one of the callers.
//
// We run many trials with N producer threads each calling register_target
// for the SAME tuple. Exactly one must succeed; all others must report
// `Error::InvalidConfig` ("already registered" or "lost TOCTOU race").
// After all threads join, exactly one slot is claimed in the directory.
// ─────────────────────────────────────────────────────────────────────────────

#include <thread>
#include <vector>

TEST(IcmpDirectory, ConcurrentRegisterSameTuple_ExactlyOneWinner) {
    auto h_opt = IcmpDirectoryHandle::create_primary("idtest_race");
    ASSERT_TRUE(h_opt.has_value()) << h_opt.error();
    auto& h = *h_opt;

    constexpr int kThreads = 8;
    constexpr int kTrials  = 50;
    constexpr uint8_t kProtoTcp = 6;

    int total_winners = 0;
    int total_dups    = 0;

    for (int trial = 0; trial < kTrials; ++trial) {
        // Build a unique tuple per trial so no cross-trial residue.
        ConnectionTuple t{
            .src_ip   = 0x0a000001u,
            .dst_ip   = 0xC0A80101u,
            .src_port = static_cast<uint16_t>(40000 + trial),
            .dst_port = 443,
        };

        std::atomic<int> winners{0};
        std::atomic<int> dups{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);

        for (int th = 0; th < kThreads; ++th) {
            threads.emplace_back([&, th]() {
                // Spin-wait so all threads start the CAS-claim race
                // around the same instant — maximises contention.
                while (!start.load(std::memory_order_acquire)) {
                    // busy spin
                }
                auto r = h.register_target(
                    t, kProtoTcp, /*owner_proc=*/static_cast<uint8_t>(th));
                if (r.has_value()) {
                    winners.fetch_add(1, std::memory_order_relaxed);
                } else {
                    EXPECT_EQ(r.error().code, Error::InvalidConfig)
                        << "expected dup-class error, got: " << r.error();
                    dups.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& th : threads) th.join();

        // Invariant 1: exactly one CAS-claim wins.
        EXPECT_EQ(winners.load(), 1)
            << "trial " << trial << ": expected exactly 1 winner, got "
            << winners.load();
        // Invariant 2: every other thread reports a dup-class error.
        EXPECT_EQ(dups.load(), kThreads - 1)
            << "trial " << trial << ": expected " << kThreads - 1
            << " dup errors, got " << dups.load();

        // Invariant 3: lookup returns exactly one entry for this tuple.
        auto found = h.lookup(t, kProtoTcp);
        ASSERT_TRUE(found.has_value())
            << "trial " << trial << ": tuple lost after race";

        // Invariant 4: scanning the entire directory finds exactly one
        // claimed entry matching the tuple — no duplicate slots survive.
        size_t live_count = 0;
        for (size_t i = 0; i < kIcmpDirectoryMaxEntries; ++i) {
            const auto& e = h.header()->entries[i];
            if (e.claimed.load(std::memory_order_acquire) == 0) continue;
            if (e.proto == kProtoTcp &&
                e.src_ip == t.src_ip && e.dst_ip == t.dst_ip &&
                e.src_port == t.src_port && e.dst_port == t.dst_port) {
                ++live_count;
            }
        }
        EXPECT_EQ(live_count, 1u)
            << "trial " << trial << ": found " << live_count
            << " entries for the racing tuple — directory corrupt";

        total_winners += winners.load();
        total_dups    += dups.load();

        // Clean up so the next trial isn't blocked by accumulating state.
        h.unregister(found->slot_idx);
    }

    EXPECT_EQ(total_winners, kTrials);
    EXPECT_EQ(total_dups, kTrials * (kThreads - 1));
}

// ─────────────────────────────────────────────────────────────────────────────
// audit_sweep_one_round — cursor advancement + wrap-around
// ─────────────────────────────────────────────────────────────────────────────
//
// The handle's docstring claims `audit_sweep_one_round(batch)` advances
// the cursor across calls and wraps to 0 once a full pass is complete,
// but this was previously untested. These cases pin the contract so a
// future refactor breaking cursor arithmetic fails loudly here.

TEST(IcmpDirectory, AuditSweep_ZeroWhenHmacDisabled) {
    // Default unkeyed mode: hmac_enabled=0, sweep is a deterministic
    // no-op even when populated entries exist.
    auto h = IcmpDirectoryHandle::create_primary("idtest_swdis");
    ASSERT_TRUE(h.has_value()) << h.error();

    ASSERT_TRUE(h->register_target(make_tuple(40010), kProtoTcp, 0).has_value());
    ASSERT_TRUE(h->register_target(make_tuple(40020), kProtoTcp, 0).has_value());

    // hmac_enabled is 0, so audit_sweep returns 0 mismatches without
    // running HMAC over any entry. Repeated calls stay at 0.
    EXPECT_EQ(h->audit_sweep_one_round(/*batch=*/64), 0u);
    EXPECT_EQ(h->audit_sweep_one_round(/*batch=*/64), 0u);
}

TEST(IcmpDirectory, AuditSweep_HealthyKeyedReturnsZeroMismatches) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_swhealth");
    ASSERT_TRUE(h.has_value()) << h.error();

    eph::net::HmacSha256Key key{std::string_view{"sweep-test-key"}};
    h->enable_hmac_(std::move(key));

    // Register 3 targets — they get signed at publish time.
    ASSERT_TRUE(h->register_target(make_tuple(41000), kProtoTcp, 0).has_value());
    ASSERT_TRUE(h->register_target(make_tuple(41010), kProtoTcp, 1).has_value());
    ASSERT_TRUE(h->register_target(make_tuple(41020), kProtoUdp, 2).has_value());

    // Walk the entire directory in chunks. With kIcmpDirectoryMaxEntries=1024
    // and batch=128, a full pass takes 8 calls. Healthy = 0 mismatches each.
    size_t total_mm = 0;
    for (int i = 0; i < 8; ++i) {
        total_mm += h->audit_sweep_one_round(/*batch=*/128);
    }
    EXPECT_EQ(total_mm, 0u);
}

TEST(IcmpDirectory, AuditSweep_DetectsTamper) {
    // Wild-pointer tamper: flip a byte in proto. Audit sweep MUST surface it.
    auto h = IcmpDirectoryHandle::create_primary("idtest_swtamp");
    ASSERT_TRUE(h.has_value()) << h.error();

    eph::net::HmacSha256Key key{std::string_view{"sweep-tamper-key"}};
    h->enable_hmac_(std::move(key));

    auto t = make_tuple(42000);
    auto idx_r = h->register_target(t, kProtoTcp, /*owner=*/0);
    ASSERT_TRUE(idx_r.has_value()) << idx_r.error();
    const size_t idx = *idx_r;

    // Healthy: full sweep yields 0 mismatches.
    size_t healthy_mm = 0;
    for (int i = 0; i < 16; ++i) {  // 16 * 64 = 1024 = full pass
        healthy_mm += h->audit_sweep_one_round(/*batch=*/64);
    }
    EXPECT_EQ(healthy_mm, 0u);

    // Tamper proto in place. The acquire-load on `claimed` still sees
    // Published, but the recomputed HMAC will not match the stored tag.
    h->header()->entries[idx].proto ^= 0xFF;

    // Sweep — exactly one mismatch should surface, and only on the
    // pass that covers slot `idx`.
    size_t total_mm = 0;
    for (int i = 0; i < 16; ++i) {
        total_mm += h->audit_sweep_one_round(/*batch=*/64);
    }
    EXPECT_EQ(total_mm, 1u)
        << "expected exactly 1 mismatch from the tampered slot " << idx;
}

TEST(IcmpDirectory, AuditSweep_CursorWrapsAfterFullPass) {
    // Walk the entire directory using batch=kIcmpDirectoryMaxEntries; the
    // cursor must reset to 0 so a SECOND pass also covers everything.
    // Verify by tampering AFTER the first full pass — the second pass
    // must still detect the new tamper (proves the cursor wrapped).
    auto h = IcmpDirectoryHandle::create_primary("idtest_swwrap");
    ASSERT_TRUE(h.has_value()) << h.error();

    eph::net::HmacSha256Key key{std::string_view{"sweep-wrap-key"}};
    h->enable_hmac_(std::move(key));

    auto t = make_tuple(43000);
    auto idx_r = h->register_target(t, kProtoTcp, /*owner=*/0);
    ASSERT_TRUE(idx_r.has_value()) << idx_r.error();

    // Full sweep #1 — healthy, 0 mismatches.
    EXPECT_EQ(h->audit_sweep_one_round(kIcmpDirectoryMaxEntries), 0u);

    // Tamper AFTER pass #1 closes. If the cursor failed to wrap to 0,
    // the next call would scan an empty tail range and miss this.
    h->header()->entries[*idx_r].owner_proc = 99;  // tampered

    // Full sweep #2 — must detect the tamper, proving cursor wrapped.
    EXPECT_EQ(h->audit_sweep_one_round(kIcmpDirectoryMaxEntries), 1u)
        << "cursor did not wrap after first full pass";
}

TEST(IcmpDirectory, AuditSweep_BatchSmallerThanTotalProgressesAcrossCalls) {
    // Batch smaller than total: cursor should advance by batch each
    // call, so a tampered slot near the END of the directory is only
    // detected by the call whose window covers it.
    auto h = IcmpDirectoryHandle::create_primary("idtest_swprog");
    ASSERT_TRUE(h.has_value()) << h.error();

    eph::net::HmacSha256Key key{std::string_view{"sweep-prog-key"}};
    h->enable_hmac_(std::move(key));

    // Force registrations into known slots by registering many targets;
    // first-fit allocates 0,1,2,… in order. After registering 3 targets,
    // slots 0,1,2 are populated, all others are Free.
    auto idx0 = h->register_target(make_tuple(44000), kProtoTcp, 0);
    auto idx1 = h->register_target(make_tuple(44010), kProtoTcp, 1);
    auto idx2 = h->register_target(make_tuple(44020), kProtoTcp, 2);
    ASSERT_TRUE(idx0.has_value() && idx1.has_value() && idx2.has_value());
    EXPECT_EQ(*idx0, 0u);
    EXPECT_EQ(*idx1, 1u);
    EXPECT_EQ(*idx2, 2u);

    // Tamper slot 1.
    h->header()->entries[1].src_port ^= 0x55;

    // batch=1: each call advances cursor by 1. Calls #0..#2 cover slots
    // 0,1,2 — call #1 (covering slot 1) should report the only mismatch.
    EXPECT_EQ(h->audit_sweep_one_round(/*batch=*/1), 0u);  // slot 0: clean
    EXPECT_EQ(h->audit_sweep_one_round(/*batch=*/1), 1u);  // slot 1: tampered
    EXPECT_EQ(h->audit_sweep_one_round(/*batch=*/1), 0u);  // slot 2: clean

    // Calls #3..#1023: all Free (skip), 0 mismatches each. We don't
    // walk all 1021 here for test runtime; the wrap test covers full
    // pass behaviour separately.
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(h->audit_sweep_one_round(/*batch=*/1), 0u)
            << "extra call " << i << " (free slot range)";
    }
}

TEST(IcmpDirectory, AuditSweep_ZeroBatchIsNoOp) {
    // Defensive edge: batch_size=0. Should be a clean no-op (no
    // division-by-zero, no infinite loop).
    auto h = IcmpDirectoryHandle::create_primary("idtest_swzero");
    ASSERT_TRUE(h.has_value()) << h.error();

    eph::net::HmacSha256Key key{std::string_view{"sweep-zero-key"}};
    h->enable_hmac_(std::move(key));

    ASSERT_TRUE(h->register_target(make_tuple(45000), kProtoTcp, 0).has_value());

    // Even with a tampered slot, batch=0 doesn't scan anything.
    h->header()->entries[0].dst_ip ^= 1u;
    EXPECT_EQ(h->audit_sweep_one_round(/*batch=*/0), 0u);
    EXPECT_EQ(h->audit_sweep_one_round(/*batch=*/0), 0u);

    // A batch>=1 call in the same window should still find the tamper —
    // this proves the cursor was NOT advanced by the zero-batch calls.
    EXPECT_EQ(h->audit_sweep_one_round(/*batch=*/64), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// audit_entry — single-slot HMAC verification (audit-on-suspicion path)
// ─────────────────────────────────────────────────────────────────────────────
//
// `audit_entry(slot_idx)` is the audit-on-error fast path: when a
// dispatch handler rejects an entry, the call site verifies just that
// one slot to distinguish logical (tuple mismatch) vs malicious
// (tampered) rejection. The docstring promises specific returns for
// every state combination — pin them here.

TEST(IcmpDirectory, AuditEntry_UnkeyedAlwaysTrue) {
    // hmac_enabled=0 (default unkeyed) → audit_entry returns true
    // unconditionally without scanning the entry. Single-tenant
    // deployments pay zero overhead.
    auto h = IcmpDirectoryHandle::create_primary("idtest_aeunk");
    ASSERT_TRUE(h.has_value()) << h.error();

    auto idx = h->register_target(make_tuple(46000), kProtoTcp, 0);
    ASSERT_TRUE(idx.has_value());

    // Even with a deliberately tampered entry, unkeyed audit returns true.
    h->header()->entries[*idx].proto ^= 0xFF;
    EXPECT_TRUE(h->audit_entry(*idx))
        << "unkeyed audit_entry must return true (no-op success)";
}

TEST(IcmpDirectoryHmac, AuditEntry_HealthyPublishedReturnsTrue) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_aeok");
    ASSERT_TRUE(h.has_value()) << h.error();
    h->enable_hmac_(eph::net::HmacSha256Key{std::string_view{"audit-ok-key"}});

    auto idx = h->register_target(make_tuple(46100), kProtoTcp, 0);
    ASSERT_TRUE(idx.has_value());

    EXPECT_TRUE(h->audit_entry(*idx))
        << "freshly-published entry must verify under its sign-time key";
}

TEST(IcmpDirectoryHmac, AuditEntry_TamperedPublishedReturnsFalse) {
    auto h = IcmpDirectoryHandle::create_primary("idtest_aetamp");
    ASSERT_TRUE(h.has_value()) << h.error();
    h->enable_hmac_(eph::net::HmacSha256Key{std::string_view{"audit-tamp-key"}});

    auto idx = h->register_target(make_tuple(46200), kProtoTcp, 0);
    ASSERT_TRUE(idx.has_value());

    h->header()->entries[*idx].owner_proc = 99;  // tamper
    EXPECT_FALSE(h->audit_entry(*idx))
        << "tampered authenticated field must surface as audit failure";
}

TEST(IcmpDirectoryHmac, AuditEntry_FreeSlotReturnsTrue) {
    // Free slots have no signed payload; audit_entry returns true (the
    // "nothing to verify" no-op so callers can probe any index).
    auto h = IcmpDirectoryHandle::create_primary("idtest_aefree");
    ASSERT_TRUE(h.has_value()) << h.error();
    h->enable_hmac_(eph::net::HmacSha256Key{std::string_view{"audit-free-key"}});

    // No registrations → all 1024 slots are Free.
    EXPECT_TRUE(h->audit_entry(/*slot=*/0));
    EXPECT_TRUE(h->audit_entry(/*slot=*/100));
    EXPECT_TRUE(h->audit_entry(/*slot=*/1023));
}

TEST(IcmpDirectoryHmac, AuditEntry_OutOfRangeReturnsFalse) {
    // slot_idx >= entries.size() is rejected (defensive bound check;
    // callers passing the result of `lookup`/`register_target` never
    // hit this — but a misbehaving caller deserves a deterministic no
    // rather than UB).
    auto h = IcmpDirectoryHandle::create_primary("idtest_aeoob");
    ASSERT_TRUE(h.has_value()) << h.error();
    h->enable_hmac_(eph::net::HmacSha256Key{std::string_view{"audit-oob-key"}});

    EXPECT_FALSE(h->audit_entry(/*slot=*/kIcmpDirectoryMaxEntries));
    EXPECT_FALSE(h->audit_entry(/*slot=*/kIcmpDirectoryMaxEntries + 100));
    EXPECT_FALSE(h->audit_entry(/*slot=*/SIZE_MAX));
}

TEST(IcmpDirectoryHmac, AuditEntry_AfterUnregisterReturnsTrue) {
    // After unregister, the slot's `claimed` is Free. audit_entry
    // returns true because there's nothing meaningful to verify on
    // a Free slot — even though the now-stale `hmac_tag` bytes remain
    // (R2 fix: unregister deliberately doesn't zero fields to avoid
    // racing concurrent acquire-Published readers).
    auto h = IcmpDirectoryHandle::create_primary("idtest_aeunreg");
    ASSERT_TRUE(h.has_value()) << h.error();
    h->enable_hmac_(eph::net::HmacSha256Key{std::string_view{"audit-unreg-key"}});

    auto idx = h->register_target(make_tuple(46300), kProtoTcp, 0);
    ASSERT_TRUE(idx.has_value());
    EXPECT_TRUE(h->audit_entry(*idx));   // pre-unregister: healthy

    h->unregister(*idx);
    EXPECT_TRUE(h->audit_entry(*idx))   // post-unregister: Free → true
        << "Free slot must audit as no-op true regardless of stale tag";
}
