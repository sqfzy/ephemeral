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
    EXPECT_EQ(tag::msg_type_name('Z'), "Unknown");
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
