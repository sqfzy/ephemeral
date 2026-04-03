/// @file test_hmac.cpp
/// Unit tests for HMAC-SHA256 signing utility.
///
/// Test vectors sourced from RFC 4231 (HMAC-SHA256 test cases).
/// Also includes exchange-specific signature examples (Binance-style).

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "eph/net/hmac.hpp"

using namespace eph::net;

// ─────────────────────────────────────────────────────────────────────────────
// Hex encoding
// ─────────────────────────────────────────────────────────────────────────────

TEST(ToHex, EmptyInput) {
    std::vector<uint8_t> empty;
    EXPECT_EQ(to_hex(empty), "");
}

TEST(ToHex, SingleByte) {
    std::array<uint8_t, 1> data{0xAB};
    EXPECT_EQ(to_hex(data), "ab");
}

TEST(ToHex, MultipleBytesLowercase) {
    std::array<uint8_t, 4> data{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(to_hex(data), "deadbeef");
}

TEST(ToHex, AllZeroes) {
    std::array<uint8_t, 3> data{0x00, 0x00, 0x00};
    EXPECT_EQ(to_hex(data), "000000");
}

TEST(ToHex, AllOnes) {
    std::array<uint8_t, 2> data{0xFF, 0xFF};
    EXPECT_EQ(to_hex(data), "ffff");
}

// ─────────────────────────────────────────────────────────────────────────────
// Base64 encoding
// ─────────────────────────────────────────────────────────────────────────────

TEST(ToBase64, EmptyInput) {
    std::vector<uint8_t> empty;
    auto result = to_base64(empty);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "");
}

TEST(ToBase64, HelloWorld) {
    // "Hello" -> "SGVsbG8="
    std::string_view input = "Hello";
    std::vector<uint8_t> bytes(input.begin(), input.end());
    auto result = to_base64(bytes);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "SGVsbG8=");
}

TEST(ToBase64, ThreeBytes) {
    // "Man" -> "TWFu" (no padding)
    std::string_view input = "Man";
    std::vector<uint8_t> bytes(input.begin(), input.end());
    auto result = to_base64(bytes);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "TWFu");
}

TEST(ToBase64, SingleByte) {
    // "M" -> "TQ=="
    std::vector<uint8_t> bytes{'M'};
    auto result = to_base64(bytes);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "TQ==");
}

TEST(ToBase64, TwoBytes) {
    // "Ma" -> "TWE="
    std::vector<uint8_t> bytes{'M', 'a'};
    auto result = to_base64(bytes);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "TWE=");
}

// ─────────────────────────────────────────────────────────────────────────────
// HMAC-SHA256 — RFC 4231 test vectors
// ─────────────────────────────────────────────────────────────────────────────

// Helper: convert hex string to byte vector for test key/data setup.
static std::vector<uint8_t> hex_to_bytes(std::string_view hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        auto hi = hex[i];
        auto lo = hex[i + 1];
        auto nibble = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        bytes.push_back(static_cast<uint8_t>((nibble(hi) << 4) | nibble(lo)));
    }
    return bytes;
}

