/// @file test_base64.cpp
/// Unit tests for eph::core::detail::base64_encode() — RFC 4648 test vectors
/// and boundary cases.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/detail/base64.hpp"

using eph::core::detail::base64_encode;

// ----------------------------------------------------------------------------
// RFC 4648 S10 test vectors
// ----------------------------------------------------------------------------

struct Rfc4648Vector {
    std::string input;
    std::string expected;
};

class Base64Rfc4648 : public ::testing::TestWithParam<Rfc4648Vector> {};

TEST_P(Base64Rfc4648, EncodeMatchesExpected) {
    const auto& [input, expected] = GetParam();
    EXPECT_EQ(base64_encode(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
    Rfc4648Vectors,
    Base64Rfc4648,
    ::testing::Values(
        Rfc4648Vector{"",       ""},
        Rfc4648Vector{"f",      "Zg=="},
        Rfc4648Vector{"fo",     "Zm8="},
        Rfc4648Vector{"foo",    "Zm9v"},
        Rfc4648Vector{"foob",   "Zm9vYg=="},
        Rfc4648Vector{"fooba",  "Zm9vYmE="},
        Rfc4648Vector{"foobar", "Zm9vYmFy"}
    ));

// ----------------------------------------------------------------------------
// Padding variations: 1, 2, 3 bytes (covering all mod-3 remainders)
// ----------------------------------------------------------------------------

TEST(Base64Encode, SingleByte) {
    // 'A' = 0x41 => "QQ=="
    const uint8_t data[] = {0x41};
    EXPECT_EQ(base64_encode(data, 1), "QQ==");
}

TEST(Base64Encode, TwoBytes) {
    // 'AB' = 0x41 0x42 => "QUI="
    const uint8_t data[] = {0x41, 0x42};
    EXPECT_EQ(base64_encode(data, 2), "QUI=");
}

TEST(Base64Encode, ThreeBytes_NoPadding) {
    // 'ABC' = 0x41 0x42 0x43 => "QUJD"
    const uint8_t data[] = {0x41, 0x42, 0x43};
    EXPECT_EQ(base64_encode(data, 3), "QUJD");
}

// ----------------------------------------------------------------------------
// Binary data encoding (non-printable bytes)
// ----------------------------------------------------------------------------

TEST(Base64Encode, BinaryData) {
    const uint8_t data[] = {0x00, 0xFF, 0x80, 0x7F, 0x01, 0xFE};
    std::string result = base64_encode(data, sizeof(data));
    // Verify round-trip expectation: AP+AfwH+
    EXPECT_EQ(result, "AP+AfwH+");
}

TEST(Base64Encode, AllZeroBytes) {
    const uint8_t data[] = {0x00, 0x00, 0x00};
    EXPECT_EQ(base64_encode(data, 3), "AAAA");
}

TEST(Base64Encode, AllOnesBytes) {
    const uint8_t data[] = {0xFF, 0xFF, 0xFF};
    EXPECT_EQ(base64_encode(data, 3), "////");
}

// Pointer-overload zero-length: the string-overload empty case is
// covered above via `Rfc4648Vector{"", ""}`, but the byte-pointer
// overload's `len==0` short-circuit (the for-loop body never runs)
// is a separate path. Pin it: `base64_encode(data, 0)` returns "" no
// matter what `data` points at — so passing nullptr is also safe.
TEST(Base64Encode, PointerOverloadZeroLengthReturnsEmpty) {
    const uint8_t data[] = {0xAB, 0xCD};
    EXPECT_EQ(base64_encode(data, 0), "");
    EXPECT_EQ(base64_encode(nullptr, 0), "");
}

// Round-trip the canonical Sec-WebSocket-Accept input shape. The
// existing tests use small printable inputs; this pins the encoder
// against real-world WS-handshake usage where the input is a 20-byte
// SHA-1 digest of ASCII, NOT another printable string.
TEST(Base64Encode, Sha1DigestShapeProducesTwentyEightCharsWithEqualsPadding) {
    // 20-byte input — SHA-1 output length. Encodes to ceil(20/3)*4 =
    // 28 chars including '=' padding (since 20 mod 3 == 2 → one '=').
    const uint8_t digest[] = {
        0xb3, 0x7a, 0x4f, 0x2c, 0xc0, 0x62, 0x4f, 0x16, 0x90, 0xf6,
        0x46, 0x06, 0xcf, 0x38, 0x59, 0x45, 0xb2, 0xbe, 0xc4, 0xea};
    auto out = base64_encode(digest, sizeof(digest));
    EXPECT_EQ(out.size(), 28u);
    // Last char must be '=' (the single-pad case for mod-3==2 inputs).
    EXPECT_EQ(out.back(), '=');
    // Second-to-last must NOT be '=' (a 20-byte input only needs one
    // pad char; two-padding would mean a 19-byte / mod-3==1 input).
    EXPECT_NE(out[out.size() - 2], '=');
}
