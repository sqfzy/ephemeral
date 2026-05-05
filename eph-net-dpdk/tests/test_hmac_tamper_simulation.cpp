/// @file test_hmac_tamper_simulation.cpp
/// Simulated-tamper fuzz for `HmacKeyedEntry<T>` (T3.3 — skeleton
/// portion, paired with T2.3 skeleton in commit f2733623).
///
/// What this fuzzes:
///   - Random byte mutations of `entry.data` after signing
///     → verify_entry() must return false on every mutation.
///   - Random byte mutations of `entry.tag` after signing
///     → verify_entry() must return false on every mutation.
///   - Identical mutations applied + re-signed
///     → verify_entry() must return true again (sanity that the
///       primitive isn't permanently latching anything).
///   - Random tag XOR with another tag from a different key
///     → verify_entry() must return false (cross-key tags don't
///       happen to satisfy each other).
///
/// What this does NOT fuzz:
///   - Cross-process tamper detection — the registries
///     (`MpRegistry` / `IcmpDirectory` / `QueueAllocator`) do not
///     yet carry HMAC tags (full T2.3 wiring is deferred — see
///     `DEFERRED.md`). Once that wiring lands, a follow-up test
///     should exercise the actual cross-process tamper path.
///   - The threat model around daemon-distributed keys (out of scope
///     for the primitive — that's a deployment-architecture concern).
///
/// The test is deterministic (fixed seed) so a regression is
/// reproducible. Iteration count chosen so the random byte indices
/// touch every byte of a 32-byte tag and an 80-byte payload at
/// least once with high probability under a uniform PRNG.

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/dpdk/detail/hmac_keyed_entry.hpp"
#include "eph/net/hmac.hpp"

namespace ed = eph::dpdk::detail;

namespace {

// 80 bytes — larger than ProcSlot's data region; gives the random
// indexing room to land on any payload byte multiple times across
// iterations.
struct WireSlot {
    uint64_t                       claimed;
    std::array<char, 32>           tag_str;
    uint16_t                       q_lo;
    uint16_t                       q_hi;
    uint32_t                       p_lo;
    uint32_t                       p_hi;
    uint64_t                       lcore_mask;
    int32_t                        pid;
    std::array<uint8_t, 16>        reserved;
};

[[nodiscard]] WireSlot make_slot(uint64_t seed) {
    WireSlot s{};
    s.claimed    = 1;
    s.tag_str[0] = 't';  // arbitrary marker
    s.q_lo       = static_cast<uint16_t>(seed & 0xFFu);
    s.q_hi       = static_cast<uint16_t>((seed & 0xFFu) + 4u);
    s.p_lo       = 32768u;
    s.p_hi       = 40000u;
    s.lcore_mask = seed;
    s.pid        = 1234;
    return s;
}

}  // namespace

TEST(HmacTamperSimulation, RandomDataByteFlipsAlwaysDetected) {
    eph::net::HmacSha256Key key{std::string_view{
        "tamper-fuzz-key-aaaaaaaaaaaaaaaaaaaaa"}};

    constexpr int kIterations = 1000;
    std::mt19937_64 rng(0xDEADBEEFCAFEBABEull);

    int detected = 0;
    int undetected = 0;

    for (int i = 0; i < kIterations; ++i) {
        ed::HmacKeyedEntry<WireSlot> entry{};
        entry.data = make_slot(rng());
        ed::sign_entry(entry, key);
        ASSERT_TRUE(ed::verify_entry(entry, key));

        // Pick a random byte of `data` and a random non-zero XOR mask.
        const size_t byte_idx = rng() % sizeof(WireSlot);
        const uint8_t flip = static_cast<uint8_t>(1u + (rng() % 255u));

        auto* bytes = reinterpret_cast<uint8_t*>(&entry.data);
        bytes[byte_idx] ^= flip;

        if (ed::verify_entry(entry, key)) {
            ++undetected;  // would be a real bug
        } else {
            ++detected;
        }

        // Sanity: re-signing recovers verification.
        ed::sign_entry(entry, key);
        ASSERT_TRUE(ed::verify_entry(entry, key));
    }

    EXPECT_EQ(undetected, 0)
        << "data byte flips should always be detected; got "
        << undetected << "/" << kIterations << " undetected";
    EXPECT_EQ(detected, kIterations);
}

