#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "eph/fix.hpp"

using namespace eph::fix;

// ---------------------------------------------------------------------------
// Helper: build a raw FIX message string with correct BodyLength and CheckSum
// ---------------------------------------------------------------------------
static std::vector<uint8_t> make_fix_msg(std::string_view begin_string,
                                          std::string_view body) {
    // Header: "8=FIX.4.2\x01" "9=NNN\x01"
    std::string header = "8=";
    header += begin_string;
    header += '\x01';

    std::string body_len_field = "9=" + std::to_string(body.size()) + '\x01';
    std::string full = header + body_len_field + std::string(body);

    // Compute checksum
    uint32_t sum = 0;
    for (char c : full) sum += static_cast<uint8_t>(c);
    uint8_t cs = static_cast<uint8_t>(sum & 0xFF);

    char cs_str[8];
    std::snprintf(cs_str, sizeof(cs_str), "10=%03u\x01", cs);
    full += cs_str;

    return {full.begin(), full.end()};
}

// ===========================================================================
// Tags
// ===========================================================================

TEST(FixTags, tag_name_known) {
    EXPECT_EQ(tag::tag_name(tag::BeginString), "BeginString");
    EXPECT_EQ(tag::tag_name(tag::MsgType), "MsgType");
    EXPECT_EQ(tag::tag_name(tag::Symbol), "Symbol");
    EXPECT_EQ(tag::tag_name(tag::Price), "Price");
    EXPECT_EQ(tag::tag_name(tag::MDReqID), "MDReqID");
}

TEST(FixTags, tag_name_unknown) {
    EXPECT_EQ(tag::tag_name(99999), "Unknown");
}

TEST(FixTags, msg_type_name_known) {
    EXPECT_EQ(tag::msg_type_name('D'), "NewOrderSingle");
    EXPECT_EQ(tag::msg_type_name('8'), "ExecutionReport");
    EXPECT_EQ(tag::msg_type_name('A'), "Logon");
    EXPECT_EQ(tag::msg_type_name('5'), "Logout");
    EXPECT_EQ(tag::msg_type_name('0'), "Heartbeat");
    EXPECT_EQ(tag::msg_type_name('V'), "MarketDataRequest");
    EXPECT_EQ(tag::msg_type_name('W'), "MarketDataSnapshot");
    EXPECT_EQ(tag::msg_type_name('X'), "MarketDataIncRefresh");
}

TEST(FixTags, msg_type_name_unknown) {
    EXPECT_EQ(tag::msg_type_name('~'), "Unknown");
}

TEST(FixTags, msg_type_name_new_single_char) {
    EXPECT_EQ(tag::msg_type_name('d'), "SecurityDefinition");
    EXPECT_EQ(tag::msg_type_name('f'), "SecurityStatus");
    EXPECT_EQ(tag::msg_type_name('i'), "MassQuote");
    EXPECT_EQ(tag::msg_type_name('Z'), "QuoteCancel");
    EXPECT_EQ(tag::msg_type_name('y'), "SecurityList");
    EXPECT_EQ(tag::msg_type_name('x'), "SecurityListRequest");
}

TEST(FixTags, msg_type_name_multi_char) {
    using namespace std::string_view_literals;
    EXPECT_EQ(tag::msg_type_name("AE"sv), "TradeCaptureReport");
    EXPECT_EQ(tag::msg_type_name("AR"sv), "TradeCaptureReportAck");
    EXPECT_EQ(tag::msg_type_name("AP"sv), "PositionReport");
    EXPECT_EQ(tag::msg_type_name("ZZ"sv), "Unknown");
    // Single-char via string_view overload
    EXPECT_EQ(tag::msg_type_name("D"sv), "NewOrderSingle");
}

// ===========================================================================
// Parser
// ===========================================================================

TEST(FixParser, parse_valid_new_order_single) {
    std::string body =
        "35=D\x01"
        "49=SENDER\x01"
        "56=TARGET\x01"
        "34=1\x01"
        "52=20260322-12:00:00\x01"
        "11=order1\x01"
        "55=AAPL\x01"
        "54=1\x01"
        "38=100\x01"
        "40=2\x01"
        "44=150.50\x01";

    auto raw = make_fix_msg("FIX.4.2", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value()) << "parse failed: " << parse_error_name(result.error());

    auto& msg = *result;
    EXPECT_EQ(msg.msg_type().value(), "D");
    EXPECT_EQ(msg.get(tag::SenderCompID).value(), "SENDER");
    EXPECT_EQ(msg.get(tag::TargetCompID).value(), "TARGET");
    EXPECT_EQ(msg.get(tag::Symbol).value(), "AAPL");
    EXPECT_EQ(msg.get_int(tag::Side).value(), 1);
    EXPECT_EQ(msg.get_int(tag::OrderQty).value(), 100);
    EXPECT_DOUBLE_EQ(msg.get_double(tag::Price).value(), 150.50);
    EXPECT_EQ(msg.total_len(), raw.size());
}

TEST(FixParser, parse_empty_returns_incomplete) {
    auto result = parse(nullptr, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kIncomplete);
}

TEST(FixParser, parse_truncated_returns_incomplete) {
    std::string partial = "8=FIX.4.2\x01" "9=5\x01" "35=D\x01";
    auto result = parse(reinterpret_cast<const uint8_t*>(partial.data()), partial.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kIncomplete);
}

TEST(FixParser, parse_bad_start_returns_invalid_format) {
    std::string bad = "35=D\x01" "10=000\x01";
    auto result = parse(reinterpret_cast<const uint8_t*>(bad.data()), bad.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kInvalidFormat);
}

TEST(FixParser, parse_checksum_mismatch) {
    std::string body = "35=D\x01";
    auto raw = make_fix_msg("FIX.4.2", body);
    // Corrupt the checksum
    raw[raw.size() - 2] = '0'; // change last digit
    auto result = parse(raw.data(), raw.size());
    // May be mismatch or invalid format depending on which digit changed
    ASSERT_FALSE(result.has_value());
}

TEST(FixParser, get_int_negative) {
    std::string body = "35=8\x01" "58=-42\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_int(tag::Text).value(), -42);
}

TEST(FixParser, get_missing_tag_returns_nullopt) {
    std::string body = "35=D\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get(tag::Symbol).has_value());
    EXPECT_FALSE(result->get_int(tag::Price).has_value());
    EXPECT_FALSE(result->get_double(tag::Price).has_value());
}

TEST(FixParser, for_each_iterates_all_fields) {
    std::string body = "35=D\x01" "55=MSFT\x01" "54=2\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    size_t count = 0;
    result->for_each([&](uint32_t, std::string_view) { ++count; });
    EXPECT_EQ(count, result->field_count());
    EXPECT_EQ(count, 3u);
}

TEST(FixParser, parse_field_overflow_returns_error) {
    // Build a body with more than kMaxFields (128) fields
    std::string body;
    body += "35=D\x01";
    for (int i = 0; i < 128; ++i) {
        // Use tags 5000+ to avoid collisions with standard tags
        body += std::to_string(5000 + i) + "=val" + std::to_string(i) + '\x01';
    }
    // That's 129 fields total (35=D plus 128 extra), which exceeds kMaxFields=128
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kFieldOverflow);
}

TEST(FixParser, verify_checksum_valid) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    EXPECT_TRUE(verify_checksum(raw.data(), raw.size()));
}

TEST(FixParser, verify_checksum_invalid) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    // Corrupt a body byte
    raw[5] = 'Z';
    EXPECT_FALSE(verify_checksum(raw.data(), raw.size()));
}

TEST(FixParser, get_int_non_numeric_returns_nullopt) {
    std::string body = "35=D\x01" "58=abc\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    // Text tag contains "abc" — get_int should return nullopt
    EXPECT_FALSE(result->get_int(tag::Text).has_value());
}

TEST(FixParser, get_double_non_numeric_returns_nullopt) {
    std::string body = "35=D\x01" "58=not.a.number\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_double(tag::Text).has_value());
}

TEST(FixParser, get_int_empty_value_returns_nullopt) {
    std::string body = "35=D\x01" "58=\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_int(tag::Text).has_value());
}

TEST(FixParser, exact_max_fields_boundary) {
    // Build a body with exactly kMaxFields (128) fields
    std::string body;
    for (int i = 0; i < 128; ++i) {
        body += std::to_string(5000 + i) + "=v\x01";
    }
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    // Should succeed — exactly at the limit
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->field_count(), 128u);
}

TEST(FixParser, verify_checksum_too_short) {
    uint8_t tiny[] = {0x31}; // just 1 byte
    EXPECT_FALSE(verify_checksum(tiny, sizeof(tiny)));
}

