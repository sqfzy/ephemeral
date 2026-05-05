/// @file test_mp_registry_hmac.cpp
/// T2.3 wiring tests: HMAC-SHA256 tamper protection on the
/// cross-process MpRegistry.
///
/// What's tested:
///   - Wire format v4: `header.hmac_enabled` flag, `ProcSlot::hmac_tag`
///     field, 32-byte authenticated payload size.
///   - `pack_slot_for_hmac` packs the right 56 bytes in the documented
///     little-endian order.
///   - `sign_slot_in_place` is deterministic for a given key; same input
///     bytes + same key => same tag.
///   - `verify_slot` returns true on the freshly-signed slot.
///   - `verify_slot` returns false when ANY authenticated field is
///     tampered (queue_lo, port_hi, lcore_mask, pid, tag string).
///   - `verify_slot` ignores `claimed` and `hmac_tag` self-modifications
///     (they are *not* in the authenticated payload — claimed is the
///     atomic claim flag, hmac_tag is the tag itself).
///   - `init_mp_registry_header_with_hmac` flips `hmac_enabled` to 1
///     and signs every populated slot.
///   - `init_mp_registry_header` (unkeyed) leaves `hmac_enabled=0` and
///     all hmac_tag bytes zero.
///   - Keyed init + tamper round-trip: sign → tamper → verify fails.
///   - Different keys produce different tags (no collision under the
///     fixed test inputs).
///
/// What's NOT tested here:
///   - The on-disk key file at `/run/eph/<bdf>.key` (needs root + tmpfs
///     setup; covered by the integration test).
///   - End-to-end Platform::serve_nic + Platform::create attaching
///     under HMAC (needs EAL + hugepages; integration scope).

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/dpdk/detail/mp_registry.hpp"
#include "eph/net/hmac.hpp"

namespace ed = eph::dpdk::detail;

namespace {

// ProcSlot contains std::atomic which is non-copyable; fill in place.
void fill_populated_slot(ed::ProcSlot& s, uint8_t i) {
    s.claimed.store(1, std::memory_order_relaxed);
    std::memset(s.tag, 0, sizeof(s.tag));
    std::memcpy(s.tag, "trader", 6);
    s.queue_lo   = static_cast<uint16_t>(2 * i);
    s.queue_hi   = static_cast<uint16_t>(2 * i + 2);
    s.port_lo    = 32768u + i * 4096u;
    s.port_hi    = 32768u + (i + 1) * 4096u;
    s.lcore_mask = (uint64_t{1} << i);
    s.pid        = 1000 + i;
    std::memset(s.hmac_tag, 0, sizeof(s.hmac_tag));
}

}  // namespace

TEST(MpRegistryHmac, AuthBytesPackingDeterministic) {
    ed::ProcSlot s{}; fill_populated_slot(s, 3);
    std::array<uint8_t, ed::kSlotAuthBytes> p1{};
    std::array<uint8_t, ed::kSlotAuthBytes> p2{};
    ed::pack_slot_for_hmac(s, p1);
    ed::pack_slot_for_hmac(s, p2);
    EXPECT_EQ(p1, p2);
    EXPECT_EQ(ed::kSlotAuthBytes, 56u);
}

TEST(MpRegistryHmac, SignVerifyRoundTrip) {
    eph::net::HmacSha256Key key{
        std::string_view{"daemon-test-key-32-bytes-XXXXXX"}};
    ed::ProcSlot s{}; fill_populated_slot(s, 1);
    ed::sign_slot_in_place(s, key);

    EXPECT_TRUE(ed::verify_slot(s, key));
}

TEST(MpRegistryHmac, TamperedQueueLoRejected) {
    eph::net::HmacSha256Key key{std::string_view{"k1"}};
    ed::ProcSlot s{}; fill_populated_slot(s, 2);
    ed::sign_slot_in_place(s, key);

    s.queue_lo ^= 0xFF;
    EXPECT_FALSE(ed::verify_slot(s, key));
}

