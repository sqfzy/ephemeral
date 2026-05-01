/// @file test_json.cpp
/// Unit tests for the JSON parser and JsonFramer.

#include <cstdint>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/json/framer.hpp"
#include "eph/json/parser.hpp"

using namespace eph::json;

// ---------------------------------------------------------------------------
// Helper: parse a string literal
// ---------------------------------------------------------------------------
static std::expected<JsonView, ParseError> parse_str(std::string_view sv) {
    return parse(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

// ---------------------------------------------------------------------------
// Happy path: real Binance bookTicker message
// ---------------------------------------------------------------------------

static constexpr std::string_view kBinanceBookTicker =
    R"({"e":"bookTicker","u":4736462646,"s":"BTCUSDT","b":"87245.30","B":"0.500","a":"87245.40","A":"1.200","T":1711612345678,"E":1711612345679})";

TEST(JsonParser, BinanceBookTicker) {
    auto result = parse_str(kBinanceBookTicker);
    ASSERT_TRUE(result.has_value()) << parse_error_name(result.error());

    auto& v = *result;
    EXPECT_EQ(v.field_count(), 9u);
    EXPECT_EQ(v.get_string("e"), "bookTicker");
    EXPECT_EQ(v.get_string("s"), "BTCUSDT");
    EXPECT_EQ(v.get_string("b"), "87245.30");
    EXPECT_EQ(v.get_string("B"), "0.500");
    EXPECT_EQ(v.get_string("a"), "87245.40");
    EXPECT_EQ(v.get_string("A"), "1.200");
    EXPECT_EQ(v.get_int("u"), 4736462646);
    EXPECT_EQ(v.get_int("T"), 1711612345678);
    EXPECT_EQ(v.get_int("E"), 1711612345679);
}

// ---------------------------------------------------------------------------
// Happy path: Binance combined stream wrapper
// ---------------------------------------------------------------------------

static constexpr std::string_view kBinanceCombined =
    R"({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker","u":123,"s":"BTCUSDT","b":"87000.00","B":"1.0","a":"87001.00","A":"2.0","T":999,"E":1000}})";

TEST(JsonParser, BinanceCombinedStream) {
    auto result = parse_str(kBinanceCombined);
    ASSERT_TRUE(result.has_value());

    auto& v = *result;
    // "stream" is a string, "data" is a nested object (opaque)
    EXPECT_EQ(v.get_string("stream"), "btcusdt@bookTicker");
    EXPECT_TRUE(v.has("data"));
    // The nested object is captured as opaque value
    auto data_val = v.get("data");
    EXPECT_TRUE(data_val.starts_with("{"));
    EXPECT_TRUE(data_val.ends_with("}"));
}

// ---------------------------------------------------------------------------
// Happy path: OKX ticker
// ---------------------------------------------------------------------------

static constexpr std::string_view kOkxTicker =
    R"({"instId":"BTC-USDT","last":"87200.5","lastSz":"0.001","askPx":"87201.0","askSz":"0.5","bidPx":"87200.0","bidSz":"1.2","ts":"1711612345678"})";

TEST(JsonParser, OkxTicker) {
    auto result = parse_str(kOkxTicker);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->get_string("instId"), "BTC-USDT");
    EXPECT_EQ(result->get_string("last"), "87200.5");
    EXPECT_EQ(result->get_string("ts"), "1711612345678");
    EXPECT_EQ(result->field_count(), 8u);
}

// ---------------------------------------------------------------------------
// Value type parsing
// ---------------------------------------------------------------------------

TEST(JsonParser, IntegerValues) {
    auto result = parse_str(R"({"a":42,"b":-7,"c":0,"d":9223372036854775807})");
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->get_int("a"), 42);
    EXPECT_EQ(result->get_int("b"), -7);
    EXPECT_EQ(result->get_int("c"), 0);
    EXPECT_EQ(result->get_int("d"), INT64_MAX);
}

TEST(JsonParser, DoubleValues) {
    auto result = parse_str(R"({"price":"87245.30","qty":1.5,"neg":-0.001})");
    ASSERT_TRUE(result.has_value());

    // Quoted number (string) — get_double parses the string content
    EXPECT_EQ(result->get_string("price"), "87245.30");
    EXPECT_DOUBLE_EQ(*result->get_double("qty"), 1.5);
    EXPECT_DOUBLE_EQ(*result->get_double("neg"), -0.001);
}

TEST(JsonParser, BooleanValues) {
    auto result = parse_str(R"({"active":true,"deleted":false})");
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->get_bool("active"), true);
    EXPECT_EQ(result->get_bool("deleted"), false);
}

TEST(JsonParser, NullValue) {
    auto result = parse_str(R"({"x":null})");
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->get("x"), "null");
    EXPECT_FALSE(result->get_int("x").has_value());
    EXPECT_FALSE(result->get_bool("x").has_value());
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(JsonParser, EmptyObject) {
    auto result = parse_str(R"({})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->field_count(), 0u);
}

TEST(JsonParser, WhitespaceAroundTokens) {
    auto result = parse_str(R"({  "a"  :  "b"  ,  "c"  :  42  })");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_string("a"), "b");
    EXPECT_EQ(result->get_int("c"), 42);
}

