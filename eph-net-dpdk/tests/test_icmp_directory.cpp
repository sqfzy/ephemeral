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