// RFC 4231 Test Case 1
// Key = 0x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b (20 bytes)
// Data = "Hi There"
// HMAC-SHA-256 = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
TEST(HmacSha256, Rfc4231TestCase1) {
    auto key_bytes = hex_to_bytes("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    std::string_view key(reinterpret_cast<const char*>(key_bytes.data()), key_bytes.size());
    std::string_view data = "Hi There";

    auto result = hmac_sha256_hex(key, data);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

// RFC 4231 Test Case 2
// Key = "Jefe"
// Data = "what do ya want for nothing?"
// HMAC-SHA-256 = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
TEST(HmacSha256, Rfc4231TestCase2) {
    auto result = hmac_sha256_hex("Jefe", "what do ya want for nothing?");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// RFC 4231 Test Case 3
// Key = 0xaaaa... (20 bytes)
// Data = 0xdddd... (50 bytes)
// HMAC-SHA-256 = 773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe
TEST(HmacSha256, Rfc4231TestCase3) {
    auto key_bytes = hex_to_bytes("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    std::string_view key(reinterpret_cast<const char*>(key_bytes.data()), key_bytes.size());

    std::string data_hex;
    for (int i = 0; i < 50; ++i) data_hex += "dd";
    auto data_bytes = hex_to_bytes(data_hex);
    std::string_view data(reinterpret_cast<const char*>(data_bytes.data()), data_bytes.size());

    auto result = hmac_sha256_hex(key, data);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
}

// RFC 4231 Test Case 4
// Key = 0x0102030405060708090a0b0c0d0e0f10111213141516171819 (25 bytes)
// Data = 0xcdcdcd... (50 bytes)
// HMAC-SHA-256 = 82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b
TEST(HmacSha256, Rfc4231TestCase4) {
    auto key_bytes = hex_to_bytes("0102030405060708090a0b0c0d0e0f10111213141516171819");
    std::string_view key(reinterpret_cast<const char*>(key_bytes.data()), key_bytes.size());

    std::string data_hex;
    for (int i = 0; i < 50; ++i) data_hex += "cd";
    auto data_bytes = hex_to_bytes(data_hex);
    std::string_view data(reinterpret_cast<const char*>(data_bytes.data()), data_bytes.size());

    auto result = hmac_sha256_hex(key, data);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases: empty key and empty message
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacSha256, EmptyKeyEmptyMessage) {
    // HMAC-SHA256("", "") is well-defined.
    auto result = hmac_sha256_hex("", "");
    ASSERT_TRUE(result.has_value()) << result.error();
    // Known value: HMAC-SHA256 with empty key and empty data.
    EXPECT_EQ(*result, "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    EXPECT_EQ(result->size(), 64u) << "hex digest must be 64 chars (32 bytes)";
}

TEST(HmacSha256, EmptyMessage) {
    auto result = hmac_sha256_hex("secret", "");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->size(), 64u);
}

TEST(HmacSha256, EmptyKey) {
    auto result = hmac_sha256_hex("", "message");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->size(), 64u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Raw digest returns 32 bytes
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacSha256, RawDigestIs32Bytes) {
    auto result = hmac_sha256("key", "data");
    ASSERT_TRUE(result.has_value()) << result.error();
    // std::array<uint8_t, 32> — size is compile-time, but verify at runtime too.
    EXPECT_EQ(result->size(), 32u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Binance-style signature example
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacSha256, BinanceStyleSignature) {
    // Simulates Binance API signature: HMAC-SHA256 of query string with API secret.
    // This is a synthetic example — not a real key/secret.
    std::string_view api_secret = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
    std::string_view query_string =
        "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC"
        "&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";

    auto result = hmac_sha256_hex(api_secret, query_string);
    ASSERT_TRUE(result.has_value()) << result.error();
    // The signature is a 64-char hex string.
    EXPECT_EQ(result->size(), 64u);
    // Verify the actual signature value (computed with Python hmac module).
    EXPECT_EQ(*result, "c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71");
}

// ─────────────────────────────────────────────────────────────────────────────
// OKX-style base64 signature
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacSha256, OkxStyleBase64Signature) {
    // OKX signs with HMAC-SHA256 then base64-encodes the raw digest.
    std::string_view secret = "test-secret-key";
    std::string_view prehash = "2023-01-01T00:00:00.000ZGET/api/v5/account/balance";

    auto digest = hmac_sha256(secret, prehash);
    ASSERT_TRUE(digest.has_value()) << digest.error();

    auto b64 = to_base64(*digest);
    ASSERT_TRUE(b64.has_value()) << b64.error();

    // Base64 of 32 bytes = 44 chars (with padding).
    EXPECT_EQ(b64->size(), 44u);
    // Verify all characters are valid base64.
    for (char c : *b64) {
        EXPECT_TRUE(std::isalnum(c) || c == '+' || c == '/' || c == '=')
            << "Invalid base64 char: " << c;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Consistency: hex(raw) == hex_convenience
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacSha256, HexConvenienceMatchesManual) {
    std::string_view key = "my-secret";
    std::string_view msg = "some-query-string&timestamp=12345";

    auto raw = hmac_sha256(key, msg);
    ASSERT_TRUE(raw.has_value()) << raw.error();

    auto hex_convenience = hmac_sha256_hex(key, msg);
    ASSERT_TRUE(hex_convenience.has_value()) << hex_convenience.error();

    EXPECT_EQ(to_hex(*raw), *hex_convenience);
}

// ─────────────────────────────────────────────────────────────────────────────
// HMAC verification (constant-time)
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacVerify, CorrectDigestReturnsTrue) {
    std::string_view key = "test-key";
    std::string_view msg = "test-message";
    auto digest = hmac_sha256(key, msg);
    ASSERT_TRUE(digest.has_value());
    EXPECT_TRUE(hmac_verify(key, msg, *digest));
}

TEST(HmacVerify, WrongDigestReturnsFalse) {
    std::string_view key = "test-key";
    std::string_view msg = "test-message";
    std::array<uint8_t, 32> wrong{};  // all zeros
    EXPECT_FALSE(hmac_verify(key, msg, wrong));
}

TEST(HmacVerify, DifferentMessageReturnsFalse) {
    std::string_view key = "test-key";
    auto digest = hmac_sha256(key, "message-a");
    ASSERT_TRUE(digest.has_value());
    EXPECT_FALSE(hmac_verify(key, "message-b", *digest));
}

TEST(HmacVerifyHex, CorrectHexReturnsTrue) {
    std::string_view key = "test-key";
    std::string_view msg = "test-message";
    auto hex = hmac_sha256_hex(key, msg);
    ASSERT_TRUE(hex.has_value());
    EXPECT_TRUE(hmac_verify_hex(key, msg, *hex));
}

TEST(HmacVerifyHex, WrongHexReturnsFalse) {
    std::string_view key = "test-key";
    std::string_view msg = "test-message";
    std::string wrong_hex(64, '0');
    EXPECT_FALSE(hmac_verify_hex(key, msg, wrong_hex));
}

TEST(HmacVerifyHex, WrongLengthReturnsFalse) {
    std::string_view key = "test-key";
    std::string_view msg = "test-message";
    EXPECT_FALSE(hmac_verify_hex(key, msg, "tooshort"));
}

TEST(HmacVerifyHex, EmptyExpectedReturnsFalse) {
    std::string_view key = "test-key";
    std::string_view msg = "test-message";
    EXPECT_FALSE(hmac_verify_hex(key, msg, ""));
}

// ─────────────────────────────────────────────────────────────────────────────
// HMAC-SHA256 with large message
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacSha256, LargeMessage4KB) {
    // Verify HMAC works correctly with a 4KB message (typical orderbook snapshot).
    std::string msg(4096, 'A');
    auto result = hmac_sha256_hex("key", msg);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->size(), 64u);
}

TEST(HmacSha256, LargeMessage64KB) {
    // Verify HMAC works correctly with a 64KB message.
    std::string msg(65536, 'B');
    auto result = hmac_sha256_hex("key", msg);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->size(), 64u);
}

TEST(HmacSha256, DeterministicWithSameInput) {
    // Same key + message must always produce the same signature.
    auto r1 = hmac_sha256_hex("key", "data");
    auto r2 = hmac_sha256_hex("key", "data");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r1, *r2);
}