TEST(FixParser, parse_multiple_messages_consumes_first_only) {
    std::string body1 = "35=D\x01";
    std::string body2 = "35=8\x01";
    auto raw1 = make_fix_msg("FIX.4.4", body1);
    auto raw2 = make_fix_msg("FIX.4.4", body2);

    // Concatenate two messages
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), raw1.begin(), raw1.end());
    combined.insert(combined.end(), raw2.begin(), raw2.end());

    auto result = parse(combined.data(), combined.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type().value(), "D");
    EXPECT_EQ(result->total_len(), raw1.size()); // only first consumed
}

// ===========================================================================
// Builder
// ===========================================================================

TEST(FixBuilder, build_and_parse_roundtrip) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set(tag::SenderCompID, "SENDER");
    b.set(tag::TargetCompID, "TARGET");
    b.set_int(tag::MsgSeqNum, 42);
    b.set(tag::SendingTime, "20260322-12:00:00");
    b.set(tag::ClOrdID, "order123");
    b.set(tag::Symbol, "TSLA");
    b.set_int(tag::Side, 1);
    b.set_int(tag::OrderQty, 500);
    b.set_int(tag::OrdType, 2);
    b.set_double(tag::Price, 199.99);

    size_t len = b.finish("FIX.4.4");
    ASSERT_GT(len, 0u) << "builder overflow";
    EXPECT_EQ(b.size(), len);

    // Verify checksum
    EXPECT_TRUE(verify_checksum(b.data(), b.size()));

    // Parse the built message
    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value()) << parse_error_name(result.error());

    EXPECT_EQ(result->msg_type().value(), "D");
    EXPECT_EQ(result->get(tag::SenderCompID).value(), "SENDER");
    EXPECT_EQ(result->get(tag::Symbol).value(), "TSLA");
    EXPECT_EQ(result->get_int(tag::MsgSeqNum).value(), 42);
    EXPECT_EQ(result->get_int(tag::OrderQty).value(), 500);
    EXPECT_DOUBLE_EQ(result->get_double(tag::Price).value(), 199.99);
}

TEST(FixBuilder, buffer_too_small_returns_zero) {
    uint8_t buf[16]; // way too small
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set(tag::SenderCompID, "VERY_LONG_SENDER_ID_THAT_OVERFLOWS");
    size_t len = b.finish();
    EXPECT_EQ(len, 0u);
}

TEST(FixBuilder, set_double_precision) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_double(tag::Price, 123.456789, 4);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());

    // Should have 4 decimal places
    auto price_str = result->get(tag::Price).value();
    EXPECT_NE(price_str.find('.'), std::string_view::npos);
    auto dot_pos = price_str.find('.');
    EXPECT_EQ(price_str.size() - dot_pos - 1, 4u);
}

TEST(FixBuilder, negative_integer) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "8");
    b.set_int(tag::Text, -999);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_int(tag::Text).value(), -999);
}

TEST(FixBuilder, int64_min_no_ub) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "8");
    b.set_int(tag::Text, INT64_MIN);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value()) << parse_error_name(result.error());

    auto val = result->get(tag::Text);
    ASSERT_TRUE(val.has_value());
    // INT64_MIN = -9223372036854775808
    EXPECT_EQ(val.value(), "-9223372036854775808");
}

TEST(FixBuilder, zero_integer) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_int(tag::OrderQty, 0);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_int(tag::OrderQty).value(), 0);
}

TEST(FixBuilder, zero_double) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_double(tag::Price, 0.0);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->get_double(tag::Price).value(), 0.0);
}

TEST(FixBuilder, negative_double) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_double(tag::Price, -42.75);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->get_double(tag::Price).value(), -42.75);
}

TEST(FixBuilder, empty_string_field) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set(tag::Text, "");

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    auto val = result->get(tag::Text);
    ASSERT_TRUE(val.has_value());
    EXPECT_TRUE(val->empty());
}

TEST(FixBuilder, finish_with_exact_capacity) {
    // Build a minimal message and verify it works with a tight buffer
    uint8_t buf[128];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    size_t len = b.finish();
    ASSERT_GT(len, 0u);
    EXPECT_TRUE(verify_checksum(b.data(), b.size()));
}

TEST(FixBuilder, reset_allows_reuse) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D").set(tag::Symbol, "AAPL");
    size_t len1 = b.finish();
    ASSERT_GT(len1, 0u);

    // Reset and build a different message
    b.reset();
    b.set(tag::MsgType, "8").set(tag::Symbol, "TSLA").set_int(tag::OrderQty, 200);
    size_t len2 = b.finish();
    ASSERT_GT(len2, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type().value(), "8");
    EXPECT_EQ(result->get(tag::Symbol).value(), "TSLA");
    EXPECT_EQ(result->get_int(tag::OrderQty).value(), 200);
}

TEST(FixParser, get_char_single_char_field) {
    std::string body = "35=D\x01" "54=1\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_char(tag::Side).value(), '1');
}

TEST(FixParser, get_char_multi_char_returns_nullopt) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_char(tag::Symbol).has_value());
}

TEST(FixParser, get_char_empty_returns_nullopt) {
    std::string body = "35=D\x01" "58=\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_char(tag::Text).has_value());
}

TEST(FixParser, get_char_missing_tag_returns_nullopt) {
    std::string body = "35=D\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_char(tag::Side).has_value());
}

TEST(FixParser, get_int_bare_minus_returns_nullopt) {
    std::string body = "35=D\x01" "58=-\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_int(tag::Text).has_value());
}

TEST(FixParser, get_double_overflow_returns_nullopt) {
    // An extremely large number that overflows double to infinity
    std::string huge_num(310, '9'); // 10^310 > DBL_MAX
    std::string body = "35=D\x01" "58=" + huge_num + "\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_double(tag::Text).has_value());
}

TEST(FixParser, get_int_overflow_returns_nullopt) {
    // Value exceeding INT64_MAX should return nullopt, not silently wrap
    std::string body = "35=D\x01" "58=99999999999999999999\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_int(tag::Text).has_value());
}

TEST(FixParser, get_int_negative_overflow_returns_nullopt) {
    // Value below INT64_MIN should return nullopt
    std::string body = "35=D\x01" "58=-99999999999999999999\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_int(tag::Text).has_value());
}

TEST(FixParser, get_int_int64_max_exact) {
    // INT64_MAX should parse successfully
    std::string body = "35=D\x01" "58=9223372036854775807\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_int(tag::Text).value(), INT64_MAX);
}

TEST(FixParser, get_int_int64_min_exact) {
    // INT64_MIN should parse successfully
    std::string body = "35=D\x01" "58=-9223372036854775808\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_int(tag::Text).value(), INT64_MIN);
}

TEST(FixParser, get_int_one_past_int64_max_returns_nullopt) {
    // INT64_MAX + 1 should fail
    std::string body = "35=D\x01" "58=9223372036854775808\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_int(tag::Text).has_value());
}

TEST(FixBuilder, int64_max_roundtrip) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "8");
    b.set_int(tag::Text, INT64_MAX);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value()) << parse_error_name(result.error());

    auto val = result->get(tag::Text);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val.value(), "9223372036854775807");
}

TEST(FixBuilder, reset_after_overflow) {
    uint8_t buf[64]; // small buffer
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set(tag::SenderCompID, "VERY_LONG_VALUE_THAT_DEFINITELY_OVERFLOWS");
    EXPECT_EQ(b.finish(), 0u); // overflow

    // Reset should clear the overflow flag and allow reuse
    b.reset();
    b.set(tag::MsgType, "D");
    size_t len = b.finish();
    ASSERT_GT(len, 0u);
    EXPECT_TRUE(verify_checksum(b.data(), b.size()));
}

TEST(FixBuilder, chained_set_calls) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    // Verify fluent API (set returns *this)
    b.set(tag::MsgType, "D")
     .set(tag::Symbol, "AAPL")
     .set_int(tag::Side, 1)
     .set_double(tag::Price, 100.0);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get(tag::Symbol).value(), "AAPL");
}

// ===========================================================================
// Builder — comprehensive tests (FixBuilder suite)
// ===========================================================================

TEST(FixBuilder, BasicBuild) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set(tag::SenderCompID, "SENDER");
    b.set(tag::TargetCompID, "TARGET");
    b.set(tag::Symbol, "AAPL");
    b.set_int(tag::Side, 1);
    b.set_double(tag::Price, 150.50);
    b.set_int(tag::OrderQty, 100);

    size_t len = b.finish();
    ASSERT_GT(len, 0u) << "finish() should return non-zero for a valid message";
    EXPECT_EQ(b.size(), len);
}

