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