TEST(HmacSha256, DifferentKeysProduceDifferentDigests) {
    auto r1 = hmac_sha256_hex("key-a", "same data");
    auto r2 = hmac_sha256_hex("key-b", "same data");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_NE(*r1, *r2);
}

TEST(HmacSha256, DifferentMessagesProduceDifferentDigests) {
    auto r1 = hmac_sha256_hex("same-key", "message-a");
    auto r2 = hmac_sha256_hex("same-key", "message-b");
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_NE(*r1, *r2);
}

// ─────────────────────────────────────────────────────────────────────────────
// to_hex edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(ToHex, SequentialBytes) {
    std::array<uint8_t, 16> data;
    for (size_t i = 0; i < 16; ++i) data[i] = static_cast<uint8_t>(i);
    EXPECT_EQ(to_hex(data), "000102030405060708090a0b0c0d0e0f");
}

TEST(ToHex, HighNibblePattern) {
    // Verify high nibble encoding for 0xF0, 0xA0, etc.
    std::array<uint8_t, 3> data{0xF0, 0xA0, 0x50};
    EXPECT_EQ(to_hex(data), "f0a050");
}

// ─────────────────────────────────────────────────────────────────────────────
// hmac_sha256_base64 — convenience function
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacSha256Base64, ProducesCorrectLength) {
    // Base64 of 32 bytes = 44 chars (with padding).
    auto result = hmac_sha256_base64("key", "message");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->size(), 44u);
}