TEST(FixBuilder, RoundTripParseBuiltMessage) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set(tag::SenderCompID, "CLIENT");
    b.set(tag::TargetCompID, "EXCHANGE");
    b.set(tag::Symbol, "MSFT");
    b.set_int(tag::Side, 2);
    b.set_double(tag::Price, 425.75);
    b.set_int(tag::OrderQty, 50);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value()) << parse_error_name(result.error());

    EXPECT_EQ(result->msg_type().value(), "D");
    EXPECT_EQ(result->get(tag::SenderCompID).value(), "CLIENT");
    EXPECT_EQ(result->get(tag::TargetCompID).value(), "EXCHANGE");
    EXPECT_EQ(result->get(tag::Symbol).value(), "MSFT");
    EXPECT_EQ(result->get_int(tag::Side).value(), 2);
    EXPECT_DOUBLE_EQ(result->get_double(tag::Price).value(), 425.75);
    EXPECT_EQ(result->get_int(tag::OrderQty).value(), 50);
}

TEST(FixBuilder, ResetAndReuse) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));

    // Build first message
    b.set(tag::MsgType, "D");
    b.set(tag::Symbol, "AAPL");
    b.set_int(tag::Side, 1);
    size_t len1 = b.finish();
    ASSERT_GT(len1, 0u);

    // Save first message for verification
    std::vector<uint8_t> msg1(b.data(), b.data() + b.size());

    // Reset and build second message
    b.reset();
    b.set(tag::MsgType, "8");
    b.set(tag::Symbol, "GOOG");
    b.set_int(tag::Side, 2);
    size_t len2 = b.finish();
    ASSERT_GT(len2, 0u);

    // Both messages should parse correctly
    auto r1 = parse(msg1.data(), msg1.size());
    ASSERT_TRUE(r1.has_value()) << parse_error_name(r1.error());
    EXPECT_EQ(r1->msg_type().value(), "D");
    EXPECT_EQ(r1->get(tag::Symbol).value(), "AAPL");

    auto r2 = parse(b.data(), b.size());
    ASSERT_TRUE(r2.has_value()) << parse_error_name(r2.error());
    EXPECT_EQ(r2->msg_type().value(), "8");
    EXPECT_EQ(r2->get(tag::Symbol).value(), "GOOG");
}

TEST(FixBuilder, BufferOverflowReturnsZero) {
    uint8_t buf[10]; // tiny buffer — cannot fit any valid FIX message
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    size_t len = b.finish();
    EXPECT_EQ(len, 0u) << "finish() should return 0 when buffer is too small";
}

TEST(FixBuilder, EmptyBodyProducesValidMessage) {
    // Build with no user fields — only BeginString, BodyLength, CheckSum
    uint8_t buf[128];
    MessageBuilder b(buf, sizeof(buf));
    size_t len = b.finish();
    ASSERT_GT(len, 0u) << "empty-body message should still produce valid output";
    EXPECT_TRUE(verify_checksum(b.data(), b.size()));

    // Should at least have BeginString (tag 8) present
    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value()) << parse_error_name(result.error());
}

TEST(FixBuilder, SetIntNegativeValue) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "8");
    b.set_int(tag::Text, -42);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value()) << parse_error_name(result.error());
    EXPECT_EQ(result->get_int(tag::Text).value(), -42);
}

TEST(FixBuilder, SetDoubleWithPrecision) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_double(tag::Price, 123.456789, 4);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value()) << parse_error_name(result.error());

    // With precision=4, 123.456789 rounds to 123.4568
    double parsed = result->get_double(tag::Price).value();
    EXPECT_NEAR(parsed, 123.4568, 0.00005);

    // Verify string representation has exactly 4 decimal places
    auto price_str = result->get(tag::Price).value();
    auto dot_pos = price_str.find('.');
    ASSERT_NE(dot_pos, std::string_view::npos);
    EXPECT_EQ(price_str.size() - dot_pos - 1, 4u);
}

TEST(FixBuilder, CustomBeginString) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set(tag::Symbol, "AAPL");

    size_t len = b.finish("FIX.4.2");
    ASSERT_GT(len, 0u);

    // Verify the message is valid and parseable
    EXPECT_TRUE(verify_checksum(b.data(), b.size()));
    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value()) << parse_error_name(result.error());

    // Verify the raw bytes start with "8=FIX.4.2\x01"
    // (BeginString is parsed but not stored as a regular field by the parser)
    std::string_view raw(reinterpret_cast<const char*>(b.data()), b.size());
    EXPECT_TRUE(raw.starts_with("8=FIX.4.2\x01"))
        << "message should start with custom BeginString";

    // Verify body fields are still intact
    EXPECT_EQ(result->msg_type().value(), "D");
    EXPECT_EQ(result->get(tag::Symbol).value(), "AAPL");
}

TEST(FixBuilder, DataAndSize) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set(tag::Symbol, "AAPL");

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    // data() should point to the buffer
    EXPECT_EQ(b.data(), buf);
    // size() should equal the return value of finish()
    EXPECT_EQ(b.size(), len);
}

TEST(FixBuilder, MultipleFieldsSameTag) {
    // Add the same tag twice via the builder (simulating repeating groups)
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "W");
    b.set_int(tag::NoMDEntries, 2);
    b.set(tag::MDEntryPx, "100.25");
    b.set(tag::MDEntryPx, "200.50");

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value()) << parse_error_name(result.error());

    // count() should return 2 for the repeated tag
    EXPECT_EQ(result->count(tag::MDEntryPx), 2u);

    // get_nth() should return correct values in order
    EXPECT_EQ(result->get_nth(tag::MDEntryPx, 0).value(), "100.25");
    EXPECT_EQ(result->get_nth(tag::MDEntryPx, 1).value(), "200.50");
}

// ===========================================================================
// has() convenience method
// ===========================================================================

TEST(FixParser, has_returns_true_for_present_tag) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has(tag::MsgType));
    EXPECT_TRUE(result->has(tag::Symbol));
}

TEST(FixParser, has_returns_false_for_absent_tag) {
    std::string body = "35=D\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has(tag::Symbol));
    EXPECT_FALSE(result->has(tag::Price));
}

TEST(FixParser, has_returns_false_on_empty_body) {
    // A minimal message with no body fields (just MsgType is technically required
    // but the parser doesn't enforce that — so an empty body = 0 fields).
    MessageView empty_msg;
    EXPECT_FALSE(empty_msg.has(tag::MsgType));
    EXPECT_FALSE(empty_msg.has(tag::Symbol));
}

// ===========================================================================
// set_raw() builder method
// ===========================================================================

TEST(FixBuilder, set_raw_roundtrip) {
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_raw(tag::Text, payload, sizeof(payload));

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    auto val = result->get(tag::Text);
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(val->size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>((*val)[0]), 0xDE);
    EXPECT_EQ(static_cast<uint8_t>((*val)[1]), 0xAD);
    EXPECT_EQ(static_cast<uint8_t>((*val)[2]), 0xBE);
    EXPECT_EQ(static_cast<uint8_t>((*val)[3]), 0xEF);
}

// ===========================================================================
// Framer
// ===========================================================================

TEST(FixFramer, decode_valid_message) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);

    auto result = FixFramer::decode(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->total_len, raw.size());
    EXPECT_EQ(result->payload_len, raw.size());
    EXPECT_EQ(result->msg_type, static_cast<uint8_t>('D'));
    EXPECT_FALSE(result->is_control);
}

TEST(FixFramer, decode_incomplete) {
    std::string partial = "8=FIX.4.4\x019=10\x01";
    auto result = FixFramer::decode(
        reinterpret_cast<const uint8_t*>(partial.data()), partial.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), eph::net::FrameError::kIncomplete);
}

TEST(FixFramer, decode_bad_start) {
    std::string bad = "X=FIX.4.4\x01";
    auto result = FixFramer::decode(
        reinterpret_cast<const uint8_t*>(bad.data()), bad.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), eph::net::FrameError::kInvalidFormat);
}

TEST(FixFramer, decode_checksum_mismatch) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    // Corrupt a byte in the body to create checksum mismatch
    // (the checksum field itself remains structurally valid)
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == 'A') { raw[i] = 'Z'; break; }
    }
    auto result = FixFramer::decode(raw.data(), raw.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), eph::net::FrameError::kInvalidFormat);
}

TEST(FixFramer, encode_passthrough) {
    uint8_t in[] = {1, 2, 3, 4};
    uint8_t out[8];
    FixFramer f;
    size_t n = f.encode(out, in, sizeof(in), 0);
    EXPECT_EQ(n, sizeof(in));
    EXPECT_EQ(std::memcmp(out, in, sizeof(in)), 0);
}

TEST(FixFramer, satisfies_concept) {
    static_assert(eph::net::MessageFramer<FixFramer>);
}

// ===========================================================================
// std::formatter
// ===========================================================================

// ===========================================================================
// Repeating group support: count() and get_nth()
// ===========================================================================