TEST(HmacTamperSimulation, RandomTagByteFlipsAlwaysDetected) {
    eph::net::HmacSha256Key key{std::string_view{"tamper-tag-fuzz-key"}};

    constexpr int kIterations = 1000;
    std::mt19937_64 rng(0x1234567890ABCDEFull);

    int detected = 0;
    int undetected = 0;

    for (int i = 0; i < kIterations; ++i) {
        ed::HmacKeyedEntry<WireSlot> entry{};
        entry.data = make_slot(rng());
        ed::sign_entry(entry, key);
        ASSERT_TRUE(ed::verify_entry(entry, key));

        const size_t byte_idx = rng() % entry.tag.size();
        const uint8_t flip = static_cast<uint8_t>(1u + (rng() % 255u));
        entry.tag[byte_idx] ^= flip;

        if (ed::verify_entry(entry, key)) {
            ++undetected;
        } else {
            ++detected;
        }
    }

    EXPECT_EQ(undetected, 0);
    EXPECT_EQ(detected, kIterations);
}

TEST(HmacTamperSimulation, MultipleSimultaneousByteFlipsAlwaysDetected) {
    eph::net::HmacSha256Key key{std::string_view{"multi-flip-key"}};

    constexpr int kIterations = 500;
    std::mt19937_64 rng(0xCAFEFEED12345678ull);

    int undetected = 0;
    for (int i = 0; i < kIterations; ++i) {
        ed::HmacKeyedEntry<WireSlot> entry{};
        entry.data = make_slot(rng());
        ed::sign_entry(entry, key);
        ASSERT_TRUE(ed::verify_entry(entry, key));

        // Flip 1-7 random bytes in entry.data simultaneously.
        const int n_flips = 1 + (rng() % 7);
        auto* bytes = reinterpret_cast<uint8_t*>(&entry.data);
        for (int f = 0; f < n_flips; ++f) {
            const size_t idx = rng() % sizeof(WireSlot);
            bytes[idx] ^= static_cast<uint8_t>(1u + (rng() % 255u));
        }

        if (ed::verify_entry(entry, key)) ++undetected;
    }

    EXPECT_EQ(undetected, 0);
}

TEST(HmacTamperSimulation, CrossKeyTagSwapDetected) {
    eph::net::HmacSha256Key key_alice{std::string_view{"alice"}};
    eph::net::HmacSha256Key key_bob  {std::string_view{"bob"}};

    constexpr int kIterations = 500;
    std::mt19937_64 rng(0xABCDEFABCDEFABCDull);

    int undetected = 0;
    for (int i = 0; i < kIterations; ++i) {
        ed::HmacKeyedEntry<WireSlot> e_alice{}, e_bob{};
        e_alice.data = make_slot(rng());
        e_bob.data   = e_alice.data;  // SAME data
        ed::sign_entry(e_alice, key_alice);
        ed::sign_entry(e_bob,   key_bob);

        // Swap tags — alice's entry now bears bob's tag.
        std::swap(e_alice.tag, e_bob.tag);

        // Both verifications must fail.
        if (ed::verify_entry(e_alice, key_alice)) ++undetected;
        if (ed::verify_entry(e_bob,   key_bob))   ++undetected;
    }

    EXPECT_EQ(undetected, 0);
}

TEST(HmacTamperSimulation, ZeroByteFlip_NoOp_StillValid) {
    // Sanity: XOR with 0x00 doesn't change anything; verification still
    // passes. Pinning the test rig itself: a buggy "always invalid"
    // implementation would falsely pass the previous tests via
    // false-rejection.
    eph::net::HmacSha256Key key{std::string_view{"sanity-key"}};

    ed::HmacKeyedEntry<WireSlot> entry{};
    entry.data = make_slot(0xCAFE);
    ed::sign_entry(entry, key);

    auto* bytes = reinterpret_cast<uint8_t*>(&entry.data);
    bytes[0] ^= 0x00;  // no-op
    EXPECT_TRUE(ed::verify_entry(entry, key));
}
