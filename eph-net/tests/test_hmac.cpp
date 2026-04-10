/// @file test_hmac.cpp
/// @brief Unit tests for `eph::net::HmacSha256Key` + `hmac_sha256_sign`.
///
/// Covers:
///   * All 7 RFC 4231 §4.6 test vectors (HMAC-SHA256)
///   * Key-normalization paths (short, exact-64, >64 hashed)
///   * Tag hex encoding (zero-alloc span + allocating string)
///   * Compile-time non-copyable / noexcept guarantees
///   * Destructor zero-on-destroy (best-effort reach-in)
///   * Determinism and key/data separation
///   * Large (1 MiB) and empty-data edge cases
///
/// Test vector bytes and expected tags are transcribed from RFC 4231,
/// which is public-domain standards text.

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "eph/net/hmac.hpp"

using eph::net::HmacSha256Key;
using eph::net::HmacSha256Tag;
using eph::net::hmac_sha256_sign;

// ─────────────────────────────────────────────────────────────────────────────
// Test helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Decode an ASCII hex string into a byte vector. Lowercase + uppercase.
std::vector<uint8_t> hex_to_bytes(std::string_view hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
        return 0;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(
            static_cast<uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return bytes;
}

/// Construct a span view over a byte vector.
std::span<const uint8_t> span_of(const std::vector<uint8_t>& v) {
    return {v.data(), v.size()};
}

/// Expect the tag's lowercase hex to equal `want` (64 chars).
void expect_tag_hex_eq(const HmacSha256Tag& tag, std::string_view want) {
    uint8_t hex_buf[64];
    ASSERT_EQ(64u, tag.to_hex(std::span<uint8_t, 64>{hex_buf}));
    std::string_view got{reinterpret_cast<const char*>(hex_buf), 64};
    EXPECT_EQ(got, want);
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Compile-time guarantees (Tokio-style typed wrapper invariants)
// ═════════════════════════════════════════════════════════════════════════════

TEST(HmacSha256Key_Static, NonCopyable) {
    static_assert(!std::is_copy_constructible_v<HmacSha256Key>);
    static_assert(!std::is_copy_assignable_v<HmacSha256Key>);
    SUCCEED();
}

TEST(HmacSha256Key_Static, Movable) {
    static_assert(std::is_move_constructible_v<HmacSha256Key>);
    static_assert(std::is_move_assignable_v<HmacSha256Key>);
    static_assert(std::is_nothrow_move_constructible_v<HmacSha256Key>);
    SUCCEED();
}

TEST(HmacSha256Key_Static, NoexceptConstruct) {
    static_assert(std::is_nothrow_constructible_v<
                  HmacSha256Key, std::span<const uint8_t>>);
    static_assert(std::is_nothrow_constructible_v<
                  HmacSha256Key, std::string_view>);
    static_assert(std::is_nothrow_destructible_v<HmacSha256Key>);
    SUCCEED();
}

TEST(HmacSha256Key_Static, SignIsNoexcept) {
    static_assert(noexcept(hmac_sha256_sign(
        std::declval<const HmacSha256Key&>(),
        std::declval<std::span<const uint8_t>>())));
    static_assert(noexcept(hmac_sha256_sign(
        std::declval<const HmacSha256Key&>(),
        std::declval<std::string_view>())));
    SUCCEED();
}

TEST(HmacSha256Tag_Static, ExactSize32) {
    static_assert(sizeof(HmacSha256Tag) == 32);
    SUCCEED();
}

// ═════════════════════════════════════════════════════════════════════════════
// RFC 4231 §4.6 — HMAC-SHA256 test vectors (all 7)
// ═════════════════════════════════════════════════════════════════════════════

// RFC 4231 Test Case 1
// Key  = 0x0b × 20
// Data = "Hi There"
// HMAC = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
TEST(Rfc4231, TestCase1) {
    auto key_bytes = hex_to_bytes("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    HmacSha256Key key{span_of(key_bytes)};
    auto tag = hmac_sha256_sign(key, std::string_view{"Hi There"});
    expect_tag_hex_eq(
        tag, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

// RFC 4231 Test Case 2
// Key  = "Jefe"
// Data = "what do ya want for nothing?"
// HMAC = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
TEST(Rfc4231, TestCase2) {
    HmacSha256Key key{std::string_view{"Jefe"}};
    auto tag = hmac_sha256_sign(
        key, std::string_view{"what do ya want for nothing?"});
    expect_tag_hex_eq(
        tag, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// RFC 4231 Test Case 3
// Key  = 0xaa × 20
// Data = 0xdd × 50
// HMAC = 773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe
TEST(Rfc4231, TestCase3) {
    auto key_bytes = hex_to_bytes("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    HmacSha256Key key{span_of(key_bytes)};
    std::vector<uint8_t> data(50, 0xdd);
    auto tag = hmac_sha256_sign(key, span_of(data));
    expect_tag_hex_eq(
        tag, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
}

// RFC 4231 Test Case 4
// Key  = 0x01..0x19 (25 bytes, ascending)
// Data = 0xcd × 50
// HMAC = 82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b
TEST(Rfc4231, TestCase4) {
    auto key_bytes =
        hex_to_bytes("0102030405060708090a0b0c0d0e0f10111213141516171819");
    HmacSha256Key key{span_of(key_bytes)};
    std::vector<uint8_t> data(50, 0xcd);
    auto tag = hmac_sha256_sign(key, span_of(data));
    expect_tag_hex_eq(
        tag, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
}

// RFC 4231 Test Case 5 (truncation test — our API returns full 32 bytes)
// Key  = 0x0c × 20
// Data = "Test With Truncation"
// HMAC = a3b6167473100ee06e0c796c2955552bfa6f7c0a6a8aef8b93f860aab0cd20c5
//        (RFC 4231 §4.6 lists the *untruncated* value; clients MAY truncate)
TEST(Rfc4231, TestCase5) {
    auto key_bytes = hex_to_bytes("0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c");
    HmacSha256Key key{span_of(key_bytes)};
    auto tag = hmac_sha256_sign(key, std::string_view{"Test With Truncation"});
    expect_tag_hex_eq(
        tag, "a3b6167473100ee06e0c796c2955552bfa6f7c0a6a8aef8b93f860aab0cd20c5");
}

// RFC 4231 Test Case 6 — key > 64 bytes, must be hashed first
// Key  = 0xaa × 131
// Data = "Test Using Larger Than Block-Size Key - Hash Key First"
// HMAC = 60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54
TEST(Rfc4231, TestCase6) {
    std::vector<uint8_t> key_bytes(131, 0xaa);
    HmacSha256Key key{span_of(key_bytes)};
    auto tag = hmac_sha256_sign(
        key,
        std::string_view{"Test Using Larger Than Block-Size Key - Hash Key First"});
    expect_tag_hex_eq(
        tag, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// RFC 4231 Test Case 7 — same long key, longer data
// Key  = 0xaa × 131
// Data = "This is a test using a larger than block-size key and a larger "
//        "than block-size data. The key needs to be hashed before being used "
//        "by the HMAC algorithm."
// HMAC = 9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2
TEST(Rfc4231, TestCase7) {
    std::vector<uint8_t> key_bytes(131, 0xaa);
    HmacSha256Key key{span_of(key_bytes)};
    auto tag = hmac_sha256_sign(
        key,
        std::string_view{
            "This is a test using a larger than block-size key and a larger "
            "than block-size data. The key needs to be hashed before being "
            "used by the HMAC algorithm."});
    expect_tag_hex_eq(
        tag, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

// ═════════════════════════════════════════════════════════════════════════════
// Key construction — normalization paths
// ═════════════════════════════════════════════════════════════════════════════

TEST(KeyConstruction, ShortKey8BytesZeroPadded) {
    // 8-byte key — zero-padded to 64 internally. Signing must still work.
    std::vector<uint8_t> k(8, 0x42);
    HmacSha256Key key{span_of(k)};
    auto tag = hmac_sha256_sign(key, std::string_view{"hello"});
    // Sanity: tag is not all-zero (would indicate sign failure).
    bool all_zero = true;
    for (auto b : tag.bytes) {
        if (b != 0) {
            all_zero = false;
            break;
        }
    }
    EXPECT_FALSE(all_zero);
}

TEST(KeyConstruction, Exactly64ByteKey) {
    // Exactly-64-byte key — normalization is identity (no pad, no hash).
    // Determinism check: sign twice and compare.
    std::vector<uint8_t> k(64, 0x5a);
    HmacSha256Key key1{span_of(k)};
    HmacSha256Key key2{span_of(k)};
    auto t1 = hmac_sha256_sign(key1, std::string_view{"data"});
    auto t2 = hmac_sha256_sign(key2, std::string_view{"data"});
    EXPECT_EQ(t1.bytes, t2.bytes);
}

TEST(KeyConstruction, Key65BytesHashed) {
    // 65 bytes triggers the hash-then-pad path (> 64).
    std::vector<uint8_t> k(65, 0x33);
    HmacSha256Key key{span_of(k)};
    auto tag = hmac_sha256_sign(key, std::string_view{"x"});
    // Hash-path keys must still produce deterministic, non-zero tags.
    bool all_zero = true;
    for (auto b : tag.bytes) {
        if (b != 0) {
            all_zero = false;
            break;
        }
    }
    EXPECT_FALSE(all_zero);
}

TEST(KeyConstruction, Key131BytesSameAsRfcCase6Key) {
    // Cross-check: constructing from 131-byte 0xaa via the span ctor and the
    // string_view ctor must produce identical tags.
    std::vector<uint8_t> k(131, 0xaa);
    std::string          k_str(131, static_cast<char>(0xaa));
    HmacSha256Key key_span{span_of(k)};
    HmacSha256Key key_sv{std::string_view{k_str}};
    auto t1 = hmac_sha256_sign(key_span, std::string_view{"cross"});
    auto t2 = hmac_sha256_sign(key_sv,   std::string_view{"cross"});
    EXPECT_EQ(t1.bytes, t2.bytes);
}

TEST(KeyConstruction, EmptyKey) {
    // Empty key is valid per HMAC RFC (all-zero 64-byte block).
    HmacSha256Key key{std::string_view{""}};
    auto tag = hmac_sha256_sign(key, std::string_view{""});
    // Known value: HMAC-SHA256("", "") ==
    //   b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad
    expect_tag_hex_eq(
        tag, "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
}

TEST(KeyConstruction, KeyFromStringView) {
    // string_view constructor must be byte-equivalent to the span constructor
    // for the same bytes.
    std::string sv = "my-binance-api-secret";
    HmacSha256Key k_sv{std::string_view{sv}};
    HmacSha256Key k_sp{std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(sv.data()), sv.size()}};
    auto t1 = hmac_sha256_sign(k_sv, std::string_view{"payload"});
    auto t2 = hmac_sha256_sign(k_sp, std::string_view{"payload"});
    EXPECT_EQ(t1.bytes, t2.bytes);
}

// ═════════════════════════════════════════════════════════════════════════════
// Tag hex encoding
// ═════════════════════════════════════════════════════════════════════════════

TEST(TagToHex, SpanWritesExactly64Bytes) {
    HmacSha256Tag tag{};
    for (size_t i = 0; i < 32; ++i) {
        tag.bytes[i] = static_cast<uint8_t>(i);
    }
    uint8_t out[64] = {0xCC}; // sentinel — should be overwritten
    auto written = tag.to_hex(std::span<uint8_t, 64>{out});
    EXPECT_EQ(written, 64u);
    std::string_view sv{reinterpret_cast<const char*>(out), 64};
    EXPECT_EQ(sv, "000102030405060708090a0b0c0d0e0f"
                  "101112131415161718191a1b1c1d1e1f");
}

TEST(TagToHex, StringReturns64Chars) {
    HmacSha256Tag tag{};
    for (size_t i = 0; i < 32; ++i) {
        tag.bytes[i] = static_cast<uint8_t>(0xff - i);
    }
    std::string s = tag.to_hex();
    EXPECT_EQ(s.size(), 64u);
    EXPECT_EQ(s, "fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0"
                 "efeeedecebeae9e8e7e6e5e4e3e2e1e0");
}

TEST(TagToHex, Lowercase) {
    // Binance/Bybit convention: signatures are lowercase hex.
    HmacSha256Tag tag{};
    tag.bytes[0] = 0xAB;
    tag.bytes[1] = 0xCD;
    tag.bytes[2] = 0xEF;
    std::string s = tag.to_hex();
    EXPECT_EQ(s.substr(0, 6), "abcdef");
    // Verify no uppercase anywhere.
    for (char c : s) {
        EXPECT_FALSE(c >= 'A' && c <= 'F') << "Unexpected uppercase: " << c;
    }
}

TEST(TagToHex, SpanAndStringAgree) {
    HmacSha256Key key{std::string_view{"k"}};
    auto          tag = hmac_sha256_sign(key, std::string_view{"m"});

    uint8_t buf[64];
    tag.to_hex(std::span<uint8_t, 64>{buf});
    std::string_view span_view{reinterpret_cast<const char*>(buf), 64};
    std::string      alloc = tag.to_hex();
    EXPECT_EQ(alloc, span_view);
}

// ═════════════════════════════════════════════════════════════════════════════
// Destructor zero-on-destroy — best-effort reach-in test
// ═════════════════════════════════════════════════════════════════════════════

TEST(KeyDestructor, ClearsBufferOnDestroy) {
    // Strategy: allocate a Key in a stack frame backed by a known-sized
    // scratch buffer that we zero first, placement-new the key into it,
    // run its destructor explicitly, and then check that the bytes where
    // the key lived are now zero.
    //
    // This is the most portable "zero-on-destroy" verification — the
    // compiler is not allowed to elide OPENSSL_cleanse, so the memory
    // MUST be zero after the destructor returns, regardless of optimizer
    // level.
    alignas(HmacSha256Key) unsigned char storage[sizeof(HmacSha256Key)]{};
    std::memset(storage, 0xAA, sizeof(storage)); // poison

    {
        auto* kp = new (storage) HmacSha256Key{std::string_view{"secret-123"}};
        // After construction the buffer contains the key bytes — it is NOT
        // all 0xAA anymore.
        bool saw_non_poison = false;
        for (auto byte : storage) {
            if (byte != 0xAA) {
                saw_non_poison = true;
                break;
            }
        }
        EXPECT_TRUE(saw_non_poison);
        kp->~HmacSha256Key();
    }

    // After explicit destruction, all 64 bytes of the `normalized_` array
    // must be zero (the rest of the object is empty — the buffer IS the
    // whole object).
    for (size_t i = 0; i < sizeof(storage); ++i) {
        EXPECT_EQ(storage[i], 0u)
            << "byte " << i << " was not zeroed on destroy";
    }
}

TEST(KeyDestructor, MoveConstructorClearsSource) {
    alignas(HmacSha256Key) unsigned char storage_src[sizeof(HmacSha256Key)]{};
    alignas(HmacSha256Key) unsigned char storage_dst[sizeof(HmacSha256Key)]{};

    auto* src = new (storage_src) HmacSha256Key{std::string_view{"api-secret"}};
    auto* dst = new (storage_dst) HmacSha256Key{std::move(*src)};

    // Source buffer should now be all-zero (move ctor cleanses it).
    for (size_t i = 0; i < sizeof(storage_src); ++i) {
        EXPECT_EQ(storage_src[i], 0u)
            << "moved-from src byte " << i << " not zeroed";
    }
    // Destination must still produce a valid signature — i.e. the key
    // material actually made it across the move.
    auto tag = hmac_sha256_sign(*dst, std::string_view{"payload"});
    bool all_zero = true;
    for (auto b : tag.bytes) {
        if (b != 0) {
            all_zero = false;
            break;
        }
    }
    EXPECT_FALSE(all_zero);

    src->~HmacSha256Key();
    dst->~HmacSha256Key();
}

// ═════════════════════════════════════════════════════════════════════════════
// Edge cases — empty data, large data, determinism, separation
// ═════════════════════════════════════════════════════════════════════════════

TEST(Sign, EmptyDataWithNonEmptyKey) {
    HmacSha256Key key{std::string_view{"some-secret"}};
    auto tag = hmac_sha256_sign(key, std::string_view{""});
    // Signing empty data must not crash and must produce a deterministic tag.
    auto tag2 = hmac_sha256_sign(key, std::string_view{""});
    EXPECT_EQ(tag.bytes, tag2.bytes);
}

TEST(Sign, LargeData1MiB) {
    // 1 MiB of 'X' — exercises the HMAC inner hash over many blocks.
    std::string msg(1u << 20, 'X');
    HmacSha256Key key{std::string_view{"k"}};
    auto tag = hmac_sha256_sign(key, std::string_view{msg});
    // Signing twice is deterministic.
    auto tag2 = hmac_sha256_sign(key, std::string_view{msg});
    EXPECT_EQ(tag.bytes, tag2.bytes);
}

TEST(Sign, DeterministicSameKeySameData) {
    HmacSha256Key key{std::string_view{"det-key"}};
    auto t1 = hmac_sha256_sign(key, std::string_view{"det-data"});
    auto t2 = hmac_sha256_sign(key, std::string_view{"det-data"});
    EXPECT_EQ(t1.bytes, t2.bytes);
}

TEST(Sign, DifferentKeysProduceDifferentTags) {
    HmacSha256Key k1{std::string_view{"key-a"}};
    HmacSha256Key k2{std::string_view{"key-b"}};
    auto t1 = hmac_sha256_sign(k1, std::string_view{"same data"});
    auto t2 = hmac_sha256_sign(k2, std::string_view{"same data"});
    EXPECT_NE(t1.bytes, t2.bytes);
}

TEST(Sign, DifferentDataProducesDifferentTags) {
    HmacSha256Key key{std::string_view{"same-key"}};
    auto t1 = hmac_sha256_sign(key, std::string_view{"message-a"});
    auto t2 = hmac_sha256_sign(key, std::string_view{"message-b"});
    EXPECT_NE(t1.bytes, t2.bytes);
}

TEST(Sign, SpanAndStringViewOverloadsAgree) {
    HmacSha256Key key{std::string_view{"api-secret"}};
    std::string   msg = "symbol=BTCUSDT&side=BUY";
    auto t1 = hmac_sha256_sign(key, std::string_view{msg});
    auto t2 = hmac_sha256_sign(
        key,
        std::span<const uint8_t>{
            reinterpret_cast<const uint8_t*>(msg.data()), msg.size()});
    EXPECT_EQ(t1.bytes, t2.bytes);
}

TEST(Sign, ConsecutiveSignsDoNotCorruptKey) {
    // Regression guard: HMAC() is stateless but if a future refactor moved
    // to EVP_MAC_*/HMAC_Init/Update/Final, the key buffer might accidentally
    // be mutated. Sign twice and make sure the second signature matches the
    // first for the same input.
    HmacSha256Key key{std::string_view{"persistent-key"}};
    auto t1 = hmac_sha256_sign(key, std::string_view{"msg-1"});
    auto t_other = hmac_sha256_sign(key, std::string_view{"msg-2"});
    (void)t_other;
    auto t1_again = hmac_sha256_sign(key, std::string_view{"msg-1"});
    EXPECT_EQ(t1.bytes, t1_again.bytes);
}

TEST(Sign, MoveThenSignProducesSameTagAsOriginal) {
    HmacSha256Key k1{std::string_view{"movable-key"}};
    auto t_before = hmac_sha256_sign(k1, std::string_view{"data"});

    HmacSha256Key k2{std::move(k1)};
    auto t_after = hmac_sha256_sign(k2, std::string_view{"data"});
    EXPECT_EQ(t_before.bytes, t_after.bytes);
}

TEST(Sign, MoveAssignmentWorks) {
    HmacSha256Key k1{std::string_view{"aaa"}};
    HmacSha256Key k2{std::string_view{"bbb"}};
    auto before_k2 = hmac_sha256_sign(k2, std::string_view{"x"});
    k1 = std::move(k2);
    auto after_k1 = hmac_sha256_sign(k1, std::string_view{"x"});
    // k1 now holds what was k2's material.
    EXPECT_EQ(before_k2.bytes, after_k1.bytes);
}

// ═════════════════════════════════════════════════════════════════════════════
// Realistic exchange-style usage
// ═════════════════════════════════════════════════════════════════════════════

TEST(ExchangeStyle, BinanceSignatureShapeIsLowercaseHex64) {
    // Canonical Binance signing: HMAC-SHA256 of the query string with the
    // API secret, then lowercase hex.
    HmacSha256Key key{std::string_view{
        "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j"}};
    std::string_view qs =
        "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC"
        "&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";
    auto tag = hmac_sha256_sign(key, qs);

    std::string hex = tag.to_hex();
    EXPECT_EQ(hex.size(), 64u);
    for (char c : hex) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(ok) << "non-hex char in tag: " << c;
    }
    // Known value (computed with Python `hmac.new(..., hashlib.sha256)`).
    EXPECT_EQ(hex,
              "c8db56825ae71d6d79447849e617115f"
              "4a920fa2acdcab2b053c4b2838bd6b71");
}

TEST(ExchangeStyle, ReusedKeyAcrossRequests) {
    // Realistic pattern: construct the key once per adapter, sign N requests.
    HmacSha256Key key{std::string_view{"api-secret-kept-for-whole-session"}};
    std::array<std::string_view, 4> payloads{
        "ts=1&symbol=BTCUSDT",
        "ts=2&symbol=ETHUSDT",
        "ts=3&symbol=SOLUSDT",
        "ts=4&symbol=XRPUSDT",
    };
    std::array<HmacSha256Tag, 4> tags;
    for (size_t i = 0; i < payloads.size(); ++i) {
        tags[i] = hmac_sha256_sign(key, payloads[i]);
    }
    // Sign again, must be identical (key is immutable across signs).
    for (size_t i = 0; i < payloads.size(); ++i) {
        auto again = hmac_sha256_sign(key, payloads[i]);
        EXPECT_EQ(tags[i].bytes, again.bytes);
    }
    // All 4 signatures are distinct (different payloads).
    for (size_t i = 0; i < payloads.size(); ++i) {
        for (size_t j = i + 1; j < payloads.size(); ++j) {
            EXPECT_NE(tags[i].bytes, tags[j].bytes);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Tag is a POD — can be copied, compared, stored in containers
// ═════════════════════════════════════════════════════════════════════════════

TEST(TagSemantics, CopyableAndComparable) {
    static_assert(std::is_trivially_copyable_v<HmacSha256Tag>);
    HmacSha256Key key{std::string_view{"k"}};
    auto          t1 = hmac_sha256_sign(key, std::string_view{"m"});
    HmacSha256Tag t2 = t1;
    EXPECT_EQ(t1.bytes, t2.bytes);
    t2.bytes[0] ^= 1;
    EXPECT_NE(t1.bytes, t2.bytes);
}

TEST(TagSemantics, DefaultConstructedIsZero) {
    HmacSha256Tag tag{};
    for (auto b : tag.bytes) {
        EXPECT_EQ(b, 0u);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Extra coverage — single-byte permutations, alignment, and bytes API
// ═════════════════════════════════════════════════════════════════════════════

TEST(TagToHex, AllZeroBytes) {
    HmacSha256Tag tag{};
    std::string   s = tag.to_hex();
    EXPECT_EQ(s, std::string(64, '0'));
}

TEST(TagToHex, AllFfBytes) {
    HmacSha256Tag tag{};
    for (auto& b : tag.bytes) b = 0xff;
    std::string s = tag.to_hex();
    EXPECT_EQ(s, std::string(64, 'f'));
}

TEST(TagToHex, NibbleBoundary) {
    // 0x10 -> "10", 0x01 -> "01" — verifies byte-order vs nibble-order.
    HmacSha256Tag tag{};
    tag.bytes[0] = 0x10;
    tag.bytes[1] = 0x01;
    std::string s = tag.to_hex();
    EXPECT_EQ(s.substr(0, 4), "1001");
}

TEST(KeyAlignment, BufferIsCacheLineAligned) {
    // alignof must be at least 64 (matches our `alignas(64)` requirement).
    static_assert(alignof(HmacSha256Key) >= 64,
                  "HmacSha256Key must be cache-line aligned");
    HmacSha256Key key{std::string_view{"x"}};
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&key) % 64u, 0u);
}

TEST(Rfc4231, TestCase1_TagBytesMatchRfc) {
    // Same vector as TestCase1 but checking the raw `bytes` field directly
    // instead of the hex encoding. Ensures the byte order matches the RFC
    // (and catches a future to_hex bug where hex works but bytes don't).
    auto          key_bytes = hex_to_bytes("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    HmacSha256Key key{span_of(key_bytes)};
    auto          tag = hmac_sha256_sign(key, std::string_view{"Hi There"});
    const std::array<uint8_t, 32> expected{
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
    };
    EXPECT_EQ(tag.bytes, expected);
}

TEST(Rfc4231, TestCase2_TagBytesMatchRfc) {
    HmacSha256Key key{std::string_view{"Jefe"}};
    auto          tag = hmac_sha256_sign(
        key, std::string_view{"what do ya want for nothing?"});
    const std::array<uint8_t, 32> expected{
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
        0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
        0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
    };
    EXPECT_EQ(tag.bytes, expected);
}

TEST(Sign, SingleByteData) {
    // Smallest non-empty input.
    HmacSha256Key key{std::string_view{"k"}};
    uint8_t       byte = 0x42;
    auto          tag  = hmac_sha256_sign(key, std::span<const uint8_t>{&byte, 1});
    auto          tag2 = hmac_sha256_sign(key, std::span<const uint8_t>{&byte, 1});
    EXPECT_EQ(tag.bytes, tag2.bytes);
}

TEST(KeyConstruction, KeyWithEmbeddedNulls) {
    // Binary keys often contain NUL bytes — make sure we don't truncate on
    // them (we use span/size, not strlen). Use the span ctor explicitly.
    std::array<uint8_t, 16> k{};
    k[0]  = 0x01;
    k[7]  = 0x00; // explicit NUL in the middle
    k[15] = 0xff;
    HmacSha256Key key1{std::span<const uint8_t>{k.data(), k.size()}};
    // And compare against a key that has a different byte at index 8 —
    // must produce a different tag.
    auto k2 = k;
    k2[8]   = 0xaa;
    HmacSha256Key key2{std::span<const uint8_t>{k2.data(), k2.size()}};
    auto t1 = hmac_sha256_sign(key1, std::string_view{"data"});
    auto t2 = hmac_sha256_sign(key2, std::string_view{"data"});
    EXPECT_NE(t1.bytes, t2.bytes);
}

TEST(Sign, Data64BytesExactBlockBoundary) {
    // SHA-256 block size is 64 bytes — signing exactly-block-boundary data
    // exercises the final padding path.
    HmacSha256Key key{std::string_view{"k"}};
    std::string   msg(64, 'A');
    auto          t1 = hmac_sha256_sign(key, std::string_view{msg});
    auto          t2 = hmac_sha256_sign(key, std::string_view{msg});
    EXPECT_EQ(t1.bytes, t2.bytes);
    // And one byte less / one byte more must produce different tags.
    std::string msg_less(63, 'A');
    std::string msg_more(65, 'A');
    auto        t_less = hmac_sha256_sign(key, std::string_view{msg_less});
    auto        t_more = hmac_sha256_sign(key, std::string_view{msg_more});
    EXPECT_NE(t1.bytes, t_less.bytes);
    EXPECT_NE(t1.bytes, t_more.bytes);
}