TEST(FixRepeatingGroup, count_returns_zero_for_absent_tag) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->count(tag::Price), 0u);
}

TEST(FixRepeatingGroup, count_returns_one_for_unique_tag) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->count(tag::Symbol), 1u);
}

TEST(FixRepeatingGroup, count_and_get_nth_for_repeating_fields) {
    // Simulate a market data message with 3 MDEntryPx values
    std::string body =
        "35=W\x01"
        "268=3\x01"       // NoMDEntries=3
        "269=0\x01"       // MDEntryType=Bid
        "270=150.50\x01"  // MDEntryPx
        "269=1\x01"       // MDEntryType=Offer
        "270=151.00\x01"  // MDEntryPx
        "269=2\x01"       // MDEntryType=Trade
        "270=150.75\x01"; // MDEntryPx
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->count(tag::MDEntryPx), 3u);
    EXPECT_EQ(result->count(tag::MDEntryType), 3u);
    EXPECT_EQ(result->count(tag::NoMDEntries), 1u);

    EXPECT_EQ(result->get_nth(tag::MDEntryPx, 0).value(), "150.50");
    EXPECT_EQ(result->get_nth(tag::MDEntryPx, 1).value(), "151.00");
    EXPECT_EQ(result->get_nth(tag::MDEntryPx, 2).value(), "150.75");

    EXPECT_EQ(result->get_nth(tag::MDEntryType, 0).value(), "0");
    EXPECT_EQ(result->get_nth(tag::MDEntryType, 1).value(), "1");
    EXPECT_EQ(result->get_nth(tag::MDEntryType, 2).value(), "2");
}

TEST(FixRepeatingGroup, get_nth_out_of_range_returns_nullopt) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_nth(tag::Symbol, 1).has_value()); // only index 0 exists
    EXPECT_FALSE(result->get_nth(tag::Price, 0).has_value());  // tag absent
}

TEST(FixRepeatingGroup, for_each_matching_collects_all_values) {
    std::string body =
        "35=W\x01"
        "268=3\x01"
        "270=100.00\x01"
        "270=200.00\x01"
        "270=300.00\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    std::vector<std::string> prices;
    result->for_each_matching(tag::MDEntryPx, [&](std::string_view v) {
        prices.emplace_back(v);
    });
    ASSERT_EQ(prices.size(), 3u);
    EXPECT_EQ(prices[0], "100.00");
    EXPECT_EQ(prices[1], "200.00");
    EXPECT_EQ(prices[2], "300.00");
}

TEST(FixRepeatingGroup, for_each_matching_absent_tag_no_calls) {
    std::string body = "35=D\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    size_t calls = 0;
    result->for_each_matching(tag::Price, [&](std::string_view) { ++calls; });
    EXPECT_EQ(calls, 0u);
}

// ===========================================================================
// std::formatter<MessageView>
// ===========================================================================

TEST(FixFormatter, message_view_format_basic) {
    std::string body = "35=D\x01" "55=AAPL\x01" "54=1\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    auto formatted = std::format("{}", *result);
    EXPECT_EQ(formatted, "35=D|55=AAPL|54=1");
}

TEST(FixFormatter, message_view_format_single_field) {
    std::string body = "35=D\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    auto formatted = std::format("{}", *result);
    EXPECT_EQ(formatted, "35=D");
}

TEST(FixFormatter, message_view_format_empty) {
    // A MessageView with 0 fields should format as empty string
    MessageView empty_msg;
    auto formatted = std::format("{}", empty_msg);
    EXPECT_TRUE(formatted.empty());
}

TEST(FixFormatter, parse_error_format) {
    EXPECT_EQ(std::format("{}", ParseError::kIncomplete), "incomplete");
    EXPECT_EQ(std::format("{}", ParseError::kInvalidFormat), "invalid format");
    EXPECT_EQ(std::format("{}", ParseError::kChecksumMismatch), "checksum mismatch");
    EXPECT_EQ(std::format("{}", ParseError::kFieldOverflow), "field overflow");
}

TEST(FixFormatter, frame_error_format) {
    EXPECT_EQ(std::format("{}", eph::net::FrameError::kIncomplete), "incomplete");
    EXPECT_EQ(std::format("{}", eph::net::FrameError::kInvalidFormat), "invalid format");
    EXPECT_EQ(std::format("{}", eph::net::FrameError::kPayloadTooLarge), "payload too large");
}

// ===========================================================================
// parse_all (batch parser)
// ===========================================================================

TEST(FixParseAll, parse_multiple_concatenated_messages) {
    auto raw1 = make_fix_msg("FIX.4.4", "35=D\x01" "55=AAPL\x01");
    auto raw2 = make_fix_msg("FIX.4.4", "35=8\x01" "55=TSLA\x01");
    auto raw3 = make_fix_msg("FIX.4.4", "35=F\x01" "55=MSFT\x01");

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), raw1.begin(), raw1.end());
    combined.insert(combined.end(), raw2.begin(), raw2.end());
    combined.insert(combined.end(), raw3.begin(), raw3.end());

    std::vector<std::string> types;
    size_t consumed = parse_all(combined.data(), combined.size(),
        [&](const MessageView& msg) {
            types.push_back(std::string(msg.msg_type().value()));
        });

    EXPECT_EQ(consumed, combined.size());
    ASSERT_EQ(types.size(), 3u);
    EXPECT_EQ(types[0], "D");
    EXPECT_EQ(types[1], "8");
    EXPECT_EQ(types[2], "F");
}

TEST(FixParseAll, early_stop_returns_consumed_bytes) {
    auto raw1 = make_fix_msg("FIX.4.4", "35=D\x01");
    auto raw2 = make_fix_msg("FIX.4.4", "35=8\x01");

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), raw1.begin(), raw1.end());
    combined.insert(combined.end(), raw2.begin(), raw2.end());

    size_t count = 0;
    size_t consumed = parse_all(combined.data(), combined.size(),
        [&](const MessageView&) -> bool {
            ++count;
            return false; // stop after first
        });

    EXPECT_EQ(count, 1u);
    EXPECT_EQ(consumed, raw1.size());
}

TEST(FixParseAll, empty_buffer_returns_zero) {
    size_t consumed = parse_all(nullptr, 0,
        [](const MessageView&) { FAIL() << "should not be called"; });
    EXPECT_EQ(consumed, 0u);
}

TEST(FixParseAll, partial_message_at_end_stops) {
    auto raw1 = make_fix_msg("FIX.4.4", "35=D\x01");
    std::string partial = "8=FIX.4.4\x01" "9=50\x01"; // incomplete

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), raw1.begin(), raw1.end());
    combined.insert(combined.end(), partial.begin(), partial.end());

    size_t count = 0;
    size_t consumed = parse_all(combined.data(), combined.size(),
        [&](const MessageView&) { ++count; });

    EXPECT_EQ(count, 1u);
    EXPECT_EQ(consumed, raw1.size());
}

// ===========================================================================
// MessageView iterators (begin/end, range-for, structured bindings)
// ===========================================================================

TEST(FixIterator, range_for_loop) {
    std::string body = "35=D\x01" "55=AAPL\x01" "54=1\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    std::vector<uint32_t> tags;
    std::vector<std::string> values;
    for (const auto& field : *result) {
        tags.push_back(field.tag);
        values.emplace_back(field.value);
    }
    ASSERT_EQ(tags.size(), 3u);
    EXPECT_EQ(tags[0], tag::MsgType);
    EXPECT_EQ(tags[1], tag::Symbol);
    EXPECT_EQ(tags[2], tag::Side);
    EXPECT_EQ(values[0], "D");
    EXPECT_EQ(values[1], "AAPL");
    EXPECT_EQ(values[2], "1");
}

TEST(FixIterator, structured_bindings) {
    std::string body = "35=D\x01" "44=150.50\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    auto it = result->begin();
    auto [t1, v1] = *it++;
    EXPECT_EQ(t1, tag::MsgType);
    EXPECT_EQ(v1, "D");

    auto [t2, v2] = *it++;
    EXPECT_EQ(t2, tag::Price);
    EXPECT_EQ(v2, "150.50");

    EXPECT_EQ(it, result->end());
}

TEST(FixIterator, empty_message_view_begin_equals_end) {
    MessageView empty_msg;
    EXPECT_EQ(empty_msg.begin(), empty_msg.end());
    EXPECT_EQ(std::distance(empty_msg.begin(), empty_msg.end()), 0);
}

TEST(FixIterator, std_find_if) {
    std::string body = "35=D\x01" "55=AAPL\x01" "54=1\x01" "44=150.50\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    auto it = std::find_if(result->begin(), result->end(),
        [](const Field& f) { return f.tag == tag::Price; });
    ASSERT_NE(it, result->end());
    EXPECT_EQ(it->value, "150.50");
}