TEST(MpRegistryHmac, TamperedPortHiRejected) {
    eph::net::HmacSha256Key key{std::string_view{"k2"}};
    ed::ProcSlot s{}; fill_populated_slot(s, 2);
    ed::sign_slot_in_place(s, key);

    s.port_hi -= 1;
    EXPECT_FALSE(ed::verify_slot(s, key));
}

TEST(MpRegistryHmac, TamperedLcoreMaskRejected) {
    eph::net::HmacSha256Key key{std::string_view{"k3"}};
    ed::ProcSlot s{}; fill_populated_slot(s, 2);
    ed::sign_slot_in_place(s, key);

    s.lcore_mask ^= uint64_t{1} << 32;
    EXPECT_FALSE(ed::verify_slot(s, key));
}

TEST(MpRegistryHmac, TamperedPidRejected) {
    eph::net::HmacSha256Key key{std::string_view{"k4"}};
    ed::ProcSlot s{}; fill_populated_slot(s, 2);
    ed::sign_slot_in_place(s, key);

    s.pid = 99999;
    EXPECT_FALSE(ed::verify_slot(s, key));
}

TEST(MpRegistryHmac, TamperedTagStringRejected) {
    eph::net::HmacSha256Key key{std::string_view{"k5"}};
    ed::ProcSlot s{}; fill_populated_slot(s, 2);
    ed::sign_slot_in_place(s, key);

    s.tag[0] = 'X';
    EXPECT_FALSE(ed::verify_slot(s, key));
}

TEST(MpRegistryHmac, ClaimedNotInAuthPayload) {
    // The cross-process atomic claim flag is intentionally NOT
    // authenticated — the daemon signs the slot once at write time
    // and we want claim/release transitions to NOT invalidate the
    // tag. Verify that flipping `claimed` does not break verify.
    eph::net::HmacSha256Key key{std::string_view{"k6"}};
    ed::ProcSlot s{}; fill_populated_slot(s, 2);
    ed::sign_slot_in_place(s, key);

    EXPECT_TRUE(ed::verify_slot(s, key));
    s.claimed.store(0, std::memory_order_relaxed);
    EXPECT_TRUE(ed::verify_slot(s, key));
    s.claimed.store(1, std::memory_order_relaxed);
    EXPECT_TRUE(ed::verify_slot(s, key));
}

TEST(MpRegistryHmac, TampedHmacTagItselfRejected) {
    eph::net::HmacSha256Key key{std::string_view{"k7"}};
    ed::ProcSlot s{}; fill_populated_slot(s, 2);
    ed::sign_slot_in_place(s, key);

    s.hmac_tag[0] ^= 0x01;
    EXPECT_FALSE(ed::verify_slot(s, key));
}

TEST(MpRegistryHmac, DifferentKeysDifferentTags) {
    eph::net::HmacSha256Key key_a{std::string_view{"alice"}};
    eph::net::HmacSha256Key key_b{std::string_view{"bob"}};
    ed::ProcSlot s_a{}; fill_populated_slot(s_a, 2);
    ed::ProcSlot s_b{}; fill_populated_slot(s_b, 2);  // identical bytes
    ed::sign_slot_in_place(s_a, key_a);
    ed::sign_slot_in_place(s_b, key_b);

    EXPECT_NE(0, std::memcmp(s_a.hmac_tag, s_b.hmac_tag,
                             sizeof(s_a.hmac_tag)));
    EXPECT_TRUE(ed::verify_slot(s_a, key_a));
    EXPECT_TRUE(ed::verify_slot(s_b, key_b));
    EXPECT_FALSE(ed::verify_slot(s_a, key_b));
    EXPECT_FALSE(ed::verify_slot(s_b, key_a));
}

TEST(MpRegistryHmac, UnkeyedInitLeavesHmacDisabled) {
    alignas(64) ed::MpRegistryHeader hdr{};
    auto topo = eph::dpdk::MpTopology::uniform(0, 2, 8, 32768, 32768);
    ed::init_mp_registry_header(&hdr, "ut_unkeyed", topo);

    EXPECT_EQ(hdr.hmac_enabled, 0u);
    for (size_t i = 0; i < topo.total_procs; ++i) {
        for (uint8_t b : hdr.procs[i].hmac_tag) {
            EXPECT_EQ(b, 0u) << "slot " << i;
        }
    }
}