TEST(HmacSha256Base64, AllCharsAreValidBase64) {
    auto result = hmac_sha256_base64("secret", "data");
    ASSERT_TRUE(result.has_value()) << result.error();
    for (char c : *result) {
        EXPECT_TRUE(std::isalnum(c) || c == '+' || c == '/' || c == '=')
            << "Invalid base64 char: " << c;
    }
}

TEST(HmacSha256Base64, MatchesManualPipeline) {
    // Verify convenience function produces same result as manual hmac_sha256 + to_base64.
    std::string_view key = "test-secret-key";
    std::string_view msg = "2023-01-01T00:00:00.000ZGET/api/v5/account/balance";

    auto convenience = hmac_sha256_base64(key, msg);
    ASSERT_TRUE(convenience.has_value()) << convenience.error();

    auto digest = hmac_sha256(key, msg);
    ASSERT_TRUE(digest.has_value()) << digest.error();
    auto manual = to_base64(*digest);
    ASSERT_TRUE(manual.has_value()) << manual.error();

    EXPECT_EQ(*convenience, *manual);
}

TEST(HmacSha256Base64, EmptyKeyAndMessage) {
    auto result = hmac_sha256_base64("", "");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->size(), 44u);
}

TEST(HmacSha256Base64, DifferentKeysProduceDifferentSignatures) {
    auto sig1 = hmac_sha256_base64("key-a", "same message");
    auto sig2 = hmac_sha256_base64("key-b", "same message");
    ASSERT_TRUE(sig1.has_value());
    ASSERT_TRUE(sig2.has_value());
    EXPECT_NE(*sig1, *sig2);
}

// ─────────────────────────────────────────────────────────────────────────────
// hmac_verify_base64 — constant-time base64 verification
// ─────────────────────────────────────────────────────────────────────────────

TEST(HmacVerifyBase64, CorrectBase64ReturnsTrue) {
    std::string_view key = "test-key";
    std::string_view msg = "test-message";
    auto b64 = hmac_sha256_base64(key, msg);
    ASSERT_TRUE(b64.has_value());
    EXPECT_TRUE(hmac_verify_base64(key, msg, *b64));
}

TEST(HmacVerifyBase64, WrongBase64ReturnsFalse) {
    std::string_view key = "test-key";
    std::string_view msg = "test-message";
    // 44-char base64 string that won't match
    std::string wrong_b64(44, 'A');
    EXPECT_FALSE(hmac_verify_base64(key, msg, wrong_b64));
}

TEST(HmacVerifyBase64, WrongLengthReturnsFalse) {
    std::string_view key = "test-key";
    std::string_view msg = "test-message";
    EXPECT_FALSE(hmac_verify_base64(key, msg, "tooshort"));
}

TEST(HmacVerifyBase64, EmptyExpectedReturnsFalse) {
    std::string_view key = "test-key";
    std::string_view msg = "test-message";
    EXPECT_FALSE(hmac_verify_base64(key, msg, ""));
}

TEST(HmacVerifyBase64, DifferentMessageReturnsFalse) {
    std::string_view key = "test-key";
    auto b64 = hmac_sha256_base64(key, "message-a");
    ASSERT_TRUE(b64.has_value());
    EXPECT_FALSE(hmac_verify_base64(key, "message-b", *b64));
}
