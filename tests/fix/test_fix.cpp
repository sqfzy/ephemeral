#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
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