TEST(FixIterator, iterator_arithmetic) {
    std::string body = "35=D\x01" "55=AAPL\x01" "54=1\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->end() - result->begin(), 3);
    EXPECT_EQ(result->begin()[2].tag, tag::Side);
}

// ===========================================================================
// set_timestamp() builder method
// ===========================================================================

TEST(FixBuilder, set_timestamp_epoch_zero) {
    // 1970-01-01 00:00:00.000 UTC
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::SendingTime, 0);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get(tag::SendingTime).value(), "19700101-00:00:00.000");
}

TEST(FixBuilder, set_timestamp_known_date) {
    // 2024-03-15 14:30:45.123 UTC
    // Manually computed: 2024-03-15 is day 19797 from epoch
    // 19797 * 86400 + 14*3600 + 30*60 + 45 = 1710513045
    // In nanoseconds: 1710513045123000000
    uint64_t epoch_ns = 1'710'513'045'123'000'000ULL;

    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::SendingTime, epoch_ns);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get(tag::SendingTime).value(), "20240315-14:30:45.123");
}

TEST(FixBuilder, set_timestamp_transact_time) {
    // Test with TransactTime tag (60)
    // 2026-01-01 00:00:00.000 UTC = 1767225600 seconds
    uint64_t epoch_ns = 1'767'225'600'000'000'000ULL;

    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::TransactTime, epoch_ns);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get(tag::TransactTime).value(), "20260101-00:00:00.000");
}

TEST(FixBuilder, set_timestamp_millisecond_precision) {
    // Verify millisecond precision: 999ms
    // 1970-01-01 00:00:00.999 UTC
    uint64_t epoch_ns = 999'000'000ULL;

    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::SendingTime, epoch_ns);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get(tag::SendingTime).value(), "19700101-00:00:00.999");
}

TEST(FixBuilder, set_timestamp_end_of_day) {
    // 1970-01-01 23:59:59.500 UTC
    uint64_t epoch_ns = (23ULL*3600 + 59*60 + 59) * 1'000'000'000ULL + 500'000'000ULL;

    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::SendingTime, epoch_ns);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get(tag::SendingTime).value(), "19700101-23:59:59.500");
}

TEST(FixBuilder, set_timestamp_february_date) {
    // 2000-02-29 12:00:00.000 UTC (leap day)
    // 2000-02-29 = day 11016 from epoch
    // 11016 * 86400 + 12*3600 = 951825600
    uint64_t epoch_ns = 951'825'600'000'000'000ULL;

    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::SendingTime, epoch_ns);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get(tag::SendingTime).value(), "20000229-12:00:00.000");
}

TEST(FixBuilder, set_timestamp_february_non_leap) {
    // 2023-02-28 08:15:30.456 UTC
    // 2023-02-28 = 1677542400 epoch seconds + 8*3600 + 15*60 + 30 = 1677572130
    uint64_t epoch_ns = 1'677'572'130'456'000'000ULL;

    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::SendingTime, epoch_ns);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get(tag::SendingTime).value(), "20230228-08:15:30.456");
}

TEST(FixBuilder, set_timestamp_format_length) {
    // Verify the timestamp is exactly 21 characters: YYYYMMDD-HH:MM:SS.sss
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::SendingTime, 0);

    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(b.data(), b.size());
    ASSERT_TRUE(result.has_value());
    auto ts = result->get(tag::SendingTime).value();
    EXPECT_EQ(ts.size(), 21u);
    // Verify format: YYYYMMDD-HH:MM:SS.sss
    EXPECT_EQ(ts[8], '-');
    EXPECT_EQ(ts[11], ':');
    EXPECT_EQ(ts[14], ':');
    EXPECT_EQ(ts[17], '.');
}

// ===========================================================================
// get_timestamp() — parse UTCTimestamp back to epoch nanoseconds
// ===========================================================================

TEST(FixParser, get_timestamp_epoch_zero) {
    // 1970-01-01 00:00:00.000 → 0 ns
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "52=19700101-00:00:00.000\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    auto ts = msg->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(*ts, 0u);
}

TEST(FixParser, get_timestamp_round_trip) {
    // Build a timestamp with set_timestamp, then parse it back with get_timestamp
    constexpr uint64_t epoch_ns = 1'700'000'000'123'000'000ULL; // 2023-11-14 22:13:20.123
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::SendingTime, epoch_ns);
    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto msg = parse(b.data(), b.size());
    ASSERT_TRUE(msg.has_value());
    auto ts = msg->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    // Round-trip: millisecond precision (nanoseconds within the ms are truncated by set_timestamp)
    EXPECT_EQ(*ts, (epoch_ns / 1'000'000ULL) * 1'000'000ULL);
}

TEST(FixParser, get_timestamp_without_millis) {
    // "YYYYMMDD-HH:MM:SS" (17 chars, no fractional seconds)
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "52=20250101-12:30:45\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    auto ts = msg->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());

    // 2025-01-01 12:30:45 UTC
    // Days from 1970-01-01 to 2025-01-01 = 20089
    uint64_t expected = (20089ULL * 86400 + 12 * 3600 + 30 * 60 + 45) * 1'000'000'000ULL;
    EXPECT_EQ(*ts, expected);
}

TEST(FixParser, get_timestamp_feb_29_leap_year) {
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "52=20240229-00:00:00.000\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    auto ts = msg->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    // 2024-02-29 is valid (leap year)
    EXPECT_GT(*ts, 0u);
}

TEST(FixParser, get_timestamp_invalid_format_returns_nullopt) {
    // Wrong separator
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "52=2025/01/01-12:30:45\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->get_timestamp(tag::SendingTime).has_value());
}

TEST(FixParser, get_timestamp_missing_tag_returns_nullopt) {
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "55=AAPL\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->get_timestamp(tag::SendingTime).has_value());
}

TEST(FixParser, get_timestamp_wrong_length_returns_nullopt) {
    // Too short
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "52=20250101\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->get_timestamp(tag::SendingTime).has_value());
}

TEST(FixParser, get_timestamp_pre_epoch_returns_nullopt) {
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "52=19691231-23:59:59.999\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->get_timestamp(tag::SendingTime).has_value());
}

TEST(FixParser, get_timestamp_feb_29_non_leap_returns_nullopt) {
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "52=20230229-00:00:00.000\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->get_timestamp(tag::SendingTime).has_value());
}

TEST(FixParser, get_timestamp_feb_31_returns_nullopt) {
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "52=20250231-12:00:00.000\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->get_timestamp(tag::SendingTime).has_value());
}

TEST(FixParser, get_timestamp_apr_31_returns_nullopt) {
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "52=20250431-12:00:00.000\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->get_timestamp(tag::SendingTime).has_value());
}

// ===========================================================================
// dispatch() — type-safe MsgType routing
// ===========================================================================

TEST(FixDispatch, dispatches_new_order_single) {
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01" "55=AAPL\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());

    bool matched = false;
    dispatch(*msg, [&](auto tag_type, const MessageView& v) {
        if constexpr (std::is_same_v<decltype(tag_type), msg::NewOrderSingle>) {
            matched = true;
            EXPECT_EQ(v.get(tag::Symbol).value(), "AAPL");
        }
    });
    EXPECT_TRUE(matched);
}