TEST(JsonParser, EscapedQuotesInString) {
    auto result = parse_str(R"({"msg":"hello \"world\""})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_string("msg"), R"(hello \"world\")");
}

TEST(JsonParser, NestedObjectSkipped) {
    auto result = parse_str(R"({"a":1,"nested":{"x":1,"y":2},"b":2})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_int("a"), 1);
    EXPECT_EQ(result->get_int("b"), 2);
    EXPECT_EQ(result->field_count(), 3u);
}

TEST(JsonParser, NestedArraySkipped) {
    auto result = parse_str(R"({"a":1,"arr":[1,2,3],"b":2})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_int("a"), 1);
    EXPECT_EQ(result->get_int("b"), 2);
}

TEST(JsonParser, MissingKeyNotFound) {
    auto result = parse_str(R"({"a":1})");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->get("missing").empty());
    EXPECT_FALSE(result->get_int("missing").has_value());
    EXPECT_FALSE(result->has("missing"));
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

TEST(JsonParser, EmptyInput) {
    auto result = parse(nullptr, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kIncomplete);
}

TEST(JsonParser, MissingOpenBrace) {
    auto result = parse_str(R"("not an object")");
    EXPECT_EQ(result.error(), ParseError::kInvalidFormat);
}

TEST(JsonParser, MissingCloseBrace) {
    auto result = parse_str(R"({"a":1)");
    EXPECT_EQ(result.error(), ParseError::kIncomplete);
}

TEST(JsonParser, MissingColon) {
    auto result = parse_str(R"({"a" 1})");
    EXPECT_EQ(result.error(), ParseError::kInvalidFormat);
}

TEST(JsonParser, UnquotedKey) {
    auto result = parse_str(R"({a:1})");
    EXPECT_EQ(result.error(), ParseError::kInvalidFormat);
}

TEST(JsonParser, FieldOverflow) {
    // Build a JSON with > kMaxFields fields
    std::string json = "{";
    for (size_t i = 0; i <= JsonView::kMaxFields; ++i) {
        if (i > 0) json += ",";
        json += "\"f" + std::to_string(i) + "\":" + std::to_string(i);
    }
    json += "}";

    auto result = parse(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    EXPECT_EQ(result.error(), ParseError::kFieldOverflow);
}

// ---------------------------------------------------------------------------
// JsonFramer concept satisfaction
// ---------------------------------------------------------------------------

TEST(JsonFramer, SatisfiesMessageFramerConcept) {
    static_assert(eph::net::MessageFramer<eph::json::JsonFramer>);
}

TEST(JsonFramer, DecodePassthrough) {
    const uint8_t data[] = R"({"a":1})";
    JsonFramer framer;
    auto result = framer.decode(data, sizeof(data) - 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload, data);
    EXPECT_EQ(result->payload_len, sizeof(data) - 1);
}

TEST(JsonFramer, DecodeEmpty) {
    JsonFramer framer;
    auto result = framer.decode(nullptr, 0);
    EXPECT_EQ(result.error(), eph::net::FrameError::kIncomplete);
}

TEST(JsonFramer, EncodePassthrough) {
    const uint8_t data[] = "hello";
    uint8_t out[16]{};
    JsonFramer framer;
    size_t written = framer.encode(out, data, 5, 0);
    EXPECT_EQ(written, 5u);
    EXPECT_EQ(std::memcmp(out, data, 5), 0);
}

// ---------------------------------------------------------------------------
// Integer edge cases (INT64_MIN, overflow)
// ---------------------------------------------------------------------------

TEST(JsonParser, IntMinValue) {
    // INT64_MIN = -9223372036854775808
    auto result = parse_str(R"({"v":-9223372036854775808})");
    ASSERT_TRUE(result.has_value());
    auto val = result->get_int("v");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, INT64_MIN);
}

TEST(JsonParser, IntMaxValue) {
    auto result = parse_str(R"({"v":9223372036854775807})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_int("v"), INT64_MAX);
}

TEST(JsonParser, IntOverflowReturnsNullopt) {
    // INT64_MAX + 1
    auto result = parse_str(R"({"v":9223372036854775808})");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_int("v").has_value());
}

// ---------------------------------------------------------------------------
// Double edge cases
// ---------------------------------------------------------------------------

TEST(JsonParser, DoubleScientificNotation) {
    auto result = parse_str(R"({"a":1.5e3,"b":2E-2,"c":3.14e+0})");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result->get_double("a"), 1500.0);
    EXPECT_DOUBLE_EQ(*result->get_double("b"), 0.02);
    EXPECT_DOUBLE_EQ(*result->get_double("c"), 3.14);
}

// ---------------------------------------------------------------------------
// field_at bounds check
// ---------------------------------------------------------------------------

TEST(JsonParser, FieldAtOutOfBounds) {
    auto result = parse_str(R"({"a":1})");
    ASSERT_TRUE(result.has_value());
    // In-bounds
    EXPECT_EQ(result->field_at(0).key, "a");
    // Out of bounds returns empty field
    EXPECT_TRUE(result->field_at(99).key.empty());
}

// ---------------------------------------------------------------------------
// ParseError formatting
// ---------------------------------------------------------------------------

TEST(JsonParseError, ErrorNames) {
    EXPECT_EQ(parse_error_name(ParseError::kIncomplete), "incomplete");
    EXPECT_EQ(parse_error_name(ParseError::kInvalidFormat), "invalid format");
    EXPECT_EQ(parse_error_name(ParseError::kFieldOverflow), "field overflow");
}

TEST(JsonParseError, FormatWorks) {
    auto s = std::format("{}", ParseError::kInvalidFormat);
    EXPECT_EQ(s, "invalid format");
}

// ---------------------------------------------------------------------------
// Number parsing RFC compliance: reject malformed numeric literals
// ---------------------------------------------------------------------------

TEST(JsonParser, RejectBareDecimalPoint) {
    // "." alone is not a valid number — must have digits.
    auto result = parse_str(R"({"v":.})");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_double("v").has_value());
}