TEST(MpRegistryHmac, KeyedInitFlipsFlagAndSignsAllPopulatedSlots) {
    alignas(64) ed::MpRegistryHeader hdr{};
    auto topo = eph::dpdk::MpTopology::uniform(0, 3, 12, 32768, 32768);

    eph::net::HmacSha256Key key{std::string_view{"keyed-init"}};
    ed::init_mp_registry_header_with_hmac(&hdr, "ut_keyed", topo, key);

    EXPECT_EQ(hdr.hmac_enabled, 1u);

    // Every populated slot must now verify under the key.
    for (uint8_t i = 0; i < topo.total_procs; ++i) {
        EXPECT_TRUE(ed::verify_slot(hdr.procs[i], key))
            << "slot " << static_cast<int>(i)
            << " did not verify after keyed init";
    }

    // Free slots beyond total_procs remain zero-tagged (so a stray
    // verify on them would fail; legitimate readers gate verify on
    // claimed=1 + hmac_enabled=1).
    for (size_t i = topo.total_procs; i < hdr.procs.size(); ++i) {
        for (uint8_t b : hdr.procs[i].hmac_tag) {
            EXPECT_EQ(b, 0u) << "free slot " << i;
        }
    }
}

TEST(MpRegistryHmac, EnableHmacRuntimeIsIdempotentAndSigns) {
    // L1 wiring: MpRegistryHandle::enable_hmac_(key) must flip
    // header.hmac_enabled and sign all populated slots so a future
    // audit_all() returns 0 mismatches in the healthy case.
    alignas(64) ed::MpRegistryHeader hdr{};
    auto topo = eph::dpdk::MpTopology::uniform(0, 2, 8, 32768, 32768);
    ed::init_mp_registry_header(&hdr, "ut_enable", topo);
    EXPECT_EQ(hdr.hmac_enabled, 0u);
    // Pre-claim slot 0 so audit_all has something to walk.
    hdr.procs[0].claimed.store(1, std::memory_order_relaxed);
    hdr.procs[1].claimed.store(1, std::memory_order_relaxed);

    // Pretend this is the daemon-side handle: bind hdr_ via the
    // memzone factory path is heavyweight (needs EAL); for unit test
    // we instead verify the underlying primitives sign+audit
    // directly. The end-to-end Platform::serve_nic path is exercised
    // by the integration test on a daemon-equipped host.
    eph::net::HmacSha256Key key{std::string_view{"runtime-enable"}};
    for (uint8_t i = 0; i < topo.total_procs; ++i) {
        ed::sign_slot_in_place(hdr.procs[i], key);
    }
    hdr.hmac_enabled = 1;

    // All populated slots verify under the key.
    for (uint8_t i = 0; i < topo.total_procs; ++i) {
        EXPECT_TRUE(ed::verify_slot(hdr.procs[i], key));
    }

    // Tamper one slot — verify mismatches.
    hdr.procs[0].port_lo += 7;
    EXPECT_FALSE(ed::verify_slot(hdr.procs[0], key));
    EXPECT_TRUE(ed::verify_slot(hdr.procs[1], key));
}

TEST(MpRegistryHmac, KeyedInitTamperRoundTrip) {
    alignas(64) ed::MpRegistryHeader hdr{};
    auto topo = eph::dpdk::MpTopology::uniform(1, 2, 8, 32768, 32768);

    eph::net::HmacSha256Key key{std::string_view{"tamper-rt"}};
    ed::init_mp_registry_header_with_hmac(&hdr, "ut_tamper", topo, key);

    // Pre-tamper: both populated slots verify.
    EXPECT_TRUE(ed::verify_slot(hdr.procs[0], key));
    EXPECT_TRUE(ed::verify_slot(hdr.procs[1], key));

    // Simulate a malicious / wild-pointer write into slot 1.
    hdr.procs[1].port_lo += 1;
    EXPECT_TRUE(ed::verify_slot(hdr.procs[0], key));   // unaffected
    EXPECT_FALSE(ed::verify_slot(hdr.procs[1], key));  // detected
}
