/// @file test_hmac_keyed_entry.cpp
/// Tests for `eph::dpdk::detail::HmacKeyedEntry<T>` (T2.3 skeleton).
///
/// What's tested:
///   - Layout: HmacKeyedEntry<T> POD, sizeof = sizeof(T) + 32, trivially
///     copyable so the type is memzone-safe.
///   - sign_entry → verify_entry round-trip succeeds.
///   - Tampered data fails verification.
///   - Tampered tag fails verification.
///   - Different keys produce different tags.
///   - compute_tag is deterministic (same input → same output).
///
/// What's NOT tested (deferred to the integration follow-up that wires
/// the wrapper into MpRegistry / IcmpDirectory / QueueAllocator):
///   - Tamper detection across processes.
///   - Daemon-distributed key threat model.
///   - Performance in the verify-on-every-read scenario.

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include <gtest/gtest.h>

#include "eph/dpdk/detail/hmac_keyed_entry.hpp"
#include "eph/net/hmac.hpp"

namespace ed = eph::dpdk::detail;

namespace {

struct DummyEntry {
    uint64_t a;
    uint32_t b;
    uint16_t c;
    uint16_t pad{};  // make it deterministic-sized
};

}  // namespace

TEST(HmacKeyedEntry, LayoutTrivial) {
    static_assert(std::is_trivially_copyable_v<ed::HmacKeyedEntry<DummyEntry>>);
    EXPECT_EQ(sizeof(ed::HmacKeyedEntry<DummyEntry>),
              sizeof(DummyEntry) + ed::kHmacKeyedEntryTagBytes);
}

