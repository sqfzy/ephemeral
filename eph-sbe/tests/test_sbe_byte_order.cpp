#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "eph/sbe.hpp"

using namespace eph::sbe;

// ===========================================================================
// Little-endian unsigned reads
// ===========================================================================

TEST(SbeByteOrder, read_le_unsigned_decodes_known_patterns) {
    // 0x01 0x02 ... laid out little-endian.
    std::array<uint8_t, 8> b{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    EXPECT_EQ(read_le16(b.data()), 0x0201u);
    EXPECT_EQ(read_le32(b.data()), 0x04030201u);
    EXPECT_EQ(read_le64(b.data()), 0x0807060504030201ull);
}

TEST(SbeByteOrder, read_le_unsigned_max_values) {
    std::array<uint8_t, 8> b;
    b.fill(0xFF);
    EXPECT_EQ(read_le16(b.data()), 0xFFFFu);
    EXPECT_EQ(read_le32(b.data()), 0xFFFFFFFFu);
    EXPECT_EQ(read_le64(b.data()), 0xFFFFFFFFFFFFFFFFull);
}

// ===========================================================================
// Little-endian signed reads (two's-complement)
// ===========================================================================

TEST(SbeByteOrder, read_le_signed_negative_one) {
    std::array<uint8_t, 8> b;
    b.fill(0xFF); // all-ones == -1 in two's complement at every width
    EXPECT_EQ(read_le_i8(b.data()), int8_t{-1});
    EXPECT_EQ(read_le_i16(b.data()), int16_t{-1});
    EXPECT_EQ(read_le_i32(b.data()), int32_t{-1});
    EXPECT_EQ(read_le_i64(b.data()), int64_t{-1});
}

TEST(SbeByteOrder, read_le_i64_int64_min_null_sentinel) {
    // INT64_MIN = 0x8000000000000000, little-endian => trailing 0x80.
    std::array<uint8_t, 8> b{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
    EXPECT_EQ(read_le_i64(b.data()), std::numeric_limits<int64_t>::min());
}

TEST(SbeByteOrder, read_le_i8_positive_and_negative) {
    uint8_t pos = 0x7F; // 127
    uint8_t neg = 0x80; // -128
    EXPECT_EQ(read_le_i8(&pos), int8_t{127});
    EXPECT_EQ(read_le_i8(&neg), int8_t{-128});
}

// ===========================================================================
// varString8
// ===========================================================================

static std::vector<uint8_t> make_var_string(std::string_view s) {
    std::vector<uint8_t> v;
    v.push_back(static_cast<uint8_t>(s.size()));
    v.insert(v.end(), s.begin(), s.end());
    return v;
}

TEST(SbeVarString, reads_value_and_advance) {
    auto buf = make_var_string("BTCUSDT");
    auto r = read_var_string8(buf.data(), buf.size());
    ASSERT_TRUE(r.has_value()) << parse_error_name(r.error());
    EXPECT_EQ(r->value, "BTCUSDT");
    EXPECT_EQ(r->advance, 1u + 7u);
}

TEST(SbeVarString, empty_string_is_valid) {
    std::array<uint8_t, 1> buf{0x00};
    auto r = read_var_string8(buf.data(), buf.size());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->value.empty());
    EXPECT_EQ(r->advance, 1u);
}

TEST(SbeVarString, truncated_length_prefix_returns_truncated) {
    auto r = read_var_string8(nullptr, 0);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ParseError::kTruncated);
}

TEST(SbeVarString, declared_length_overruns_buffer_returns_truncated) {
    // Length prefix says 10 bytes of data, but only 3 are available.
    std::array<uint8_t, 4> buf{0x0A, 'A', 'B', 'C'};
    auto r = read_var_string8(buf.data(), buf.size());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), ParseError::kTruncated);
}

// ===========================================================================
// decode_decimal — value = mantissa × 10^exponent
// ===========================================================================

TEST(SbeDecimal, negative_exponent_scales_down) {
    // 6543210000 × 10^-8 = 65.4321
    EXPECT_DOUBLE_EQ(decode_decimal(6543210000LL, -8), 65.4321);
}

TEST(SbeDecimal, zero_exponent_is_identity) {
    EXPECT_DOUBLE_EQ(decode_decimal(42, 0), 42.0);
}

TEST(SbeDecimal, positive_exponent_scales_up) {
    EXPECT_DOUBLE_EQ(decode_decimal(5, 3), 5000.0);
}

TEST(SbeDecimal, negative_mantissa) {
    EXPECT_DOUBLE_EQ(decode_decimal(-12345, -2), -123.45);
}

// ===========================================================================
// read_group_header — groupSize16Encoding (blockLength u16 + numInGroup u16)
// ===========================================================================

TEST(SbeGroupHeader, decodes_block_length_and_count) {
    // blockLength=34, numInGroup=3, little-endian.
    std::array<uint8_t, kGroupHeaderSize> b{0x22, 0x00, 0x03, 0x00};
    auto gh = read_group_header(b.data());
    EXPECT_EQ(gh.block_length, 34);
    EXPECT_EQ(gh.num_in_group, 3);
}