TEST(FixDispatch, dispatches_execution_report) {
    auto raw = make_fix_msg("FIX.4.4", "35=8\x01" "17=EXEC1\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());

    bool matched = false;
    dispatch(*msg, [&](auto tag_type, const MessageView& v) {
        if constexpr (std::is_same_v<decltype(tag_type), msg::ExecutionReport>) {
            matched = true;
            EXPECT_EQ(v.get(tag::ExecID).value(), "EXEC1");
        }
    });
    EXPECT_TRUE(matched);
}

TEST(FixDispatch, dispatches_heartbeat) {
    auto raw = make_fix_msg("FIX.4.4", "35=0\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());

    bool matched = false;
    dispatch(*msg, [&](auto tag_type, const MessageView&) {
        if constexpr (std::is_same_v<decltype(tag_type), msg::Heartbeat>) {
            matched = true;
        }
    });
    EXPECT_TRUE(matched);
}

TEST(FixDispatch, unknown_msg_type_dispatches_unknown) {
    auto raw = make_fix_msg("FIX.4.4", "35=~\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());

    bool matched = false;
    dispatch(*msg, [&](auto tag_type, const MessageView&) {
        if constexpr (std::is_same_v<decltype(tag_type), msg::Unknown>) {
            matched = true;
        }
    });
    EXPECT_TRUE(matched);
}

TEST(FixDispatch, handler_return_value_forwarded) {
    auto raw = make_fix_msg("FIX.4.4", "35=D\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());

    int result = dispatch(*msg, [](auto tag_type, const MessageView&) -> int {
        if constexpr (std::is_same_v<decltype(tag_type), msg::NewOrderSingle>) {
            return 42;
        }
        return 0;
    });
    EXPECT_EQ(result, 42);
}

TEST(FixDispatch, all_known_msg_types_dispatch_correctly) {
    // Verify each known MsgType dispatches to the correct tag type
    struct TestCase { char mt; std::string_view expected_type; };
    std::vector<TestCase> cases = {
        {'0', "Heartbeat"}, {'1', "TestRequest"}, {'A', "Logon"},
        {'5', "Logout"}, {'D', "NewOrderSingle"}, {'F', "OrderCancelRequest"},
        {'G', "OrderCancelReplace"}, {'8', "ExecutionReport"},
        {'9', "OrderCancelReject"}, {'V', "MarketDataRequest"},
        {'W', "MarketDataSnapshot"}, {'X', "MarketDataIncRefresh"},
    };

    for (auto& tc : cases) {
        std::string body = "35=";
        body += tc.mt;
        body += '\x01';
        auto raw = make_fix_msg("FIX.4.4", body);
        auto msg = parse(raw.data(), raw.size());
        ASSERT_TRUE(msg.has_value()) << "Failed to parse MsgType=" << tc.mt;

        std::string_view dispatched = "none";
        dispatch(*msg, [&](auto tag_type, const MessageView&) {
            using T = decltype(tag_type);
            if constexpr (std::is_same_v<T, msg::Heartbeat>)            dispatched = "Heartbeat";
            else if constexpr (std::is_same_v<T, msg::TestRequest>)     dispatched = "TestRequest";
            else if constexpr (std::is_same_v<T, msg::Logon>)           dispatched = "Logon";
            else if constexpr (std::is_same_v<T, msg::Logout>)          dispatched = "Logout";
            else if constexpr (std::is_same_v<T, msg::NewOrderSingle>)  dispatched = "NewOrderSingle";
            else if constexpr (std::is_same_v<T, msg::OrderCancelRequest>) dispatched = "OrderCancelRequest";
            else if constexpr (std::is_same_v<T, msg::OrderCancelReplace>) dispatched = "OrderCancelReplace";
            else if constexpr (std::is_same_v<T, msg::ExecutionReport>)    dispatched = "ExecutionReport";
            else if constexpr (std::is_same_v<T, msg::OrderCancelReject>)  dispatched = "OrderCancelReject";
            else if constexpr (std::is_same_v<T, msg::MarketDataRequest>)  dispatched = "MarketDataRequest";
            else if constexpr (std::is_same_v<T, msg::MarketDataSnapshot>) dispatched = "MarketDataSnapshot";
            else if constexpr (std::is_same_v<T, msg::MarketDataIncRefresh>) dispatched = "MarketDataIncRefresh";
            else if constexpr (std::is_same_v<T, msg::Unknown>)         dispatched = "Unknown";
        });
        EXPECT_EQ(dispatched, tc.expected_type) << "MsgType=" << tc.mt;
    }
}

// ===========================================================================
// get_bool() / set_bool() — FIX Y/N boolean fields
// ===========================================================================

TEST(FixParser, get_bool_Y_returns_true) {
    auto raw = make_fix_msg("FIX.4.4", "35=A\x01" "43=Y\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    // Tag 43 = PossDupFlag
    auto val = msg->get_bool(43);
    ASSERT_TRUE(val.has_value());
    EXPECT_TRUE(*val);
}

TEST(FixParser, get_bool_N_returns_false) {
    auto raw = make_fix_msg("FIX.4.4", "35=A\x01" "43=N\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    auto val = msg->get_bool(43);
    ASSERT_TRUE(val.has_value());
    EXPECT_FALSE(*val);
}

TEST(FixParser, get_bool_invalid_value_returns_nullopt) {
    auto raw = make_fix_msg("FIX.4.4", "35=A\x01" "43=X\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->get_bool(43).has_value());
}

TEST(FixParser, get_bool_multi_char_returns_nullopt) {
    auto raw = make_fix_msg("FIX.4.4", "35=A\x01" "43=YES\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->get_bool(43).has_value());
}

TEST(FixParser, get_bool_missing_tag_returns_nullopt) {
    auto raw = make_fix_msg("FIX.4.4", "35=A\x01");
    auto msg = parse(raw.data(), raw.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_FALSE(msg->get_bool(43).has_value());
}

TEST(FixBuilder, set_bool_true_writes_Y) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "A");
    b.set_bool(43, true);
    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto msg = parse(b.data(), b.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->get(43).value(), "Y");
    EXPECT_TRUE(msg->get_bool(43).value());
}

TEST(FixBuilder, set_bool_false_writes_N) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "A");
    b.set_bool(43, false);
    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto msg = parse(b.data(), b.size());
    ASSERT_TRUE(msg.has_value());
    EXPECT_EQ(msg->get(43).value(), "N");
    EXPECT_FALSE(msg->get_bool(43).value());
}

// ===========================================================================
// has_overflow() — early overflow detection
// ===========================================================================

TEST(FixBuilder, has_overflow_false_initially) {
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    EXPECT_FALSE(b.has_overflow());
}

TEST(FixBuilder, has_overflow_true_after_buffer_exceeded) {
    uint8_t buf[40]; // very small buffer
    MessageBuilder b(buf, sizeof(buf));
    EXPECT_FALSE(b.has_overflow());
    b.set(tag::MsgType, "D");
    // This long value should overflow the tiny buffer
    b.set(tag::Text, "This is a very long text value that will overflow the small buffer");
    EXPECT_TRUE(b.has_overflow());
    EXPECT_EQ(b.finish(), 0u);
}

TEST(FixBuilder, has_overflow_false_after_reset) {
    uint8_t buf[40];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::Text, "This is a very long text value that will overflow the small buffer");
    EXPECT_TRUE(b.has_overflow());
    b.reset();
    EXPECT_FALSE(b.has_overflow());
}

// ===========================================================================
// BasicMessageView<N> — custom capacity template parameter
// ===========================================================================

TEST(FixCustomCapacity, parse_with_small_capacity_overflows_earlier) {
    // Build a body with 5 fields — exceeds capacity of 4
    std::string body;
    for (int i = 0; i < 5; ++i) {
        body += std::to_string(5000 + i) + "=v\x01";
    }
    auto raw = make_fix_msg("FIX.4.4", body);

    // Default capacity (128) should succeed
    auto result_default = parse(raw.data(), raw.size());
    ASSERT_TRUE(result_default.has_value());
    EXPECT_EQ(result_default->field_count(), 5u);

    // Small capacity (4) should overflow
    auto result_small = parse<4>(raw.data(), raw.size());
    ASSERT_FALSE(result_small.has_value());
    EXPECT_EQ(result_small.error(), ParseError::kFieldOverflow);
}

TEST(FixCustomCapacity, parse_with_large_capacity_handles_many_fields) {
    // Build a body with 200 fields — exceeds default 128 but fits 256
    std::string body;
    body += "35=D\x01";
    for (int i = 0; i < 199; ++i) {
        body += std::to_string(5000 + i) + "=v\x01";
    }
    auto raw = make_fix_msg("FIX.4.4", body);

    // Default capacity (128) should overflow
    auto result_default = parse(raw.data(), raw.size());
    ASSERT_FALSE(result_default.has_value());
    EXPECT_EQ(result_default.error(), ParseError::kFieldOverflow);

    // Large capacity (256) should succeed
    auto result_large = parse<256>(raw.data(), raw.size());
    ASSERT_TRUE(result_large.has_value());
    EXPECT_EQ(result_large->field_count(), 200u);
    EXPECT_EQ(result_large->kMaxFields, 256u);
}

TEST(FixCustomCapacity, dispatch_deduces_template_from_view) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);

    auto result = parse<16>(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    bool dispatched = false;
    dispatch(*result, [&](auto tag_type, const auto& view) {
        if constexpr (std::is_same_v<decltype(tag_type), msg::NewOrderSingle>) {
            dispatched = true;
            EXPECT_EQ(view.kMaxFields, 16u);
            auto sym = view.get(tag::Symbol);
            ASSERT_TRUE(sym.has_value());
            EXPECT_EQ(*sym, "AAPL");
        }
    });
    EXPECT_TRUE(dispatched);
}

TEST(FixCustomCapacity, parse_all_with_custom_capacity) {
    std::string body1 = "35=D\x01" "55=AAPL\x01";
    std::string body2 = "35=8\x01" "55=MSFT\x01";
    auto raw1 = make_fix_msg("FIX.4.4", body1);
    auto raw2 = make_fix_msg("FIX.4.4", body2);

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), raw1.begin(), raw1.end());
    combined.insert(combined.end(), raw2.begin(), raw2.end());

    size_t msg_count = 0;
    size_t consumed = parse_all<16>(combined.data(), combined.size(),
        [&](const BasicMessageView<16>& view) {
            ++msg_count;
            EXPECT_EQ(view.kMaxFields, 16u);
        });

    EXPECT_EQ(msg_count, 2u);
    EXPECT_EQ(consumed, combined.size());
}

TEST(FixCustomCapacity, formatter_works_with_custom_capacity) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);

    auto result = parse<16>(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    std::string formatted = std::format("{}", *result);
    EXPECT_NE(formatted.find("35=D"), std::string::npos);
    EXPECT_NE(formatted.find("55=AAPL"), std::string::npos);
}

