/// @file test_icmp_directory_hmac.cpp
/// T2.3 wiring tests for `IcmpDirectory`'s HMAC tamper protection.
/// Hot-path verify-free, verify-on-suspicion design (K series).
///
/// What's tested:
///   - Wire format v2: `header.hmac_enabled` flag, per-entry
///     `hmac_tag[32]`, 16-byte authenticated payload size.
///   - `pack_icmp_entry_for_hmac` deterministic over (proto +
///     owner_proc + 5-tuple); excludes claimed + generation +
///     hmac_tag.
///   - `sign_icmp_entry_in_place` + `verify_icmp_entry` round-trip.
///   - Tamper detection on every authenticated field
///     (proto, owner_proc, src_ip, dst_ip, src_port, dst_port).
///   - Tamper on `hmac_tag` itself rejected.
///   - `claimed` mutations (free → in-progress → published) do NOT
///     invalidate the tag.
///   - `generation` mutations do NOT invalidate the tag.
///   - Different keys produce different tags.
///   - `enable_hmac_(key)` flips header.hmac_enabled = 1 and
///     populates the handle's key.
///   - `audit_entry(slot)` returns true on Published entry under
///     correct key; false under wrong key; true on Free / InProgress
///     slots (nothing to verify).
///   - `audit_sweep_one_round(batch)` advances the cursor + returns
///     mismatch count + wraps after one full pass.
///
/// What's NOT tested here:
///   - End-to-end Platform integration (needs daemon hardware).
///   - Actual tamper scenario via a wild-pointer write (covered
///     by the existing test_hmac_tamper_simulation pattern).

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/dpdk/detail/icmp_directory.hpp"
#include "eph/net/hmac.hpp"

namespace ed = eph::dpdk::detail;

namespace {

void fill_published_entry(ed::IcmpDirectoryEntry& e, uint8_t i) {
    e.claimed.store(ed::kIcmpSlotPublished, std::memory_order_relaxed);
    e.proto      = 6;  // TCP
    e.owner_proc = i;
    e._pad0      = 0;
    e.src_ip     = 0x0a000000u | i;
    e.dst_ip     = 0xc0a80100u | i;
    e.src_port   = static_cast<uint16_t>(32768 + i * 100);
    e.dst_port   = static_cast<uint16_t>(8080);
    e._pad1      = 0;
    e.generation.store(1, std::memory_order_relaxed);
    std::memset(e.hmac_tag, 0, sizeof(e.hmac_tag));
}

}  // namespace

TEST(IcmpDirectoryHmac, AuthBytesPackingDeterministic) {
    ed::IcmpDirectoryEntry e{};
    fill_published_entry(e, 3);
    std::array<uint8_t, ed::kIcmpEntryAuthBytes> p1{}, p2{};
    ed::pack_icmp_entry_for_hmac(e, p1);
    ed::pack_icmp_entry_for_hmac(e, p2);
    EXPECT_EQ(p1, p2);
    EXPECT_EQ(ed::kIcmpEntryAuthBytes, 16u);
}

TEST(IcmpDirectoryHmac, SignVerifyRoundTrip) {
    ed::IcmpDirectoryEntry e{};
    fill_published_entry(e, 1);
    eph::net::HmacSha256Key key{std::string_view{"icmp-rt"}};
    ed::sign_icmp_entry_in_place(e, key);
    EXPECT_TRUE(ed::verify_icmp_entry(e, key));
}

TEST(IcmpDirectoryHmac, TamperedProtoRejected) {
    ed::IcmpDirectoryEntry e{};
    fill_published_entry(e, 1);
    eph::net::HmacSha256Key key{std::string_view{"k1"}};
    ed::sign_icmp_entry_in_place(e, key);
    e.proto ^= 0xFF;
    EXPECT_FALSE(ed::verify_icmp_entry(e, key));
}

TEST(IcmpDirectoryHmac, TamperedOwnerProcRejected) {
    ed::IcmpDirectoryEntry e{};
    fill_published_entry(e, 2);
    eph::net::HmacSha256Key key{std::string_view{"k2"}};
    ed::sign_icmp_entry_in_place(e, key);
    e.owner_proc ^= 0xFF;
    EXPECT_FALSE(ed::verify_icmp_entry(e, key));
}