TEST(JsonParser, RejectTrailingDot) {
    // "1." is not RFC-compliant — fraction part requires at least one digit.
    auto result = parse_str(R"({"v":1.})");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_double("v").has_value());
}

TEST(JsonParser, RejectBareExponent) {
    // "1e" has no exponent digits — must be rejected.
    auto result = parse_str(R"({"v":1e})");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_double("v").has_value());
}

TEST(JsonParser, RejectExponentWithSignOnly) {
    // "1e+" has a sign but no exponent digits — must be rejected.
    auto result = parse_str(R"({"v":1e+})");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_double("v").has_value());
}

TEST(JsonParser, RejectExponentWithMinusSignOnly) {
    // "1e-" has a sign but no exponent digits — must be rejected.
    auto result = parse_str(R"({"v":1e-})");
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_double("v").has_value());
}

TEST(JsonParser, ValidDecimalsStillWork) {
    auto result = parse_str(R"({"a":1.0,"b":0.5,"c":-3.14,"d":1e2,"e":2.5e-1})");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(*result->get_double("a"), 1.0);
    EXPECT_DOUBLE_EQ(*result->get_double("b"), 0.5);
    EXPECT_DOUBLE_EQ(*result->get_double("c"), -3.14);
    EXPECT_DOUBLE_EQ(*result->get_double("d"), 100.0);
    EXPECT_DOUBLE_EQ(*result->get_double("e"), 0.25);
}

// ---------------------------------------------------------------------------
// Nested-skip boundary cases — the opaque-skip path is an attack surface
// (depth counter + inner string scan). These cover paths that the existing
// NestedObjectSkipped / NestedArraySkipped happy cases don't reach.
// ---------------------------------------------------------------------------

TEST(JsonParser, NestedObjectUnterminatedReturnsIncomplete) {
    // `{"a":{"x":1` — opening brace never balanced; depth-counter loop must
    // reach end-of-buffer with depth>0 and surface kIncomplete (not crash).
    auto result = parse_str(R"({"a":{"x":1)");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kIncomplete);
}

TEST(JsonParser, NestedArrayUnterminatedReturnsIncomplete) {
    // Same as above but for `[`. Different `close` byte path through
    // the depth counter.
    auto result = parse_str(R"({"a":[1,2,3)");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kIncomplete);
}

TEST(JsonParser, NestedStringUnterminatedReturnsIncomplete) {
    // Inside a nested object, an unterminated string should not let the
    // outer brace counter drift — the inner string-skip loop runs to end
    // and the depth check trips kIncomplete.
    auto result = parse_str(R"({"a":{"x":"oops)");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kIncomplete);
}

TEST(JsonParser, DeeplyNestedObjectExceedsLimit) {
    // kMaxNestingDepth is 64. Build > 64 opening braces to exercise the
    // depth-cap branch (line 375). Must surface kInvalidFormat — never crash
    // and never silently truncate.
    std::string json = R"({"x":)";
    for (int i = 0; i < 70; ++i) json += '{';
    for (int i = 0; i < 70; ++i) json += '}';
    json += '}';
    auto result = parse(reinterpret_cast<const uint8_t*>(json.data()), json.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kInvalidFormat);
}

// ---------------------------------------------------------------------------
// Format-error boundary cases — paths reached only by a second-or-later field
// ---------------------------------------------------------------------------

TEST(JsonParser, SecondKeyMissingOpeningQuote) {
    // After the first field's comma, the next key must be quoted. An
    // unquoted second key exercises the `*p != '"'` branch on the
    // post-comma path (distinct from the first-key UnquotedKey case).
    auto result = parse_str(R"({"a":1,b:2})");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kInvalidFormat);
}

TEST(JsonParser, MissingCommaBetweenFieldsRejected) {
    // Two key-value pairs with no comma between them — the parser expects
    // either `,` or `}` after a value and must reject this with InvalidFormat.
    auto result = parse_str(R"({"a":1 "b":2})");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kInvalidFormat);
}