TEST(FixCustomCapacity, basic_parser_class_with_custom_capacity) {
    std::string body = "35=D\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);

    BasicParser<16> parser;
    auto result = parser(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->kMaxFields, 16u);
    EXPECT_EQ(result->field_count(), 2u);
}

// ===========================================================================
// Timestamp microsecond/nanosecond precision (parser)
// ===========================================================================

TEST(FixParser, get_timestamp_microsecond_precision) {
    // "20260322-12:30:45.123456" — 24 chars, microsecond precision
    std::string body = "35=D\x01" "52=20260322-12:30:45.123456\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    auto ts = result->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());

    // Expected: 2026-03-22 12:30:45.123456 UTC as nanoseconds since epoch
    // Verify the sub-second part: 123456 microseconds = 123456000 nanoseconds
    uint64_t sub_sec_ns = *ts % 1'000'000'000ULL;
    EXPECT_EQ(sub_sec_ns, 123'456'000ULL);
}

TEST(FixParser, get_timestamp_nanosecond_precision) {
    // "20260322-12:30:45.123456789" — 27 chars, nanosecond precision
    std::string body = "35=D\x01" "52=20260322-12:30:45.123456789\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    auto ts = result->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());

    uint64_t sub_sec_ns = *ts % 1'000'000'000ULL;
    EXPECT_EQ(sub_sec_ns, 123'456'789ULL);
}

TEST(FixParser, get_timestamp_invalid_precision_length_rejected) {
    // 2 fractional digits — not a valid FIX precision
    std::string body = "35=D\x01" "52=20260322-12:30:45.12\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_timestamp(tag::SendingTime).has_value());
}

TEST(FixParser, get_timestamp_microsecond_roundtrip) {
    // Build with microsecond precision, parse back
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    // 2026-03-22 12:30:45.123456 UTC
    // First compute the epoch_ns for this timestamp
    // Use a known value and verify round-trip
    uint64_t epoch_ns = 1774191045'123'456'789ULL; // some timestamp with ns

    b.set_timestamp(tag::SendingTime, epoch_ns,
                    MessageBuilder::TimestampPrecision::kMicroseconds);
    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    auto ts = result->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    // Microsecond precision loses the last 3 ns digits
    EXPECT_EQ(*ts / 1000ULL, epoch_ns / 1000ULL);
}

TEST(FixParser, get_timestamp_nanosecond_roundtrip) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    uint64_t epoch_ns = 1774191045'123'456'789ULL;

    b.set_timestamp(tag::SendingTime, epoch_ns,
                    MessageBuilder::TimestampPrecision::kNanoseconds);
    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    auto ts = result->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    // Nanosecond precision preserves full value
    EXPECT_EQ(*ts, epoch_ns);
}

TEST(FixParser, get_timestamp_seconds_only_roundtrip) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    uint64_t epoch_ns = 1774191045'000'000'000ULL; // exact second

    b.set_timestamp(tag::SendingTime, epoch_ns,
                    MessageBuilder::TimestampPrecision::kSeconds);
    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());

    // Verify format is exactly 17 chars (no fractional part)
    auto sv = result->get(tag::SendingTime);
    ASSERT_TRUE(sv.has_value());
    EXPECT_EQ(sv->size(), 17u);

    auto ts = result->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    EXPECT_EQ(*ts, epoch_ns);
}

// ===========================================================================
// Multi-char MsgType dispatch
// ===========================================================================

TEST(FixDispatch, dispatches_trade_capture_report) {
    std::string body = "35=AE\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    bool dispatched = false;
    dispatch(*result, [&](auto tag, const auto&) {
        if constexpr (std::is_same_v<decltype(tag), msg::TradeCaptureReport>) {
            dispatched = true;
        }
    });
    EXPECT_TRUE(dispatched);
}

TEST(FixDispatch, dispatches_trade_capture_report_ack) {
    std::string body = "35=AR\x01" "55=MSFT\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    bool dispatched = false;
    dispatch(*result, [&](auto tag, const auto&) {
        if constexpr (std::is_same_v<decltype(tag), msg::TradeCaptureReportAck>) {
            dispatched = true;
        }
    });
    EXPECT_TRUE(dispatched);
}

TEST(FixDispatch, dispatches_position_report) {
    std::string body = "35=AP\x01" "55=GOOG\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    bool dispatched = false;
    dispatch(*result, [&](auto tag, const auto&) {
        if constexpr (std::is_same_v<decltype(tag), msg::PositionReport>) {
            dispatched = true;
        }
    });
    EXPECT_TRUE(dispatched);
}

TEST(FixDispatch, unknown_multi_char_msg_type_dispatches_unknown) {
    std::string body = "35=ZZ\x01" "55=AAPL\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    bool dispatched = false;
    dispatch(*result, [&](auto tag, const auto&) {
        if constexpr (std::is_same_v<decltype(tag), msg::Unknown>) {
            dispatched = true;
        }
    });
    EXPECT_TRUE(dispatched);
}

TEST(FixDispatch, single_char_security_definition) {
    std::string body = "35=d\x01" "55=TSLA\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());

    bool dispatched = false;
    dispatch(*result, [&](auto tag, const auto&) {
        if constexpr (std::is_same_v<decltype(tag), msg::SecurityDefinition>) {
            dispatched = true;
        }
    });
    EXPECT_TRUE(dispatched);
}

// ===========================================================================
// MessageBuilder: bytes_used, remaining_capacity, set_char
// ===========================================================================

TEST(FixBuilder, set_char_single_char_field) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_char(tag::Side, '1');     // Buy
    b.set_char(tag::OrdType, '2'); // Limit
    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->get_char(tag::Side), '1');
    EXPECT_EQ(result->get_char(tag::OrdType), '2');
}

TEST(FixBuilder, bytes_used_tracks_body_bytes) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    EXPECT_EQ(b.bytes_used(), 0u);

    b.set(tag::MsgType, "D"); // "35=D\x01" = 5 bytes
    EXPECT_EQ(b.bytes_used(), 5u);

    b.set(tag::Symbol, "AAPL"); // "55=AAPL\x01" = 8 bytes
    EXPECT_EQ(b.bytes_used(), 13u);
}

TEST(FixBuilder, remaining_capacity_decreases) {
    uint8_t buf[128];
    MessageBuilder b(buf, sizeof(buf));
    size_t initial = b.remaining_capacity();
    EXPECT_GT(initial, 0u);

    b.set(tag::MsgType, "D");
    EXPECT_LT(b.remaining_capacity(), initial);
}

TEST(FixBuilder, remaining_capacity_zero_after_overflow) {
    uint8_t buf[32]; // Very small buffer
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set(tag::Symbol, "VERY_LONG_SYMBOL_NAME_THAT_OVERFLOWS");
    EXPECT_TRUE(b.has_overflow());
    EXPECT_EQ(b.remaining_capacity(), 0u);
}

// ===========================================================================
// Additional coverage: edge cases for timestamp and builder
// ===========================================================================

TEST(FixParser, get_timestamp_leap_second_accepted) {
    // Second=60 is valid (leap second) per FIX spec
    std::string body = "35=D\x01" "52=20161231-23:59:60\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    auto ts = result->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
}

TEST(FixParser, get_timestamp_second_61_rejected) {
    std::string body = "35=D\x01" "52=20261231-23:59:61\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->get_timestamp(tag::SendingTime).has_value());
}