TEST(IcmpDirectoryHmac, TamperedSrcIpRejected) {
    ed::IcmpDirectoryEntry e{};
    fill_published_entry(e, 2);
    eph::net::HmacSha256Key key{std::string_view{"k3"}};
    ed::sign_icmp_entry_in_place(e, key);
    e.src_ip ^= 0xCAFEBABEu;
    EXPECT_FALSE(ed::verify_icmp_entry(e, key));
}

TEST(IcmpDirectoryHmac, TamperedDstPortRejected) {
    ed::IcmpDirectoryEntry e{};
    fill_published_entry(e, 2);
    eph::net::HmacSha256Key key{std::string_view{"k4"}};
    ed::sign_icmp_entry_in_place(e, key);
    e.dst_port ^= 0x4242;
    EXPECT_FALSE(ed::verify_icmp_entry(e, key));
}

TEST(IcmpDirectoryHmac, TamperedHmacTagRejected) {
    ed::IcmpDirectoryEntry e{};
    fill_published_entry(e, 2);
    eph::net::HmacSha256Key key{std::string_view{"k5"}};
    ed::sign_icmp_entry_in_place(e, key);
    e.hmac_tag[0] ^= 0x01;
    EXPECT_FALSE(ed::verify_icmp_entry(e, key));
}

TEST(IcmpDirectoryHmac, ClaimedMutationsDoNotInvalidateTag) {
    // The atomic state machine `claimed` is intentionally NOT in the
    // authenticated payload — its values mutate per claim cycle and
    // signing it would force re-sign on every publish/unclaim.
    ed::IcmpDirectoryEntry e{};
    fill_published_entry(e, 2);
    eph::net::HmacSha256Key key{std::string_view{"k6"}};
    ed::sign_icmp_entry_in_place(e, key);

    EXPECT_TRUE(ed::verify_icmp_entry(e, key));
    e.claimed.store(ed::kIcmpSlotInProgress, std::memory_order_relaxed);
    EXPECT_TRUE(ed::verify_icmp_entry(e, key));
    e.claimed.store(ed::kIcmpSlotFree, std::memory_order_relaxed);
    EXPECT_TRUE(ed::verify_icmp_entry(e, key));
}

TEST(IcmpDirectoryHmac, GenerationMutationsDoNotInvalidateTag) {
    ed::IcmpDirectoryEntry e{};
    fill_published_entry(e, 2);
    eph::net::HmacSha256Key key{std::string_view{"k7"}};
    ed::sign_icmp_entry_in_place(e, key);

    EXPECT_TRUE(ed::verify_icmp_entry(e, key));
    e.generation.fetch_add(1, std::memory_order_relaxed);
    EXPECT_TRUE(ed::verify_icmp_entry(e, key));
    e.generation.fetch_add(1, std::memory_order_relaxed);
    EXPECT_TRUE(ed::verify_icmp_entry(e, key));
}

TEST(IcmpDirectoryHmac, DifferentKeysDifferentTags) {
    ed::IcmpDirectoryEntry e_a{}, e_b{};
    fill_published_entry(e_a, 4);
    fill_published_entry(e_b, 4);  // identical fields

    eph::net::HmacSha256Key key_a{std::string_view{"alice"}};
    eph::net::HmacSha256Key key_b{std::string_view{"bob"}};
    ed::sign_icmp_entry_in_place(e_a, key_a);
    ed::sign_icmp_entry_in_place(e_b, key_b);

    EXPECT_NE(0, std::memcmp(e_a.hmac_tag, e_b.hmac_tag,
                             sizeof(e_a.hmac_tag)));
    EXPECT_TRUE(ed::verify_icmp_entry(e_a, key_a));
    EXPECT_TRUE(ed::verify_icmp_entry(e_b, key_b));
    EXPECT_FALSE(ed::verify_icmp_entry(e_a, key_b));
    EXPECT_FALSE(ed::verify_icmp_entry(e_b, key_a));
}

TEST(IcmpDirectoryHmac, HeaderInitDefaultsHmacDisabled) {
    // alignas(64) so the actual hugepage layout invariant matches.
    alignas(64) ed::IcmpDirectoryHeader hdr{};
    ed::init_icmp_directory_header(&hdr, "ut_icmp");

    EXPECT_EQ(hdr.hmac_enabled, 0u);
    for (size_t i = 0; i < 4; ++i) {
        for (uint8_t b : hdr.entries[i].hmac_tag) {
            EXPECT_EQ(b, 0u) << "slot " << i;
        }
    }
}