TEST(HmacKeyedEntry, SignVerifyRoundTrip) {
    eph::net::HmacSha256Key key{
        std::string_view{"super-secret-daemon-key-32bytes-XX"}};

    ed::HmacKeyedEntry<DummyEntry> entry{};
    entry.data = {.a = 0xdeadbeef'cafebabeULL, .b = 0xfeed1234, .c = 0xabcd};

    ed::sign_entry(entry, key);
    EXPECT_TRUE(ed::verify_entry(entry, key));
}

TEST(HmacKeyedEntry, TamperedDataFailsVerification) {
    eph::net::HmacSha256Key key{std::string_view{"key-1"}};
    ed::HmacKeyedEntry<DummyEntry> entry{};
    entry.data = {.a = 100, .b = 200, .c = 300};

    ed::sign_entry(entry, key);
    EXPECT_TRUE(ed::verify_entry(entry, key));

    // Mutate one field — verification must fail.
    entry.data.b = 0xdeadbeef;
    EXPECT_FALSE(ed::verify_entry(entry, key));
}

TEST(HmacKeyedEntry, TamperedTagFailsVerification) {
    eph::net::HmacSha256Key key{std::string_view{"key-2"}};
    ed::HmacKeyedEntry<DummyEntry> entry{};
    entry.data = {.a = 1, .b = 2, .c = 3};

    ed::sign_entry(entry, key);
    EXPECT_TRUE(ed::verify_entry(entry, key));

    // Flip one bit of the tag — verification must fail.
    entry.tag[0] ^= 0x01;
    EXPECT_FALSE(ed::verify_entry(entry, key));
}

TEST(HmacKeyedEntry, DifferentKeysProduceDifferentTags) {
    eph::net::HmacSha256Key key_a{std::string_view{"key-a"}};
    eph::net::HmacSha256Key key_b{std::string_view{"key-b"}};

    ed::HmacKeyedEntry<DummyEntry> e1{};
    ed::HmacKeyedEntry<DummyEntry> e2{};
    e1.data = {.a = 42, .b = 43, .c = 44};
    e2.data = e1.data;

    ed::sign_entry(e1, key_a);
    ed::sign_entry(e2, key_b);

    // Same data, different keys => tags must differ.
    EXPECT_NE(0, std::memcmp(e1.tag.data(), e2.tag.data(),
                             ed::kHmacKeyedEntryTagBytes));

    // Cross-verify must fail.
    EXPECT_TRUE(ed::verify_entry(e1, key_a));
    EXPECT_FALSE(ed::verify_entry(e1, key_b));
    EXPECT_TRUE(ed::verify_entry(e2, key_b));
    EXPECT_FALSE(ed::verify_entry(e2, key_a));
}

TEST(HmacKeyedEntry, ComputeTagIsDeterministic) {
    eph::net::HmacSha256Key key{std::string_view{"deterministic-key"}};

    const std::array<uint8_t, 8> bytes = {0x01, 0x02, 0x03, 0x04,
                                          0x05, 0x06, 0x07, 0x08};
    const auto t1 = ed::compute_tag(bytes, key);
    const auto t2 = ed::compute_tag(bytes, key);

    EXPECT_EQ(t1, t2);
}

TEST(HmacKeyedEntry, VerifyTagConstantTimeMatches) {
    eph::net::HmacSha256Key key{std::string_view{"vt-key"}};
    const std::array<uint8_t, 16> bytes = {1, 2, 3, 4, 5, 6, 7, 8,
                                           9, 10, 11, 12, 13, 14, 15, 16};
    const auto good_tag = ed::compute_tag(bytes, key);

    EXPECT_TRUE(ed::verify_tag(bytes, key, good_tag));

    auto bad_tag = good_tag;
    bad_tag[31] ^= 0x80;  // flip last byte's high bit
    EXPECT_FALSE(ed::verify_tag(bytes, key, bad_tag));
}

TEST(HmacKeyedEntry, ComputeTagOnEmptySpan) {
    // Boundary: empty input is valid HMAC-SHA256 (RFC 2104 allows
    // zero-length data; aws-lc handles it). The tag is well-defined
    // and deterministic; verify the round-trip works for empty input.
    eph::net::HmacSha256Key key{std::string_view{"empty-key"}};
    const std::array<uint8_t, 0> bytes{};
    std::span<const uint8_t> span_empty{bytes};
    const auto t1 = ed::compute_tag(span_empty, key);
    const auto t2 = ed::compute_tag(span_empty, key);

    EXPECT_EQ(t1, t2)
        << "empty-input HMAC must be deterministic";
    EXPECT_TRUE(ed::verify_tag(span_empty, key, t1))
        << "empty-input verify_tag must round-trip";
}

TEST(HmacKeyedEntry, ComputeTagOnSingleByte) {
    // Smallest non-empty input. Some HMAC implementations had bugs
    // around 1-byte inputs (block-padding off-by-one); pinning the
    // round-trip catches a future regression.
    eph::net::HmacSha256Key key{std::string_view{"single-byte-key"}};
    const std::array<uint8_t, 1> bytes = {0x42};
    std::span<const uint8_t> span_one{bytes};
    const auto good = ed::compute_tag(span_one, key);

    EXPECT_TRUE(ed::verify_tag(span_one, key, good));

    // Different single byte ⇒ different tag.
    const std::array<uint8_t, 1> bytes2 = {0x43};
    const auto good2 = ed::compute_tag(std::span<const uint8_t>{bytes2}, key);
    EXPECT_NE(good, good2);
}

TEST(HmacKeyedEntry, EmptyInputDifferentKeysDifferentTags) {
    // Even with empty input, different keys MUST produce different
    // tags (key is the only entropy source). Catches a hypothetical
    // regression where empty input short-circuits past the key
    // material entirely.
    eph::net::HmacSha256Key key_a{std::string_view{"keyA"}};
    eph::net::HmacSha256Key key_b{std::string_view{"keyB"}};
    const std::array<uint8_t, 0> bytes{};
    std::span<const uint8_t> empty_span{bytes};
    const auto ta = ed::compute_tag(empty_span, key_a);
    const auto tb = ed::compute_tag(empty_span, key_b);
    EXPECT_NE(ta, tb)
        << "empty-input HMAC under different keys must differ";
}