TEST(FixParser, get_timestamp_fractional_leading_zeros) {
    // "000001" microseconds = 1 microsecond = 1000 nanoseconds
    std::string body = "35=D\x01" "52=20260322-12:00:00.000001\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    auto ts = result->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    uint64_t sub_sec_ns = *ts % 1'000'000'000ULL;
    EXPECT_EQ(sub_sec_ns, 1'000ULL); // 1 microsecond = 1000 ns
}

TEST(FixParser, get_timestamp_nanosecond_leading_zeros) {
    // "000000001" nanoseconds = 1 nanosecond
    std::string body = "35=D\x01" "52=20260322-12:00:00.000000001\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    auto ts = result->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    uint64_t sub_sec_ns = *ts % 1'000'000'000ULL;
    EXPECT_EQ(sub_sec_ns, 1ULL);
}

TEST(FixParser, get_timestamp_max_fractional_values) {
    // "999999999" nanoseconds = 999999999 ns
    std::string body = "35=D\x01" "52=20260322-12:00:00.999999999\x01";
    auto raw = make_fix_msg("FIX.4.4", body);
    auto result = parse(raw.data(), raw.size());
    ASSERT_TRUE(result.has_value());
    auto ts = result->get_timestamp(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    uint64_t sub_sec_ns = *ts % 1'000'000'000ULL;
    EXPECT_EQ(sub_sec_ns, 999'999'999ULL);
}

TEST(FixBuilder, set_char_overflow_detected) {
    uint8_t buf[48]; // small buffer
    MessageBuilder b(buf, sizeof(buf));
    // Fill most of the buffer
    b.set(tag::MsgType, "D");
    b.set(tag::Symbol, "AAPL_LONG_SYM");
    // This set_char may or may not overflow depending on remaining space
    b.set_char(tag::Side, '1');
    // Verify overflow is detectable
    if (b.has_overflow()) {
        EXPECT_EQ(b.remaining_capacity(), 0u);
    }
    // Either way, finish() should return 0 on overflow or valid length
    size_t len = b.finish();
    if (b.has_overflow()) {
        EXPECT_EQ(len, 0u);
    }
}

TEST(FixBuilder, bytes_used_with_timestamp_precision) {
    uint8_t buf[512];

    // Seconds precision: "52=YYYYMMDD-HH:MM:SS\x01" = 3 + 17 + 1 = 21 bytes
    MessageBuilder b1(buf, sizeof(buf));
    b1.set_timestamp(tag::SendingTime, 1774191045'000'000'000ULL,
                     MessageBuilder::TimestampPrecision::kSeconds);
    size_t bytes_sec = b1.bytes_used();

    // Nanoseconds precision: "52=YYYYMMDD-HH:MM:SS.sssssssss\x01" = 3 + 27 + 1 = 31 bytes
    MessageBuilder b2(buf, sizeof(buf));
    b2.set_timestamp(tag::SendingTime, 1774191045'000'000'000ULL,
                     MessageBuilder::TimestampPrecision::kNanoseconds);
    size_t bytes_ns = b2.bytes_used();

    // Nanosecond precision uses 10 more bytes than seconds (. + 9 digits)
    EXPECT_EQ(bytes_ns - bytes_sec, 10u);
}

TEST(FixBuilder, remaining_capacity_exact_boundary) {
    // Create a buffer where adding one more field would exactly hit the limit
    uint8_t buf[128];
    MessageBuilder b(buf, sizeof(buf));

    // Add fields until remaining capacity is small
    while (b.remaining_capacity() > 20 && !b.has_overflow()) {
        b.set_int(tag::MsgSeqNum, 1);
    }
    EXPECT_FALSE(b.has_overflow());
    EXPECT_GT(b.remaining_capacity(), 0u);

    // Verify finish still works
    b.set(tag::MsgType, "0"); // heartbeat
    size_t len = b.finish();
    // May or may not overflow depending on exact boundary
    if (!b.has_overflow()) {
        EXPECT_GT(len, 0u);
    }
}

// ---------------------------------------------------------------------------
// set_double NaN/Infinity protection
// ---------------------------------------------------------------------------

TEST(FixBuilder, set_double_nan_causes_overflow) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_double(tag::Price, std::numeric_limits<double>::quiet_NaN());
    EXPECT_TRUE(b.has_overflow());
    EXPECT_EQ(b.finish(), 0u);
}

TEST(FixBuilder, set_double_positive_infinity_causes_overflow) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_double(tag::Price, std::numeric_limits<double>::infinity());
    EXPECT_TRUE(b.has_overflow());
    EXPECT_EQ(b.finish(), 0u);
}

TEST(FixBuilder, set_double_negative_infinity_causes_overflow) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_double(tag::Price, -std::numeric_limits<double>::infinity());
    EXPECT_TRUE(b.has_overflow());
    EXPECT_EQ(b.finish(), 0u);
}

TEST(FixBuilder, set_double_finite_values_still_work) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_double(tag::Price, 0.0);
    b.set_double(tag::StopPx, -99.99);
    b.set_double(tag::AvgPx, 1e15, 0); // large but finite
    EXPECT_FALSE(b.has_overflow());
    size_t len = b.finish();
    EXPECT_GT(len, 0u);
}

// ---------------------------------------------------------------------------
// set_timestamp edge cases
// ---------------------------------------------------------------------------

TEST(FixBuilder, set_timestamp_uint64_max_produces_valid_date) {
    // UINT64_MAX nanoseconds = ~2554-07-21T23:34:33 — still within YYYYMMDD range.
    // No overflow possible since max uint64_t ns < year 10000.
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::SendingTime, UINT64_MAX);
    EXPECT_FALSE(b.has_overflow());
    size_t len = b.finish();
    EXPECT_GT(len, 0u);

    // Parse back and verify we get a date starting with "2554"
    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    auto ts = result->get(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    EXPECT_TRUE(ts->starts_with("2554"));
}

TEST(FixBuilder, set_timestamp_zero_epoch) {
    uint8_t buf[512];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "D");
    b.set_timestamp(tag::SendingTime, 0);
    EXPECT_FALSE(b.has_overflow());
    size_t len = b.finish();
    EXPECT_GT(len, 0u);

    auto result = parse(buf, len);
    ASSERT_TRUE(result.has_value());
    auto ts = result->get(tag::SendingTime);
    ASSERT_TRUE(ts.has_value());
    EXPECT_TRUE(ts->starts_with("19700101-00:00:00"));
}

// ---------------------------------------------------------------------------
// ParserStats
// ---------------------------------------------------------------------------

TEST(FixParserStats, accumulates_on_successful_parse) {
    // Build two messages
    uint8_t buf1[256], buf2[256];
    MessageBuilder b1(buf1, sizeof(buf1));
    b1.set(tag::MsgType, "0");
    size_t len1 = b1.finish();
    ASSERT_GT(len1, 0u);

    MessageBuilder b2(buf2, sizeof(buf2));
    b2.set(tag::MsgType, "A");
    size_t len2 = b2.finish();
    ASSERT_GT(len2, 0u);

    // Concatenate
    std::vector<uint8_t> combined(len1 + len2);
    std::memcpy(combined.data(), buf1, len1);
    std::memcpy(combined.data() + len1, buf2, len2);

    ParserStats stats;
    size_t consumed = parse_all(combined.data(), combined.size(),
        [](const auto&) {}, stats);

    EXPECT_EQ(consumed, len1 + len2);
    EXPECT_EQ(stats.messages_parsed, 2u);
    EXPECT_EQ(stats.parse_errors, 0u);
    EXPECT_EQ(stats.bytes_consumed, len1 + len2);
}

TEST(FixParserStats, counts_errors_on_corrupt_data) {
    // Corrupt data that isn't just "incomplete"
    const uint8_t corrupt[] = "GARBAGE_NOT_FIX\x01";
    ParserStats stats;
    size_t consumed = parse_all(corrupt, sizeof(corrupt) - 1,
        [](const auto&) {}, stats);

    EXPECT_EQ(consumed, 0u);
    EXPECT_EQ(stats.messages_parsed, 0u);
    EXPECT_EQ(stats.parse_errors, 1u);
}

TEST(FixParserStats, reset_clears_all_counters) {
    ParserStats stats;
    stats.messages_parsed = 42;
    stats.parse_errors = 5;
    stats.bytes_consumed = 9999;
    stats.reset();
    EXPECT_EQ(stats.messages_parsed, 0u);
    EXPECT_EQ(stats.parse_errors, 0u);
    EXPECT_EQ(stats.bytes_consumed, 0u);
}

TEST(FixParserStats, incomplete_trailing_data_is_not_counted_as_error) {
    // Build one complete message + partial trailing data
    uint8_t buf[256];
    MessageBuilder b(buf, sizeof(buf));
    b.set(tag::MsgType, "0");
    size_t len = b.finish();
    ASSERT_GT(len, 0u);

    // Append incomplete data "8=FI" (starts valid but is truncated)
    std::vector<uint8_t> combined(len + 4);
    std::memcpy(combined.data(), buf, len);
    std::memcpy(combined.data() + len, "8=FI", 4);

    ParserStats stats;
    size_t consumed = parse_all(combined.data(), combined.size(),
        [](const auto&) {}, stats);

    EXPECT_EQ(consumed, len);
    EXPECT_EQ(stats.messages_parsed, 1u);
    EXPECT_EQ(stats.parse_errors, 0u); // incomplete is not an error
}
