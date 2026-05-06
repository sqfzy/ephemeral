/// @file tests/unit/bench/test_json_scan.cpp
/// Unit tests for `bench::scan_json_uint_field`.
///
/// The scanner is NOT a full JSON parser — it matches the exact shape
/// Python mocks emit (`json.dumps` on a dict of numeric fields). These
/// tests cover:
///   - happy paths (field at start/middle/end of buffer)
///   - whitespace tolerance between `":"` and the first digit
///   - false-positive guards (`"XT":` must not match `"T":`)
///   - pathological inputs (empty buffer, missing field, string value,
///     empty value, overflow)

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "core/json_scan.hpp"

using bench::scan_json_uint_field;

namespace {

// Helper: make scan call ergonomic from a string literal.
inline std::optional<uint64_t> scan(std::string_view s,
                                    std::string_view field) {
    return scan_json_uint_field(
        reinterpret_cast<const uint8_t*>(s.data()), s.size(), field);
}

} // namespace

// ── happy paths ─────────────────────────────────────────────────────────

TEST(JsonScan, SimpleNumericField) {
    auto v = scan(R"({"T":123})", "T");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 123ull);
}

TEST(JsonScan, FieldInMiddleOfObject) {
    auto v = scan(R"({"a":1,"T":456,"b":2})", "T");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 456ull);
}

TEST(JsonScan, FieldAtEndOfObject) {
    auto v = scan(R"({"x":1,"T":99})", "T");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 99ull);
}

TEST(JsonScan, FieldAtStartOfBuffer) {
    // Exercise the path where i == 0 on first iteration.
    auto v = scan(R"("T":1,"x":2)", "T");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 1ull);
}

TEST(JsonScan, LongTimestampValue) {
    // Typical real-world `T` in ns (~19 digits).
    auto v = scan(R"({"T":1712345678901234567})", "T");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 1712345678901234567ull);
}

TEST(JsonScan, MultiCharFieldName) {
    auto v = scan(R"({"ts_ns":42})", "ts_ns");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42ull);
}

// ── whitespace tolerance ────────────────────────────────────────────────

TEST(JsonScan, WhitespaceAfterColonSpace) {
    auto v = scan(R"({"T":    789})", "T");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 789ull);
}

TEST(JsonScan, WhitespaceAfterColonTab) {
    auto v = scan("{\"T\":\t\t42}", "T");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42ull);
}

// ── false-positive guards ───────────────────────────────────────────────

TEST(JsonScan, PartialMatchInsideLongerKey) {
    // `"XT":5` must NOT satisfy a lookup for "T" — the scanner requires
    // the opening quote to be part of the match.
    auto v = scan(R"({"XT":5,"other":1})", "T");
    EXPECT_FALSE(v.has_value());
}

TEST(JsonScan, ExactKeyMatchNotPrefix) {
    // Searching for "lat_tcp" should not match "lat_tcp_variant".
    auto v = scan(R"({"lat_tcp_variant":100})", "lat_tcp");
    EXPECT_FALSE(v.has_value());
}

// ── missing / malformed fields ──────────────────────────────────────────

TEST(JsonScan, MissingField) {
    auto v = scan(R"({"a":1,"b":2})", "T");
    EXPECT_FALSE(v.has_value());
}

TEST(JsonScan, EmptyBuffer) {
    auto v = scan_json_uint_field(nullptr, 0, "T");
    EXPECT_FALSE(v.has_value());
}

TEST(JsonScan, ZeroLengthWithValidPointer) {
    const uint8_t dummy = 'x';
    auto v = scan_json_uint_field(&dummy, 0, "T");
    EXPECT_FALSE(v.has_value());
}

TEST(JsonScan, StringValueRejected) {
    // `"T":"abc"` — value is a quoted string, not a digit sequence.
    auto v = scan(R"({"T":"abc"})", "T");
    EXPECT_FALSE(v.has_value());
}

TEST(JsonScan, EmptyValueBeforeBrace) {
    auto v = scan(R"({"T":})", "T");
    EXPECT_FALSE(v.has_value());
}

TEST(JsonScan, OverflowRejected) {
    // 20+ digits overflows u64 (max is 18446744073709551615).
    auto v = scan(R"({"T":99999999999999999999})", "T");
    EXPECT_FALSE(v.has_value());
}

TEST(JsonScan, Uint64MaxBoundary) {
    // Exactly UINT64_MAX should parse.
    auto v = scan(R"({"T":18446744073709551615})", "T");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, std::numeric_limits<uint64_t>::max());
}

TEST(JsonScan, ZeroValue) {
    auto v = scan(R"({"T":0})", "T");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 0ull);
}

// scan_json_uint_field's pre-checks reject empty field_name and
// oversized field_name (> 253 chars to fit "<F>": in the 256-byte
// stack needle). Both are uncovered guards — pin them so a future
// change to the needle buffer size surfaces here.
TEST(JsonScan, EmptyFieldNameRejected) {
    // The pre-check at "field_name.empty()" returns nullopt directly.
    auto v = scan(R"({"":42,"T":99})", "");
    EXPECT_FALSE(v.has_value())
        << "empty field_name must reject — without the guard the loop "
           "would search for a degenerate 3-byte needle '\":' in the "
           "buffer and could match arbitrary content";
}

TEST(JsonScan, FieldNameTooLongForStackNeedleRejected) {
    // 254-byte field name pushes needle_len=257 > sizeof(needle)=256.
    // The function rejects rather than silently truncating.
    std::string long_name(254, 'k');
    auto v = scan(R"({"foo":1,"T":42})", long_name);
    EXPECT_FALSE(v.has_value());
    // Boundary: 253 bytes is the maximum that fits (needle_len=256).
    // We don't insist this *succeeds* (it just won't match the test
    // payload's "T" field) — only that it doesn't crash or assert.
    std::string max_name(253, 'k');
    auto v2 = scan(R"({"foo":1,"T":42})", max_name);
    EXPECT_FALSE(v2.has_value());  // field "kkk...kkk" not in payload
}
