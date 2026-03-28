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
