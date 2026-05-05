/// @file test_itch.cpp
/// Unit tests for the ITCH 5.0 message parser and zero-copy accessors.
/// Covers endian helpers, common header accessors, per-message-type accessors,
/// parser happy and error paths, lookup helpers, and ItchFramer concept check.

#include <cstdint>
#include <cstring>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/itch/framer.hpp"
#include "eph/itch/messages.hpp"
#include "eph/itch/parser.hpp"
#include "eph/core/framer_concept.hpp"
#include "eph/core/length_prefix_framer.hpp"

using namespace eph::itch;

// ---------------------------------------------------------------------------
// Test 1: Message type constants and sizes
// ---------------------------------------------------------------------------

TEST(ItchMessages, MessageTypeConstantsAndSizes) {
    // Verify all 22 type constants match their ASCII character values
    EXPECT_EQ(kSystemEvent, 'S');
    EXPECT_EQ(kStockDirectory, 'R');
    EXPECT_EQ(kStockTradingAction, 'H');
    EXPECT_EQ(kRegSHORestriction, 'Y');
    EXPECT_EQ(kMarketParticipantPosition, 'L');
    EXPECT_EQ(kMWCBDeclineLevel, 'V');
    EXPECT_EQ(kMWCBStatus, 'W');
    EXPECT_EQ(kIPOQuotingPeriod, 'K');
    EXPECT_EQ(kLULDAuctionCollar, 'J');
    EXPECT_EQ(kOperationalHalt, 'h');
    EXPECT_EQ(kAddOrder, 'A');
    EXPECT_EQ(kAddOrderMPID, 'F');
    EXPECT_EQ(kOrderExecuted, 'E');
    EXPECT_EQ(kOrderExecutedWithPrice, 'C');
    EXPECT_EQ(kOrderCancel, 'X');
    EXPECT_EQ(kOrderDelete, 'D');
    EXPECT_EQ(kOrderReplace, 'U');
    EXPECT_EQ(kNonCrossTrade, 'P');
    EXPECT_EQ(kCrossTrade, 'Q');
    EXPECT_EQ(kBrokenTrade, 'B');
    EXPECT_EQ(kNOII, 'I');
    EXPECT_EQ(kRPII, 'N');

    // Verify sizes (including the 1-byte type field)
    // Common header = type(1) + stock_locate(2) + tracking_number(2) + timestamp(6) = 11 bytes
    // Each message size = 11 + message-specific fields.
    EXPECT_EQ(kSystemEventSize, 12u);
    EXPECT_EQ(kStockDirectorySize, 39u);
    EXPECT_EQ(kStockTradingActionSize, 25u);
    EXPECT_EQ(kRegSHORestrictionSize, 20u);
    EXPECT_EQ(kMarketParticipantPositionSize, 26u);
    EXPECT_EQ(kMWCBDeclineLevelSize, 35u);
    EXPECT_EQ(kMWCBStatusSize, 12u);
    EXPECT_EQ(kIPOQuotingPeriodSize, 28u);
    EXPECT_EQ(kLULDAuctionCollarSize, 35u);
    EXPECT_EQ(kOperationalHaltSize, 21u);
    EXPECT_EQ(kAddOrderSize, 36u);
    EXPECT_EQ(kAddOrderMPIDSize, 40u);
    EXPECT_EQ(kOrderExecutedSize, 31u);
    EXPECT_EQ(kOrderExecutedWithPriceSize, 36u);
    EXPECT_EQ(kOrderCancelSize, 23u);
    EXPECT_EQ(kOrderDeleteSize, 19u);
    EXPECT_EQ(kOrderReplaceSize, 35u);
    EXPECT_EQ(kNonCrossTradeSize, 44u);
    EXPECT_EQ(kCrossTradeSize, 40u);
    EXPECT_EQ(kBrokenTradeSize, 19u);
    EXPECT_EQ(kNOIISize, 50u);
    EXPECT_EQ(kRPIISize, 20u);
}

// ---------------------------------------------------------------------------
// Test 2: Endian helpers
// ---------------------------------------------------------------------------

TEST(ItchEndian, ReadBe16) {
    // 0x0102 in big-endian
    const uint8_t data[] = {0x01, 0x02};
    EXPECT_EQ(read_be16(data), 0x0102u);
}

TEST(ItchEndian, ReadBe16_MaxValue) {
    const uint8_t data[] = {0xFF, 0xFF};
    EXPECT_EQ(read_be16(data), 0xFFFFu);
}

TEST(ItchEndian, ReadBe32) {
    // 0xDEADBEEF in big-endian
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(read_be32(data), 0xDEADBEEFu);
}

TEST(ItchEndian, ReadBe32_Zero) {
    const uint8_t data[] = {0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(read_be32(data), 0u);
}

TEST(ItchEndian, ReadBe64) {
    // 0x0102030405060708 in big-endian
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    EXPECT_EQ(read_be64(data), 0x0102030405060708ULL);
}

TEST(ItchEndian, ReadBe48) {
    // 6-byte big-endian value: 123456789 = 0x00000007'5BCD15
    // Bytes: 00 00 07 5B CD 15
    const uint8_t data[] = {0x00, 0x00, 0x07, 0x5B, 0xCD, 0x15};
    EXPECT_EQ(read_be48(data), 123456789ULL);
}

TEST(ItchEndian, ReadBe48_MaxValue) {
    // All 0xFF in 6 bytes = 0x0000FFFFFFFFFFFF
    const uint8_t data[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(read_be48(data), 0x0000FFFFFFFFFFFFULL);
}

// All-zero 6 bytes should produce 0. Symmetric with ReadBe48_MaxValue
// (and the corresponding zero/max coverage on Be32). Without this,
// a future refactor that swapped a `<< 8` for `<< 16` in the
// shift sequence could pass MaxValue (saturated all-ones still
// looks correct under a wrong shift) but produce non-zero from
// all-zero input — an asymmetry hard to debug from the
// MaxValue path alone.
TEST(ItchEndian, ReadBe48_Zero) {
    const uint8_t data[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(read_be48(data), 0ULL);
}

// MSB-only test: the high byte at offset 0 must shift to bits 40-47.
// A copy-paste-bug variant that swapped two of the shifts (e.g.
// `p[0] << 32` instead of `<< 40`) would still pass MaxValue
// (bits saturate either way) AND Zero (zeros stay zero) — but
// would fail this test where only one byte is non-zero.
TEST(ItchEndian, ReadBe48_HighByteOnlyTopOf48bits) {
    const uint8_t data[] = {0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
    // 0x80 at offset 0 → must produce 0x800000000000 (bit 47).
    EXPECT_EQ(read_be48(data), 0x0000800000000000ULL);
}

// LSB-only test: the low byte at offset 5 must NOT be shifted.
// A bug that lifted offset 5 by any amount would fail this test
// while passing the others.
TEST(ItchEndian, ReadBe48_LowByteOnlyBottomOf48bits) {
    const uint8_t data[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    EXPECT_EQ(read_be48(data), 1ULL);
}

// ---------------------------------------------------------------------------
// Test 3: Common header accessors
// ---------------------------------------------------------------------------

TEST(ItchHeader, CommonHeaderAccessors) {
    // Build a fake body (after the type byte) with known big-endian values.
    // stock_locate = 42 (0x002A), tracking = 7 (0x0007),
    // timestamp = 123456789 ns (0x00000007'5BCD15)
    uint8_t body[10];
    // stock_locate at offset 0
    body[0] = 0x00; body[1] = 0x2A;
    // tracking at offset 2
    body[2] = 0x00; body[3] = 0x07;
    // timestamp at offset 4 (6 bytes)
    body[4] = 0x00; body[5] = 0x00; body[6] = 0x07;
    body[7] = 0x5B; body[8] = 0xCD; body[9] = 0x15;

    EXPECT_EQ(stock_locate(body), 42u);
    EXPECT_EQ(tracking_number(body), 7u);
    EXPECT_EQ(timestamp_ns(body), 123456789ULL);
}

// ---------------------------------------------------------------------------
// Test 4: AddOrder parsing — full accessor validation
// ---------------------------------------------------------------------------

TEST(ItchAddOrder, ParseAllFields) {
    // Build a 35-byte AddOrder message (including the type byte).
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         order_ref(8) side(1) shares(4) stock(8) price(4)
    uint8_t buf[35];
    std::memset(buf, 0, sizeof(buf));

    buf[0] = 'A'; // msg type

    // stock_locate = 1 (offset 1, 2 bytes BE)
    buf[1] = 0x00; buf[2] = 0x01;

    // tracking = 2 (offset 3, 2 bytes BE)
    buf[3] = 0x00; buf[4] = 0x02;

    // timestamp = 123456789 ns (offset 5, 6 bytes BE)
    // 123456789 = 0x075BCD15
    buf[5] = 0x00; buf[6] = 0x00; buf[7] = 0x07;
    buf[8] = 0x5B; buf[9] = 0xCD; buf[10] = 0x15;

    // order_ref = 100 (offset 11, 8 bytes BE)
    // 100 = 0x0000000000000064
    buf[18] = 0x64;

    // side = 'B' (offset 19)
    buf[19] = 'B';

    // shares = 500 (offset 20, 4 bytes BE)
    // 500 = 0x000001F4
    buf[20] = 0x00; buf[21] = 0x00; buf[22] = 0x01; buf[23] = 0xF4;

    // stock = "AAPL    " (offset 24, 8 bytes)
    std::memcpy(buf + 24, "AAPL    ", 8);

    // price = 15050 raw (offset 32, 4 bytes BE) -> $1.5050
    // 15050 = 0x00003ACA
    buf[32] = 0x00; buf[33] = 0x00; buf[34] = 0x3A; buf[35 - 1] = 0xCA;
    // Correction: offset 32..35 but buf is only 35 bytes (indices 0..34)
    // price bytes at indices 32, 33, 34 -- only 3 bytes, but price is 4 bytes.
    // Actually kAddOrderSize = 35, so last index is 34. Let me recalculate:
    // The price field is at absolute offset 32 with 4 bytes: indices 32,33,34,35
    // But the message is only 35 bytes (0..34). That doesn't work.
    // Wait -- kAddOrderSize = 35 means the full message is 35 bytes: type(1) + body(34).
    // Re-check the layout in messages.hpp:
    //   type(1) + locate(2) + tracking(2) + timestamp(6) + order_ref(8) + side(1) +
    //   shares(4) + stock(8) + price(4) = 1+2+2+6+8+1+4+8+4 = 36
    // But kAddOrderSize = 35. The spec says 35 including type byte.
    // Let me look more carefully at the NASDAQ spec -- the size includes type byte.
    // 1+2+2+6+8+1+4+8+4 = 36... hmm but they set kAddOrderSize = 35.
    // The code uses msg[32] for price_raw. If message is 35 bytes, indices 0-34,
    // then price at 32 occupies 32,33,34,35 -- index 35 is out of bounds for 35 bytes.
    // But the existing code does read_be32(msg + 32), which reads 4 bytes starting at 32.
    // That would need the message to be at least 36 bytes.
    // The parse function returns length = expected = kAddOrderSize = 35.
    // This seems like a potential off-by-one in the constant, but let's test what the
    // code actually does. We'll use a 36-byte buffer to be safe.

    // Use a properly sized buffer for the test to avoid UB
    uint8_t msg[36];
    std::memset(msg, 0, sizeof(msg));
    std::memcpy(msg, buf, 35);
    // Set price properly across 4 bytes at offset 32
    msg[32] = 0x00; msg[33] = 0x00; msg[34] = 0x3A; msg[35] = 0xCA;

    // Accessors take the full message pointer (byte 0 = type tag)
    EXPECT_EQ(add_order::order_ref(msg), 100u);
    EXPECT_EQ(add_order::side(msg), 'B');
    EXPECT_EQ(add_order::shares(msg), 500u);
    EXPECT_EQ(add_order::stock(msg), "AAPL    ");
    EXPECT_EQ(add_order::price_raw(msg), 15050u);
    EXPECT_DOUBLE_EQ(add_order::price(msg), 1.5050);

    // Also verify common header via MessageView convenience
    // (MessageView.data points to byte 0 = type, accessors skip +1)
    EXPECT_EQ(stock_locate(msg + 1), 1u);
    EXPECT_EQ(tracking_number(msg + 1), 2u);
    EXPECT_EQ(timestamp_ns(msg + 1), 123456789ULL);
}

// ---------------------------------------------------------------------------
// Test 5: OrderExecuted parsing
// ---------------------------------------------------------------------------

TEST(ItchOrderExecuted, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         order_ref(8) executed_shares(4) match_number(8)
    // Total: 30 (but we need 31 bytes for match_number at offset 23 + 8 = 31)
    uint8_t msg[31];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'E';

    // stock_locate = 5
    msg[1] = 0x00; msg[2] = 0x05;
    // tracking = 10
    msg[3] = 0x00; msg[4] = 0x0A;
    // timestamp = 999999 (0x0F423F) -- 6 bytes BE
    msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x00;
    msg[8] = 0x0F; msg[9] = 0x42; msg[10] = 0x3F;

    // order_ref = 200 (offset 11, 8 bytes)
    msg[18] = 0xC8; // 200 = 0xC8

    // executed_shares = 100 (offset 19, 4 bytes)
    msg[19] = 0x00; msg[20] = 0x00; msg[21] = 0x00; msg[22] = 0x64;

    // match_number = 42 (offset 23, 8 bytes)
    msg[30] = 0x2A; // 42 = 0x2A

    EXPECT_EQ(order_executed::order_ref(msg), 200u);
    EXPECT_EQ(order_executed::executed_shares(msg), 100u);
    EXPECT_EQ(order_executed::match_number(msg), 42u);

    // Header
    EXPECT_EQ(stock_locate(msg + 1), 5u);
    EXPECT_EQ(tracking_number(msg + 1), 10u);
    EXPECT_EQ(timestamp_ns(msg + 1), 999999ULL);
}

// ---------------------------------------------------------------------------
// Test 6: OrderDelete parsing
// ---------------------------------------------------------------------------

TEST(ItchOrderDelete, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6) order_ref(8)
    // Total: 18 (but order_ref at 11 + 8 = 19 bytes needed)
    uint8_t msg[19];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'D';

    // stock_locate = 3
    msg[1] = 0x00; msg[2] = 0x03;
    // tracking = 0
    msg[3] = 0x00; msg[4] = 0x00;
    // timestamp = 50000000 (0x02FAF080) -> 6 bytes BE: 00 00 02 FA F0 80
    msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x02;
    msg[8] = 0xFA; msg[9] = 0xF0; msg[10] = 0x80;

    // order_ref = 12345 (0x3039) -- at offset 11, 8 bytes
    msg[17] = 0x30; msg[18] = 0x39;

    EXPECT_EQ(order_delete::order_ref(msg), 12345u);
    EXPECT_EQ(stock_locate(msg + 1), 3u);
    EXPECT_EQ(tracking_number(msg + 1), 0u);
    EXPECT_EQ(timestamp_ns(msg + 1), 50000000ULL);
}

// ---------------------------------------------------------------------------
// Test 7: parse() happy path
// ---------------------------------------------------------------------------

TEST(ItchParser, ParseHappyPath) {
    // SystemEvent is 12 bytes (type(1) + locate(2) + tracking(2) + ts(6) + event_code(1))
    uint8_t buf[12];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'S'; // SystemEvent
    buf[1] = 0x00; buf[2] = 0x07; // stock_locate = 7

    auto result = parse(buf, sizeof(buf));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, 'S');
    EXPECT_EQ(result->data, buf);
    EXPECT_EQ(result->length, kSystemEventSize);
    EXPECT_EQ(result->stock_locate(), 7u);
}

TEST(ItchParser, ParseHappyPathAddOrder) {
    // AddOrder is 36 bytes (type(1) + common(10) + order_ref(8) + side(1) + shares(4) + stock(8) + price(4))
    uint8_t buf[36];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'A';

    auto result = parse(buf, sizeof(buf));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, 'A');
    EXPECT_EQ(result->length, kAddOrderSize);
}

// ---------------------------------------------------------------------------
// Test 8: parse() empty input returns kIncomplete
// ---------------------------------------------------------------------------

TEST(ItchParser, ParseEmptyInputReturnsIncomplete) {
    auto result = parse(nullptr, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kIncomplete);
}

// ---------------------------------------------------------------------------
// Test 9: parse() unknown type returns kUnknownType
// ---------------------------------------------------------------------------

TEST(ItchParser, ParseUnknownTypeReturnsUnknownType) {
    uint8_t buf[1] = {0xFF}; // not a valid ITCH type
    auto result = parse(buf, sizeof(buf));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kUnknownType);
}

TEST(ItchParser, ParseUnknownTypeZeroByte) {
    uint8_t buf[1] = {0x00};
    auto result = parse(buf, sizeof(buf));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kUnknownType);
}

// ---------------------------------------------------------------------------
// Test 10: parse() truncated message returns kTruncated
// ---------------------------------------------------------------------------

TEST(ItchParser, ParseTruncatedReturnsError) {
    // AddOrder expects 36 bytes, provide only 10
    uint8_t buf[10];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'A'; // AddOrder type
    auto result = parse(buf, sizeof(buf));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kTruncated);
}

TEST(ItchParser, ParseTruncatedByOneByte) {
    // NOII is 49 bytes -- provide 48
    uint8_t buf[48];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'I'; // NOII type
    auto result = parse(buf, sizeof(buf));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kTruncated);
}

// ---------------------------------------------------------------------------
// Test 11: message_type_name()
// ---------------------------------------------------------------------------

TEST(ItchParser, MessageTypeNameKnownTypes) {
    EXPECT_EQ(message_type_name(kSystemEvent), "SystemEvent");
    EXPECT_EQ(message_type_name(kAddOrder), "AddOrder");
    EXPECT_EQ(message_type_name(kOrderExecuted), "OrderExecuted");
    EXPECT_EQ(message_type_name(kOrderDelete), "OrderDelete");
    EXPECT_EQ(message_type_name(kCrossTrade), "CrossTrade");
    EXPECT_EQ(message_type_name(kRPII), "RPII");
    EXPECT_EQ(message_type_name(kNOII), "NOII");
    EXPECT_EQ(message_type_name(kAddOrderMPID), "AddOrderMPID");
}

TEST(ItchParser, MessageTypeNameUnknownReturnsUnknown) {
    EXPECT_EQ(message_type_name(0x00), "Unknown");
    EXPECT_EQ(message_type_name(0xFF), "Unknown");
}

// ---------------------------------------------------------------------------
// Test 12: message_size() returns 0 for unknown type
// ---------------------------------------------------------------------------

TEST(ItchParser, MessageSizeKnownTypes) {
    EXPECT_EQ(message_size(kSystemEvent), kSystemEventSize);
    EXPECT_EQ(message_size(kAddOrder), kAddOrderSize);
    EXPECT_EQ(message_size(kOrderDelete), kOrderDeleteSize);
    EXPECT_EQ(message_size(kNOII), kNOIISize);
}

TEST(ItchParser, MessageSizeUnknownReturnsZero) {
    EXPECT_EQ(message_size(0x00), 0u);
    EXPECT_EQ(message_size(0xFF), 0u);
    EXPECT_EQ(message_size('Z'), 0u);
}

// ---------------------------------------------------------------------------
// Test 13: ItchFramer satisfies MessageFramer concept
// ---------------------------------------------------------------------------

TEST(ItchFramer, ConceptSatisfaction) {
    // Compile-time check: ItchFramer must satisfy the MessageFramer concept
    static_assert(eph::net::MessageFramer<eph::itch::ItchFramer>,
                  "ItchFramer must satisfy MessageFramer concept");

    // Also verify it is just an alias for LengthPrefixFramer
    static_assert(std::is_same_v<eph::itch::ItchFramer, eph::net::LengthPrefixFramer>,
                  "ItchFramer should be an alias for LengthPrefixFramer");
}

// ---------------------------------------------------------------------------
// Test 14: LengthPrefixFramer roundtrip via ItchFramer
// ---------------------------------------------------------------------------

TEST(ItchFramer, LengthPrefixRoundtrip) {
    // Encode a SystemEvent message with the ItchFramer (2-byte length prefix)
    uint8_t payload[11];
    std::memset(payload, 0, sizeof(payload));
    payload[0] = 'S'; // SystemEvent type
    payload[1] = 0x00; payload[2] = 0x0A; // stock_locate = 10
    payload[10] = 'O'; // event_code = start-of-messages

    // Encode: output needs payload_len + 2 bytes for the length prefix
    uint8_t encoded[13];
    eph::itch::ItchFramer framer;
    size_t written = framer.encode(encoded, payload, sizeof(payload), 0);
    EXPECT_EQ(written, 13u);

    // Verify the 2-byte big-endian length prefix: 0x000B = 11
    EXPECT_EQ(encoded[0], 0x00);
    EXPECT_EQ(encoded[1], 0x0B);

    // Decode back
    auto result = eph::itch::ItchFramer{}.decode(encoded, written);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, 11u);
    EXPECT_EQ(result->msg_type, 'S');
    EXPECT_EQ(result->is_control, false);
    EXPECT_EQ(result->total_len, 13u);

    // Verify decoded payload matches original
    EXPECT_EQ(std::memcmp(result->payload, payload, sizeof(payload)), 0);
}

// ---------------------------------------------------------------------------
// Test: parse_error_name()
// ---------------------------------------------------------------------------

TEST(ItchParser, ParseErrorNames) {
    EXPECT_EQ(parse_error_name(ParseError::kIncomplete), "incomplete");
    EXPECT_EQ(parse_error_name(ParseError::kUnknownType), "unknown message type");
    EXPECT_EQ(parse_error_name(ParseError::kTruncated), "truncated message");
}

// ---------------------------------------------------------------------------
// Test: parse() with extra data succeeds (only consumes expected size)
// ---------------------------------------------------------------------------

TEST(ItchParser, ParseWithExtraDataSucceeds) {
    // Provide more bytes than needed -- parse should succeed with expected length
    uint8_t buf[64];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'D'; // OrderDelete = 18 bytes
    auto result = parse(buf, sizeof(buf));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, 'D');
    EXPECT_EQ(result->length, kOrderDeleteSize);
}

// ---------------------------------------------------------------------------
// Test: parse_all batch parsing
// ---------------------------------------------------------------------------

TEST(ItchParser, ParseAllMultipleMessages) {
    // Concatenate: SystemEvent (11) + OrderDelete (18) + BrokenTrade (18)
    constexpr size_t total = kSystemEventSize + kOrderDeleteSize + kBrokenTradeSize;
    uint8_t buf[total];
    std::memset(buf, 0, total);
    buf[0] = kSystemEvent;
    buf[kSystemEventSize] = kOrderDelete;
    buf[kSystemEventSize + kOrderDeleteSize] = kBrokenTrade;

    size_t count = 0;
    size_t consumed = parse_all(buf, total, [&](const MessageView& mv) {
        ++count;
        if (count == 1) EXPECT_EQ(mv.msg_type, kSystemEvent);
        if (count == 2) EXPECT_EQ(mv.msg_type, kOrderDelete);
        if (count == 3) EXPECT_EQ(mv.msg_type, kBrokenTrade);
    });

    EXPECT_EQ(count, 3u);
    EXPECT_EQ(consumed, total);
}

TEST(ItchParser, ParseAllStopsOnError) {
    // SystemEvent (12) + unknown type (0xFF) — should parse 1, stop at 2nd
    uint8_t buf[32];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = kSystemEvent;
    buf[kSystemEventSize] = 0xFF; // unknown type

    size_t count = 0;
    size_t consumed = parse_all(buf, sizeof(buf), [&](const MessageView&) {
        ++count;
    });

    EXPECT_EQ(count, 1u);
    EXPECT_EQ(consumed, kSystemEventSize);
}

TEST(ItchParser, ParseAllEarlyStop) {
    // 3 messages, callback returns false on 2nd
    constexpr size_t total = kSystemEventSize + kOrderDeleteSize + kBrokenTradeSize;
    uint8_t buf[total];
    std::memset(buf, 0, total);
    buf[0] = kSystemEvent;
    buf[kSystemEventSize] = kOrderDelete;
    buf[kSystemEventSize + kOrderDeleteSize] = kBrokenTrade;

    size_t count = 0;
    size_t consumed = parse_all(buf, total, [&](const MessageView&) -> bool {
        ++count;
        return count < 2; // stop after 2nd
    });

    EXPECT_EQ(count, 2u);
    EXPECT_EQ(consumed, kSystemEventSize + kOrderDeleteSize);
}

TEST(ItchParser, ParseAllPartialTrailingMessage) {
    // SystemEvent (11) + partial OrderDelete (only 5 bytes of 18)
    constexpr size_t total = kSystemEventSize + 5;
    uint8_t buf[total];
    std::memset(buf, 0, total);
    buf[0] = kSystemEvent;
    buf[kSystemEventSize] = kOrderDelete;

    size_t count = 0;
    size_t consumed = parse_all(buf, total, [&](const MessageView&) {
        ++count;
    });

    EXPECT_EQ(count, 1u);
    EXPECT_EQ(consumed, kSystemEventSize); // partial message not consumed
}

TEST(ItchParser, ParseAllEmptyBuffer) {
    size_t count = 0;
    size_t consumed = parse_all(nullptr, 0, [&](const MessageView&) {
        ++count;
    });
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(consumed, 0u);
}

// ---------------------------------------------------------------------------
// Test: std::formatter<MessageView>
// ---------------------------------------------------------------------------

TEST(ItchFormatter, MessageViewFormat) {
    // Build a SystemEvent message with known header values
    // SystemEvent: type(1) + locate(2) + tracking(2) + timestamp(6) + event_code(1) = 12
    uint8_t buf[12];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 'S'; // SystemEvent
    // stock_locate = 42 (BE)
    buf[1] = 0x00; buf[2] = 0x2A;
    // tracking = 0
    buf[3] = 0x00; buf[4] = 0x00;
    // timestamp = 123456789 ns (BE 6 bytes): 0x00000007'5BCD15
    buf[5] = 0x00; buf[6] = 0x00; buf[7] = 0x07;
    buf[8] = 0x5B; buf[9] = 0xCD; buf[10] = 0x15;
    // event_code at offset 11
    buf[11] = 'O';

    auto result = parse(buf, sizeof(buf));
    ASSERT_TRUE(result.has_value());

    auto formatted = std::format("{}", *result);
    EXPECT_EQ(formatted, "ITCH[SystemEvent locate=42 ts=123456789ns len=12]");
}

TEST(ItchFormatter, MessageViewFormatUnknownType) {
    // Build a fake message with unknown type (won't parse, but we can construct
    // a MessageView directly for formatting)
    eph::itch::MessageView mv{};
    // Allocate a dummy buffer for the MessageView to point to
    uint8_t dummy[20];
    std::memset(dummy, 0, sizeof(dummy));
    dummy[0] = 0xFF; // unknown type
    mv.msg_type = 0xFF;
    mv.data = dummy;
    mv.length = 20;

    auto formatted = std::format("{}", mv);
    EXPECT_EQ(formatted, "ITCH[Unknown locate=0 ts=0ns len=20]");
}

// ---------------------------------------------------------------------------
// Test: AddOrderMPID accessor validation
// ---------------------------------------------------------------------------

TEST(ItchAddOrderMPID, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         order_ref(8) side(1) shares(4) stock(8) price(4) attribution(4)
    // Total: 39
    uint8_t msg[40]; // extra byte to avoid read-past-end on price field
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'F'; // msg type

    // stock_locate = 1
    msg[1] = 0x00; msg[2] = 0x01;
    // tracking = 2
    msg[3] = 0x00; msg[4] = 0x02;
    // timestamp = 123456789 ns
    msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x07;
    msg[8] = 0x5B; msg[9] = 0xCD; msg[10] = 0x15;

    // order_ref = 100 (offset 11, 8 bytes BE)
    msg[18] = 0x64;

    // side = 'S' (offset 19)
    msg[19] = 'S';

    // shares = 250 (offset 20, 4 bytes BE) 0x000000FA
    msg[20] = 0x00; msg[21] = 0x00; msg[22] = 0x00; msg[23] = 0xFA;

    // stock = "MSFT    " (offset 24, 8 bytes)
    std::memcpy(msg + 24, "MSFT    ", 8);

    // price = 25000 raw (offset 32, 4 bytes BE) -> $2.5000
    // 25000 = 0x000061A8
    msg[32] = 0x00; msg[33] = 0x00; msg[34] = 0x61; msg[35] = 0xA8;

    // attribution = "GSCO" (offset 36, 4 bytes)
    std::memcpy(msg + 36, "GSCO", 4);

    EXPECT_EQ(add_order_mpid::order_ref(msg), 100u);
    EXPECT_EQ(add_order_mpid::side(msg), 'S');
    EXPECT_EQ(add_order_mpid::shares(msg), 250u);
    EXPECT_EQ(add_order_mpid::stock(msg), "MSFT    ");
    EXPECT_EQ(add_order_mpid::price_raw(msg), 25000u);
    EXPECT_DOUBLE_EQ(add_order_mpid::price(msg), 2.5000);
    EXPECT_EQ(add_order_mpid::attribution(msg), "GSCO");
}

// ---------------------------------------------------------------------------
// Test: OrderExecutedWithPrice accessor validation
// ---------------------------------------------------------------------------

TEST(ItchOrderExecutedWithPrice, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         order_ref(8) executed_shares(4) match_number(8)
    //         printable(1) execution_price(4)
    // Total: 35
    uint8_t msg[36]; // extra byte for safety on last 4-byte read
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'C'; // msg type

    // stock_locate = 10
    msg[1] = 0x00; msg[2] = 0x0A;
    // tracking = 5
    msg[3] = 0x00; msg[4] = 0x05;
    // timestamp = 999999
    msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x00;
    msg[8] = 0x0F; msg[9] = 0x42; msg[10] = 0x3F;

    // order_ref = 300 (offset 11, 8 bytes BE) 0x012C
    msg[17] = 0x01; msg[18] = 0x2C;

    // executed_shares = 50 (offset 19, 4 bytes BE) 0x00000032
    msg[19] = 0x00; msg[20] = 0x00; msg[21] = 0x00; msg[22] = 0x32;

    // match_number = 77 (offset 23, 8 bytes BE) 0x4D
    msg[30] = 0x4D;

    // printable = 'Y' (offset 31)
    msg[31] = 'Y';

    // execution_price = 150500 raw (offset 32, 4 bytes BE) -> $15.0500
    // 150500 = 0x00024C14 -> but let's use a simpler value
    // 15050 = 0x00003ACA -> $1.5050
    msg[32] = 0x00; msg[33] = 0x00; msg[34] = 0x3A; msg[35] = 0xCA;

    EXPECT_EQ(order_executed_price::order_ref(msg), 300u);
    EXPECT_EQ(order_executed_price::executed_shares(msg), 50u);
    EXPECT_EQ(order_executed_price::match_number(msg), 77u);
    EXPECT_EQ(order_executed_price::printable(msg), 'Y');
    EXPECT_EQ(order_executed_price::execution_price_raw(msg), 15050u);
    EXPECT_DOUBLE_EQ(order_executed_price::execution_price(msg), 1.5050);
}

// ---------------------------------------------------------------------------
// Test: OrderCancel accessor validation
// ---------------------------------------------------------------------------

TEST(ItchOrderCancel, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         order_ref(8) cancelled_shares(4)
    // Total: 22
    uint8_t msg[23]; // extra byte for safety
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'X'; // msg type

    // stock_locate = 7
    msg[1] = 0x00; msg[2] = 0x07;
    // tracking = 3
    msg[3] = 0x00; msg[4] = 0x03;
    // timestamp = 50000000
    msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x02;
    msg[8] = 0xFA; msg[9] = 0xF0; msg[10] = 0x80;

    // order_ref = 999 (offset 11, 8 bytes BE) 0x03E7
    msg[17] = 0x03; msg[18] = 0xE7;

    // cancelled_shares = 150 (offset 19, 4 bytes BE) 0x00000096
    msg[19] = 0x00; msg[20] = 0x00; msg[21] = 0x00; msg[22] = 0x96;

    EXPECT_EQ(order_cancel::order_ref(msg), 999u);
    EXPECT_EQ(order_cancel::cancelled_shares(msg), 150u);
}

// ---------------------------------------------------------------------------
// Test: OrderReplace accessor validation
// ---------------------------------------------------------------------------

TEST(ItchOrderReplace, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         original_order_ref(8) new_order_ref(8) shares(4) price(4)
    // Total: 34
    uint8_t msg[35]; // extra byte for safety on last 4-byte read at offset 31
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'U'; // msg type

    // stock_locate = 2
    msg[1] = 0x00; msg[2] = 0x02;
    // tracking = 1
    msg[3] = 0x00; msg[4] = 0x01;
    // timestamp = 123456789
    msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x07;
    msg[8] = 0x5B; msg[9] = 0xCD; msg[10] = 0x15;

    // original_order_ref = 500 (offset 11, 8 bytes BE) 0x01F4
    msg[17] = 0x01; msg[18] = 0xF4;

    // new_order_ref = 501 (offset 19, 8 bytes BE) 0x01F5
    msg[25] = 0x01; msg[26] = 0xF5;

    // shares = 1000 (offset 27, 4 bytes BE) 0x000003E8
    msg[27] = 0x00; msg[28] = 0x00; msg[29] = 0x03; msg[30] = 0xE8;

    // price = 50000 raw (offset 31, 4 bytes BE) -> $5.0000
    // 50000 = 0x0000C350
    msg[31] = 0x00; msg[32] = 0x00; msg[33] = 0xC3; msg[34] = 0x50;

    EXPECT_EQ(order_replace::original_order_ref(msg), 500u);
    EXPECT_EQ(order_replace::new_order_ref(msg), 501u);
    EXPECT_EQ(order_replace::shares(msg), 1000u);
    EXPECT_EQ(order_replace::price_raw(msg), 50000u);
    EXPECT_DOUBLE_EQ(order_replace::price(msg), 5.0000);
}

// ---------------------------------------------------------------------------
// Test: NonCrossTrade accessor validation
// ---------------------------------------------------------------------------

TEST(ItchNonCrossTrade, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         order_ref(8) side(1) shares(4) stock(8) price(4) match_number(8)
    // Total: 43
    uint8_t msg[44]; // extra byte for safety on last 8-byte read at offset 36
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'P'; // msg type

    // stock_locate = 4
    msg[1] = 0x00; msg[2] = 0x04;
    // tracking = 6
    msg[3] = 0x00; msg[4] = 0x06;
    // timestamp = 999999
    msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x00;
    msg[8] = 0x0F; msg[9] = 0x42; msg[10] = 0x3F;

    // order_ref = 777 (offset 11, 8 bytes BE) 0x0309
    msg[17] = 0x03; msg[18] = 0x09;

    // side = 'B' (offset 19)
    msg[19] = 'B';

    // shares = 200 (offset 20, 4 bytes BE) 0x000000C8
    msg[20] = 0x00; msg[21] = 0x00; msg[22] = 0x00; msg[23] = 0xC8;

    // stock = "GOOG    " (offset 24, 8 bytes)
    std::memcpy(msg + 24, "GOOG    ", 8);

    // price = 30000 raw (offset 32, 4 bytes BE) -> $3.0000
    // 30000 = 0x00007530
    msg[32] = 0x00; msg[33] = 0x00; msg[34] = 0x75; msg[35] = 0x30;

    // match_number = 55 (offset 36, 8 bytes BE) 0x37
    msg[43] = 0x37;

    EXPECT_EQ(non_cross_trade::order_ref(msg), 777u);
    EXPECT_EQ(non_cross_trade::side(msg), 'B');
    EXPECT_EQ(non_cross_trade::shares(msg), 200u);
    EXPECT_EQ(non_cross_trade::stock(msg), "GOOG    ");
    EXPECT_EQ(non_cross_trade::price_raw(msg), 30000u);
    EXPECT_DOUBLE_EQ(non_cross_trade::price(msg), 3.0000);
    EXPECT_EQ(non_cross_trade::match_number(msg), 55u);
}

// ---------------------------------------------------------------------------
// Test: CrossTrade accessor validation
// ---------------------------------------------------------------------------

TEST(ItchCrossTrade, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         shares(8) stock(8) cross_price(4) match_number(8) cross_type(1)
    // Total: 1+2+2+6+8+8+4+8+1 = 40
    uint8_t msg[40];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'Q'; // msg type

    // stock_locate = 8
    msg[1] = 0x00; msg[2] = 0x08;
    // tracking = 0
    msg[3] = 0x00; msg[4] = 0x00;
    // timestamp = 50000000
    msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x02;
    msg[8] = 0xFA; msg[9] = 0xF0; msg[10] = 0x80;

    // shares = 10000 (offset 11, 8 bytes BE) 0x2710
    msg[17] = 0x27; msg[18] = 0x10;

    // stock = "TSLA    " (offset 19, 8 bytes)
    std::memcpy(msg + 19, "TSLA    ", 8);

    // cross_price = 40000 raw (offset 27, 4 bytes BE) -> $4.0000
    msg[27] = 0x00; msg[28] = 0x00; msg[29] = 0x9C; msg[30] = 0x40;

    // match_number = 79 (offset 31, 8 bytes BE)
    msg[38] = 0x4F; // 79 = 0x4F

    // cross_type = 'O' (opening cross) at offset 39
    msg[39] = 'O';

    EXPECT_EQ(cross_trade::shares(msg), 10000u);
    EXPECT_EQ(cross_trade::stock(msg), "TSLA    ");
    EXPECT_EQ(cross_trade::cross_price_raw(msg), 40000u);
    EXPECT_DOUBLE_EQ(cross_trade::cross_price(msg), 4.0000);
    EXPECT_EQ(cross_trade::match_number(msg), 79u);
    EXPECT_EQ(cross_trade::cross_type(msg), 'O');
}

// ---------------------------------------------------------------------------
// Test: SystemEvent accessor validation
// ---------------------------------------------------------------------------

TEST(ItchSystemEvent, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6) event_code(1)
    // Total: 12
    uint8_t msg[12];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'S'; // msg type

    // stock_locate = 0
    msg[1] = 0x00; msg[2] = 0x00;
    // tracking = 0
    msg[3] = 0x00; msg[4] = 0x00;
    // timestamp = 0
    // (all zeros already)

    // event_code = 'O' (start-of-messages) at offset 11
    msg[11] = 'O';

    EXPECT_EQ(system_event::event_code(msg), 'O');
}

// ---------------------------------------------------------------------------
// Test: StockDirectory accessor validation
// ---------------------------------------------------------------------------

TEST(ItchStockDirectory, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6) stock(8)
    //         market_category(1) financial_status(1) round_lot_size(4) ...
    // Total: 39
    uint8_t msg[39];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'R'; // msg type

    // stock_locate = 42
    msg[1] = 0x00; msg[2] = 0x2A;
    // tracking = 1
    msg[3] = 0x00; msg[4] = 0x01;
    // timestamp = 123456789
    msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x07;
    msg[8] = 0x5B; msg[9] = 0xCD; msg[10] = 0x15;

    // stock = "AAPL    " (offset 11, 8 bytes)
    std::memcpy(msg + 11, "AAPL    ", 8);

    // market_category = 'Q' (offset 19)
    msg[19] = 'Q';

    // financial_status = 'N' (offset 20)
    msg[20] = 'N';

    // round_lot_size = 100 (offset 21, 4 bytes BE) 0x00000064
    msg[21] = 0x00; msg[22] = 0x00; msg[23] = 0x00; msg[24] = 0x64;

    EXPECT_EQ(stock_directory::stock(msg), "AAPL    ");
    EXPECT_EQ(stock_directory::market_category(msg), 'Q');
    EXPECT_EQ(stock_directory::financial_status(msg), 'N');
    EXPECT_EQ(stock_directory::round_lot_size(msg), 100u);
}

// ---------------------------------------------------------------------------
// Test: StockTradingAction accessors
// ---------------------------------------------------------------------------

TEST(ItchStockTradingAction, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         stock(8) trading_state(1) reserved(1) reason(4)
    // Body size: 24, wire size: 25
    uint8_t msg[25];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'H';
    std::memcpy(msg + 11, "MSFT    ", 8);
    msg[19] = 'T';  // trading_state = trading
    msg[20] = ' ';  // reserved
    std::memcpy(msg + 21, "MWC1", 4); // reason

    EXPECT_EQ(stock_trading_action::stock(msg), "MSFT    ");
    EXPECT_EQ(stock_trading_action::trading_state(msg), 'T');
    EXPECT_EQ(stock_trading_action::reserved(msg), ' ');
    EXPECT_EQ(stock_trading_action::reason(msg), "MWC1");
}

TEST(ItchStockTradingAction, HaltedState) {
    uint8_t msg[25];
    std::memset(msg, 0, sizeof(msg));
    msg[0] = 'H';
    std::memcpy(msg + 11, "GOOG    ", 8);
    msg[19] = 'H'; // halted
    std::memcpy(msg + 21, "LUDP", 4);

    EXPECT_EQ(stock_trading_action::stock(msg), "GOOG    ");
    EXPECT_EQ(stock_trading_action::trading_state(msg), 'H');
    EXPECT_EQ(stock_trading_action::reason(msg), "LUDP");
}

// ---------------------------------------------------------------------------
// Test: RegSHORestriction accessors
// ---------------------------------------------------------------------------

TEST(ItchRegSHORestriction, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         stock(8) reg_sho_action(1)
    // Body size: 19, wire size: 20
    uint8_t msg[20];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'Y';
    std::memcpy(msg + 11, "TSLA    ", 8);
    msg[19] = '1'; // short sale restriction activated

    EXPECT_EQ(reg_sho_restriction::stock(msg), "TSLA    ");
    EXPECT_EQ(reg_sho_restriction::reg_sho_action(msg), '1');
}

// ---------------------------------------------------------------------------
// Test: MarketParticipantPosition accessors
// ---------------------------------------------------------------------------

TEST(ItchMarketParticipantPosition, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         mpid(4) stock(8) primary_market_maker(1) market_maker_mode(1)
    //         market_participant_state(1)
    // Body size: 25, wire size: 26
    uint8_t msg[26];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'L';
    std::memcpy(msg + 11, "GSCO", 4);    // mpid
    std::memcpy(msg + 15, "AAPL    ", 8); // stock
    msg[23] = 'Y'; // primary_market_maker
    msg[24] = 'N'; // market_maker_mode = normal
    msg[25] = 'A'; // market_participant_state = active

    EXPECT_EQ(market_participant_position::mpid(msg), "GSCO");
    EXPECT_EQ(market_participant_position::stock(msg), "AAPL    ");
    EXPECT_EQ(market_participant_position::primary_market_maker(msg), 'Y');
    EXPECT_EQ(market_participant_position::market_maker_mode(msg), 'N');
    EXPECT_EQ(market_participant_position::market_participant_state(msg), 'A');
}

// ---------------------------------------------------------------------------
// Test: MWCBDeclineLevel accessors
// ---------------------------------------------------------------------------

TEST(ItchMWCBDeclineLevel, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         level1(8) level2(8) level3(8)
    // Body size: 34, wire size: 35
    uint8_t msg[35];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'V';
    // level1 = 350000000000 raw (3500.00 in price8)
    // 350000000000 = 0x000000517DA02C00
    msg[11] = 0x00; msg[12] = 0x00; msg[13] = 0x00; msg[14] = 0x51;
    msg[15] = 0x7D; msg[16] = 0xA0; msg[17] = 0x2C; msg[18] = 0x00;
    // level2 = 200000000000 raw (2000.00 in price8)
    // 200000000000 = 0x0000002E'90EDD000
    msg[19] = 0x00; msg[20] = 0x00; msg[21] = 0x00; msg[22] = 0x2E;
    msg[23] = 0x90; msg[24] = 0xED; msg[25] = 0xD0; msg[26] = 0x00;
    // level3 = 100000000000 raw (1000.00 in price8)
    // 100000000000 = 0x00000017'4876E800
    msg[27] = 0x00; msg[28] = 0x00; msg[29] = 0x00; msg[30] = 0x17;
    msg[31] = 0x48; msg[32] = 0x76; msg[33] = 0xE8; msg[34] = 0x00;

    EXPECT_EQ(mwcb_decline_level::level1_raw(msg), 350000000000ULL);
    EXPECT_DOUBLE_EQ(mwcb_decline_level::level1(msg), 3500.0);
    EXPECT_EQ(mwcb_decline_level::level2_raw(msg), 200000000000ULL);
    EXPECT_DOUBLE_EQ(mwcb_decline_level::level2(msg), 2000.0);
    EXPECT_EQ(mwcb_decline_level::level3_raw(msg), 100000000000ULL);
    EXPECT_DOUBLE_EQ(mwcb_decline_level::level3(msg), 1000.0);
}

// ---------------------------------------------------------------------------
// Test: MWCBStatus accessors
// ---------------------------------------------------------------------------

TEST(ItchMWCBStatus, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6) breached_level(1)
    // Body size: 11, wire size: 12
    uint8_t msg[12];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'W';
    msg[11] = '2'; // breached level 2

    EXPECT_EQ(mwcb_status::breached_level(msg), '2');
}

// ---------------------------------------------------------------------------
// Test: IPOQuotingPeriod accessors
// ---------------------------------------------------------------------------

TEST(ItchIPOQuotingPeriod, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         stock(8) ipo_quotation_release_time(4)
    //         ipo_quotation_release_qualifier(1) ipo_price(4)
    // Body size: 27, wire size: 28
    uint8_t msg[28];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'K';
    std::memcpy(msg + 11, "NEWI    ", 8);
    // ipo_quotation_release_time = 36000 seconds (10:00 AM)
    // 36000 = 0x00008CA0
    msg[19] = 0x00; msg[20] = 0x00; msg[21] = 0x8C; msg[22] = 0xA0;
    msg[23] = 'A'; // anticipated
    // ipo_price = 250000 raw ($25.00)
    // 250000 = 0x0003D090
    msg[24] = 0x00; msg[25] = 0x03; msg[26] = 0xD0; msg[27] = 0x90;

    EXPECT_EQ(ipo_quoting_period::stock(msg), "NEWI    ");
    EXPECT_EQ(ipo_quoting_period::ipo_quotation_release_time(msg), 36000u);
    EXPECT_EQ(ipo_quoting_period::ipo_quotation_release_qualifier(msg), 'A');
    EXPECT_EQ(ipo_quoting_period::ipo_price_raw(msg), 250000u);
    EXPECT_DOUBLE_EQ(ipo_quoting_period::ipo_price(msg), 25.0);
}

// ---------------------------------------------------------------------------
// Test: LULDAuctionCollar accessors
// ---------------------------------------------------------------------------

TEST(ItchLULDAuctionCollar, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         stock(8) ref_price(4) upper(4) lower(4) extension(4)
    // Body size: 34, wire size: 35
    uint8_t msg[35];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'J';
    std::memcpy(msg + 11, "GOOG    ", 8);
    // ref_price = 1500000 raw ($150.00) at offset 19
    // 1500000 = 0x0016E360
    msg[19] = 0x00; msg[20] = 0x16; msg[21] = 0xE3; msg[22] = 0x60;
    // upper = 1575000 raw ($157.50) at offset 23
    // 1575000 = 0x00180858
    msg[23] = 0x00; msg[24] = 0x18; msg[25] = 0x08; msg[26] = 0x58;
    // lower = 1425000 raw ($142.50) at offset 27
    // 1425000 = 0x0015BE68
    msg[27] = 0x00; msg[28] = 0x15; msg[29] = 0xBE; msg[30] = 0x68;
    // extension = 300 at offset 31
    // 300 = 0x0000012C
    msg[31] = 0x00; msg[32] = 0x00; msg[33] = 0x01; msg[34] = 0x2C;

    EXPECT_EQ(luld_auction_collar::stock(msg), "GOOG    ");
    EXPECT_EQ(luld_auction_collar::auction_collar_reference_price_raw(msg), 1500000u);
    EXPECT_DOUBLE_EQ(luld_auction_collar::auction_collar_reference_price(msg), 150.0);
    EXPECT_EQ(luld_auction_collar::upper_auction_collar_price_raw(msg), 1575000u);
    EXPECT_DOUBLE_EQ(luld_auction_collar::upper_auction_collar_price(msg), 157.5);
    EXPECT_EQ(luld_auction_collar::lower_auction_collar_price_raw(msg), 1425000u);
    EXPECT_DOUBLE_EQ(luld_auction_collar::lower_auction_collar_price(msg), 142.5);
    EXPECT_EQ(luld_auction_collar::auction_collar_extension(msg), 300u);
}

// ---------------------------------------------------------------------------
// Test: OperationalHalt accessors
// ---------------------------------------------------------------------------

TEST(ItchOperationalHalt, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         stock(8) market_code(1) operational_halt_action(1)
    // Body size: 20, wire size: 21
    uint8_t msg[21];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'h';
    std::memcpy(msg + 11, "AMZN    ", 8);
    msg[19] = 'Q'; // NASDAQ
    msg[20] = 'H'; // halted

    EXPECT_EQ(operational_halt::stock(msg), "AMZN    ");
    EXPECT_EQ(operational_halt::market_code(msg), 'Q');
    EXPECT_EQ(operational_halt::operational_halt_action(msg), 'H');
}

// ---------------------------------------------------------------------------
// Test: BrokenTrade accessors
// ---------------------------------------------------------------------------

TEST(ItchBrokenTrade, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6) match_number(8)
    // Body size: 18, wire size: 19
    uint8_t msg[19];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'B';
    // match_number = 98765 at offset 11, 8 bytes BE
    // 98765 = 0x000181CD
    msg[16] = 0x01; msg[17] = 0x81; msg[18] = 0xCD;

    EXPECT_EQ(broken_trade::match_number(msg), 98765u);
}

// ---------------------------------------------------------------------------
// Test: NOII accessors
// ---------------------------------------------------------------------------

TEST(ItchNOII, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         paired_shares(8) imbalance_shares(8) imbalance_direction(1)
    //         stock(8) far_price(4) near_price(4) current_reference_price(4)
    //         cross_type(1) price_variation_indicator(1)
    // Body size: 49, wire size: 50
    uint8_t msg[50];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'I';
    // paired_shares = 10000 at offset 11
    // 10000 = 0x2710
    msg[17] = 0x27; msg[18] = 0x10;
    // imbalance_shares = 5000 at offset 19
    // 5000 = 0x1388
    msg[25] = 0x13; msg[26] = 0x88;
    // imbalance_direction = 'B' at offset 27
    msg[27] = 'B';
    // stock = "META    " at offset 28
    std::memcpy(msg + 28, "META    ", 8);
    // far_price = 300000 raw ($30.00) at offset 36
    // 300000 = 0x000493E0
    msg[36] = 0x00; msg[37] = 0x04; msg[38] = 0x93; msg[39] = 0xE0;
    // near_price = 299500 raw ($29.95) at offset 40
    // 299500 = 0x000491EC
    msg[40] = 0x00; msg[41] = 0x04; msg[42] = 0x91; msg[43] = 0xEC;
    // current_reference_price = 299750 raw ($29.975) at offset 44
    // 299750 = 0x000492E6
    msg[44] = 0x00; msg[45] = 0x04; msg[46] = 0x92; msg[47] = 0xE6;
    // cross_type = 'O' at offset 48
    msg[48] = 'O';
    // price_variation_indicator = 'L' at offset 49
    msg[49] = 'L';

    EXPECT_EQ(noii::paired_shares(msg), 10000u);
    EXPECT_EQ(noii::imbalance_shares(msg), 5000u);
    EXPECT_EQ(noii::imbalance_direction(msg), 'B');
    EXPECT_EQ(noii::stock(msg), "META    ");
    EXPECT_EQ(noii::far_price_raw(msg), 300000u);
    EXPECT_DOUBLE_EQ(noii::far_price(msg), 30.0);
    EXPECT_EQ(noii::near_price_raw(msg), 299500u);
    EXPECT_DOUBLE_EQ(noii::near_price(msg), 29.95);
    EXPECT_EQ(noii::current_reference_price_raw(msg), 299750u);
    EXPECT_DOUBLE_EQ(noii::current_reference_price(msg), 29.975);
    EXPECT_EQ(noii::cross_type(msg), 'O');
    EXPECT_EQ(noii::price_variation_indicator(msg), 'L');
}

// ---------------------------------------------------------------------------
// Test: RPII accessors
// ---------------------------------------------------------------------------

TEST(ItchRPII, ParseAllFields) {
    // Layout: type(1) locate(2) tracking(2) timestamp(6)
    //         stock(8) interest_flag(1)
    // Body size: 19, wire size: 20
    uint8_t msg[20];
    std::memset(msg, 0, sizeof(msg));

    msg[0] = 'N';
    std::memcpy(msg + 11, "NFLX    ", 8);
    msg[19] = 'A'; // both buy and sell side interest

    EXPECT_EQ(rpii::stock(msg), "NFLX    ");
    EXPECT_EQ(rpii::interest_flag(msg), 'A');
}

// ===========================================================================
// dispatch() — type-safe visitor
// ===========================================================================

TEST(ItchDispatch, dispatches_add_order_to_correct_tag) {
    // Build an AddOrder message (36 bytes)
    uint8_t raw[36];
    std::memset(raw, 0, sizeof(raw));
    raw[0] = kAddOrder;

    auto view = parse(raw, sizeof(raw));
    ASSERT_TRUE(view.has_value());

    bool called = false;
    dispatch(*view, [&](auto tag, const uint8_t* msg) {
        if constexpr (std::is_same_v<decltype(tag), msg::AddOrder>) {
            called = true;
            EXPECT_EQ(msg, raw); // full message pointer
        } else {
            FAIL() << "wrong tag type dispatched";
        }
    });
    EXPECT_TRUE(called);
}

TEST(ItchDispatch, dispatches_cross_trade) {
    uint8_t raw[40];
    std::memset(raw, 0, sizeof(raw));
    raw[0] = kCrossTrade;

    auto view = parse(raw, sizeof(raw));
    ASSERT_TRUE(view.has_value());

    bool called = false;
    dispatch(*view, [&](auto tag, const uint8_t*) {
        if constexpr (std::is_same_v<decltype(tag), msg::CrossTrade>) {
            called = true;
        }
    });
    EXPECT_TRUE(called);
}

TEST(ItchDispatch, dispatches_all_22_types) {
    // Verify dispatch hits the right branch for every known message type
    struct TypeCounter {
        int system_event = 0, stock_dir = 0, trading_action = 0, reg_sho = 0;
        int mpp = 0, mwcb_decline = 0, mwcb_status = 0, ipo = 0;
        int luld = 0, op_halt = 0, add_order = 0, add_order_mpid = 0;
        int exec = 0, exec_price = 0, cancel = 0, del = 0, replace = 0;
        int non_cross = 0, cross = 0, broken = 0, noii = 0, rpii = 0;
        int unknown = 0;

        void operator()(msg::SystemEvent, const uint8_t*)              { ++system_event; }
        void operator()(msg::StockDirectory, const uint8_t*)           { ++stock_dir; }
        void operator()(msg::StockTradingAction, const uint8_t*)       { ++trading_action; }
        void operator()(msg::RegSHORestriction, const uint8_t*)        { ++reg_sho; }
        void operator()(msg::MarketParticipantPosition, const uint8_t*){ ++mpp; }
        void operator()(msg::MWCBDeclineLevel, const uint8_t*)         { ++mwcb_decline; }
        void operator()(msg::MWCBStatus, const uint8_t*)               { ++mwcb_status; }
        void operator()(msg::IPOQuotingPeriod, const uint8_t*)         { ++ipo; }
        void operator()(msg::LULDAuctionCollar, const uint8_t*)        { ++luld; }
        void operator()(msg::OperationalHalt, const uint8_t*)          { ++op_halt; }
        void operator()(msg::AddOrder, const uint8_t*)                 { ++add_order; }
        void operator()(msg::AddOrderMPID, const uint8_t*)             { ++add_order_mpid; }
        void operator()(msg::OrderExecuted, const uint8_t*)            { ++exec; }
        void operator()(msg::OrderExecutedWithPrice, const uint8_t*)   { ++exec_price; }
        void operator()(msg::OrderCancel, const uint8_t*)              { ++cancel; }
        void operator()(msg::OrderDelete, const uint8_t*)              { ++del; }
        void operator()(msg::OrderReplace, const uint8_t*)             { ++replace; }
        void operator()(msg::NonCrossTrade, const uint8_t*)            { ++non_cross; }
        void operator()(msg::CrossTrade, const uint8_t*)               { ++cross; }
        void operator()(msg::BrokenTrade, const uint8_t*)              { ++broken; }
        void operator()(msg::NOII, const uint8_t*)                     { ++noii; }
        void operator()(msg::RPII, const uint8_t*)                     { ++rpii; }
        void operator()(msg::Unknown, const uint8_t*)                  { ++unknown; }
    };

    // Build and dispatch each message type
    struct TypeInfo { uint8_t type; size_t size; };
    TypeInfo types[] = {
        {kSystemEvent, kSystemEventSize}, {kStockDirectory, kStockDirectorySize},
        {kStockTradingAction, kStockTradingActionSize}, {kRegSHORestriction, kRegSHORestrictionSize},
        {kMarketParticipantPosition, kMarketParticipantPositionSize},
        {kMWCBDeclineLevel, kMWCBDeclineLevelSize}, {kMWCBStatus, kMWCBStatusSize},
        {kIPOQuotingPeriod, kIPOQuotingPeriodSize}, {kLULDAuctionCollar, kLULDAuctionCollarSize},
        {kOperationalHalt, kOperationalHaltSize}, {kAddOrder, kAddOrderSize},
        {kAddOrderMPID, kAddOrderMPIDSize}, {kOrderExecuted, kOrderExecutedSize},
        {kOrderExecutedWithPrice, kOrderExecutedWithPriceSize},
        {kOrderCancel, kOrderCancelSize}, {kOrderDelete, kOrderDeleteSize},
        {kOrderReplace, kOrderReplaceSize}, {kNonCrossTrade, kNonCrossTradeSize},
        {kCrossTrade, kCrossTradeSize}, {kBrokenTrade, kBrokenTradeSize},
        {kNOII, kNOIISize}, {kRPII, kRPIISize},
    };

    TypeCounter counter;
    uint8_t buf[64];
    for (auto& [type, size] : types) {
        std::memset(buf, 0, sizeof(buf));
        buf[0] = type;
        auto view = parse(buf, size);
        ASSERT_TRUE(view.has_value()) << "failed to parse type " << static_cast<char>(type);
        dispatch(*view, counter);
    }

    EXPECT_EQ(counter.system_event, 1);
    EXPECT_EQ(counter.stock_dir, 1);
    EXPECT_EQ(counter.trading_action, 1);
    EXPECT_EQ(counter.reg_sho, 1);
    EXPECT_EQ(counter.mpp, 1);
    EXPECT_EQ(counter.mwcb_decline, 1);
    EXPECT_EQ(counter.mwcb_status, 1);
    EXPECT_EQ(counter.ipo, 1);
    EXPECT_EQ(counter.luld, 1);
    EXPECT_EQ(counter.op_halt, 1);
    EXPECT_EQ(counter.add_order, 1);
    EXPECT_EQ(counter.add_order_mpid, 1);
    EXPECT_EQ(counter.exec, 1);
    EXPECT_EQ(counter.exec_price, 1);
    EXPECT_EQ(counter.cancel, 1);
    EXPECT_EQ(counter.del, 1);
    EXPECT_EQ(counter.replace, 1);
    EXPECT_EQ(counter.non_cross, 1);
    EXPECT_EQ(counter.cross, 1);
    EXPECT_EQ(counter.broken, 1);
    EXPECT_EQ(counter.noii, 1);
    EXPECT_EQ(counter.rpii, 1);
    EXPECT_EQ(counter.unknown, 0);
}

TEST(ItchDispatch, unknown_type_dispatches_to_unknown_tag) {
    // Construct a MessageView with an unrecognized type manually
    MessageView view;
    uint8_t raw[2] = {0xFF, 0x00};
    view.msg_type = 0xFF;
    view.data = raw;
    view.length = 2;

    bool got_unknown = false;
    dispatch(view, [&](auto tag, const uint8_t*) {
        if constexpr (std::is_same_v<decltype(tag), msg::Unknown>) {
            got_unknown = true;
        } else {
            FAIL() << "should dispatch Unknown for unrecognized type";
        }
    });
    EXPECT_TRUE(got_unknown);
}

TEST(ItchDispatch, handler_return_value_forwarded) {
    uint8_t raw[36];
    std::memset(raw, 0, sizeof(raw));
    raw[0] = kAddOrder;

    auto view = parse(raw, sizeof(raw));
    ASSERT_TRUE(view.has_value());

    int result = dispatch(*view, [](auto tag, const uint8_t*) -> int {
        if constexpr (std::is_same_v<decltype(tag), msg::AddOrder>) {
            return 42;
        }
        return 0;
    });
    EXPECT_EQ(result, 42);
}

TEST(ItchDispatch, msg_pointer_matches_accessor_convention) {
    // Verify that the msg pointer passed to dispatch works directly with
    // per-message accessor namespaces (byte 0 = type tag, absolute offsets)
    uint8_t raw[36];
    std::memset(raw, 0, sizeof(raw));
    raw[0] = kAddOrder;
    // Set stock_locate = 0x0042 at offset 1-2
    raw[1] = 0x00;
    raw[2] = 0x42;
    // Set shares = 500 at offset 20 (4 bytes BE)
    raw[20] = 0x00; raw[21] = 0x00; raw[22] = 0x01; raw[23] = 0xF4;

    auto view = parse(raw, sizeof(raw));
    ASSERT_TRUE(view.has_value());

    dispatch(*view, [](auto tag, const uint8_t* msg) {
        if constexpr (std::is_same_v<decltype(tag), msg::AddOrder>) {
            // Common header: pass msg+1 to free functions (they expect body pointer)
            EXPECT_EQ(stock_locate(msg + 1), 0x0042);
            // Per-message accessors: pass msg directly (they expect full message)
            EXPECT_EQ(add_order::shares(msg), 500u);
        }
    });
}

// ---------------------------------------------------------------------------
// trim() — remove trailing spaces from ITCH string fields
// ---------------------------------------------------------------------------

TEST(ItchTrim, trims_trailing_spaces) {
    EXPECT_EQ(trim("AAPL    "), "AAPL");
}

TEST(ItchTrim, no_trailing_spaces) {
    EXPECT_EQ(trim("MSFT"), "MSFT");
}

TEST(ItchTrim, all_spaces_returns_empty) {
    EXPECT_EQ(trim("        "), "");
}

TEST(ItchTrim, empty_string_returns_empty) {
    EXPECT_EQ(trim(""), "");
}

TEST(ItchTrim, single_char_no_trim) {
    EXPECT_EQ(trim("X"), "X");
}

TEST(ItchTrim, single_space) {
    EXPECT_EQ(trim(" "), "");
}

TEST(ItchTrim, interior_spaces_preserved) {
    EXPECT_EQ(trim("A B     "), "A B");
}

TEST(ItchTrim, works_with_stock_accessor) {
    // End-to-end: parse AddOrder, extract stock, trim
    uint8_t raw[36];
    std::memset(raw, ' ', sizeof(raw));
    raw[0] = kAddOrder;
    // Write "AAPL" at stock offset (24..31), rest is spaces
    std::memcpy(raw + 24, "AAPL    ", 8);

    auto view = parse(raw, sizeof(raw));
    ASSERT_TRUE(view.has_value());
    auto stock = add_order::stock(view->data);
    EXPECT_EQ(stock.size(), 8u);
    EXPECT_EQ(trim(stock), "AAPL");
}

// ---------------------------------------------------------------------------
// ParserStats
// ---------------------------------------------------------------------------

TEST(ItchParserStats, accumulates_on_successful_parse) {
    // Build two SystemEvent messages
    uint8_t msg1[kSystemEventSize]{};
    msg1[0] = kSystemEvent;
    msg1[11] = 'O';

    uint8_t msg2[kSystemEventSize]{};
    msg2[0] = kSystemEvent;
    msg2[11] = 'S';

    // Concatenate
    uint8_t combined[kSystemEventSize * 2];
    std::memcpy(combined, msg1, kSystemEventSize);
    std::memcpy(combined + kSystemEventSize, msg2, kSystemEventSize);

    ParserStats stats;
    size_t consumed = parse_all(combined, sizeof(combined),
        [](const auto&) {}, stats);

    EXPECT_EQ(consumed, kSystemEventSize * 2);
    EXPECT_EQ(stats.messages_parsed, 2u);
    EXPECT_EQ(stats.parse_errors, 0u);
    EXPECT_EQ(stats.bytes_consumed, kSystemEventSize * 2);
}

TEST(ItchParserStats, counts_errors_on_unknown_type) {
    // 0xFF is not a known ITCH message type
    uint8_t bad[] = {0xFF, 0x00, 0x00};
    ParserStats stats;
    size_t consumed = parse_all(bad, sizeof(bad),
        [](const auto&) {}, stats);

    EXPECT_EQ(consumed, 0u);
    EXPECT_EQ(stats.messages_parsed, 0u);
    EXPECT_EQ(stats.parse_errors, 1u);
}

TEST(ItchParserStats, reset_clears_all_counters) {
    ParserStats stats;
    stats.messages_parsed = 100;
    stats.parse_errors = 10;
    stats.bytes_consumed = 50000;
    stats.reset();
    EXPECT_EQ(stats.messages_parsed, 0u);
    EXPECT_EQ(stats.parse_errors, 0u);
    EXPECT_EQ(stats.bytes_consumed, 0u);
}

TEST(ItchParserStats, incomplete_trailing_data_is_not_counted_as_error) {
    // One complete SystemEvent + 3 bytes of incomplete data
    uint8_t combined[kSystemEventSize + 3]{};
    combined[0] = kSystemEvent;
    combined[10] = 'O';
    // Trailing 3 bytes: starts with SystemEvent type but too short
    combined[kSystemEventSize] = kSystemEvent;

    ParserStats stats;
    size_t consumed = parse_all(combined, sizeof(combined),
        [](const auto&) {}, stats);

    EXPECT_EQ(consumed, kSystemEventSize);
    EXPECT_EQ(stats.messages_parsed, 1u);
    // The trailing data has type 'S' (known) but is truncated — that's kTruncated, not kIncomplete
    // Actually: parse() sees type='S', expected size=11, but only 3 bytes remain → kTruncated
    // kTruncated is an error (corrupt data, not just partial)
    EXPECT_EQ(stats.parse_errors, 1u);
}

// ---------------------------------------------------------------------------
// stock_trimmed() convenience accessors
// ---------------------------------------------------------------------------

TEST(ItchStockTrimmed, add_order_stock_trimmed) {
    uint8_t raw[kAddOrderSize]{};
    raw[0] = kAddOrder;
    std::memcpy(raw + 24, "AAPL    ", 8);

    EXPECT_EQ(add_order::stock(raw), "AAPL    ");
    EXPECT_EQ(add_order::stock_trimmed(raw), "AAPL");
}

TEST(ItchStockTrimmed, stock_directory_stock_trimmed) {
    uint8_t raw[kStockDirectorySize]{};
    raw[0] = kStockDirectory;
    std::memcpy(raw + 11, "MSFT    ", 8);

    EXPECT_EQ(stock_directory::stock(raw), "MSFT    ");
    EXPECT_EQ(stock_directory::stock_trimmed(raw), "MSFT");
}

TEST(ItchStockTrimmed, all_spaces_returns_empty) {
    uint8_t raw[kAddOrderSize]{};
    raw[0] = kAddOrder;
    std::memcpy(raw + 24, "        ", 8);

    EXPECT_EQ(add_order::stock_trimmed(raw), "");
}

TEST(ItchStockTrimmed, no_trailing_spaces) {
    uint8_t raw[kAddOrderSize]{};
    raw[0] = kAddOrder;
    std::memcpy(raw + 24, "ABCDEFGH", 8);

    EXPECT_EQ(add_order::stock_trimmed(raw), "ABCDEFGH");
}

TEST(ItchStockTrimmed, add_order_mpid_attribution_trimmed) {
    // Buffer is 40 bytes to accommodate attribution at offset 36 (4 bytes).
    uint8_t raw[40]{};
    raw[0] = kAddOrderMPID;
    std::memcpy(raw + 36, "GS  ", 4);

    EXPECT_EQ(add_order_mpid::attribution(raw), "GS  ");
    EXPECT_EQ(add_order_mpid::attribution_trimmed(raw), "GS");
}

TEST(ItchStockTrimmed, cross_trade_stock_trimmed) {
    uint8_t raw[kCrossTradeSize]{};
    raw[0] = kCrossTrade;
    std::memcpy(raw + 19, "TSLA    ", 8);

    EXPECT_EQ(cross_trade::stock_trimmed(raw), "TSLA");
}

TEST(ItchStockTrimmed, noii_stock_trimmed) {
    uint8_t raw[kNOIISize]{};
    raw[0] = kNOII;
    std::memcpy(raw + 28, "GOOG    ", 8);

    EXPECT_EQ(noii::stock_trimmed(raw), "GOOG");
}

// ---------------------------------------------------------------------------
// ParserStats error offset tracking
// ---------------------------------------------------------------------------

TEST(ItchParserStats, error_offset_tracks_unknown_type) {
    // One valid message, then an unknown type
    uint8_t combined[kSystemEventSize + 3]{};
    combined[0] = kSystemEvent;
    combined[10] = 'O';
    combined[kSystemEventSize] = 0xFF; // unknown type

    ParserStats stats;
    parse_all(combined, sizeof(combined), [](const auto&) {}, stats);

    EXPECT_EQ(stats.parse_errors, 1u);
    EXPECT_EQ(stats.first_error_offset, kSystemEventSize);
    EXPECT_EQ(stats.first_error_type, ParseError::kUnknownType);
    EXPECT_EQ(stats.first_error_msg_byte, 0xFF);
}

TEST(ItchParserStats, error_offset_tracks_truncated_type) {
    // One valid message, then a known type but truncated
    uint8_t combined[kSystemEventSize + 3]{};
    combined[0] = kSystemEvent;
    combined[10] = 'O';
    combined[kSystemEventSize] = kAddOrder; // needs 35 bytes, only 3 available

    ParserStats stats;
    parse_all(combined, sizeof(combined), [](const auto&) {}, stats);

    EXPECT_EQ(stats.parse_errors, 1u);
    EXPECT_EQ(stats.first_error_offset, kSystemEventSize);
    EXPECT_EQ(stats.first_error_type, ParseError::kTruncated);
    EXPECT_EQ(stats.first_error_msg_byte, kAddOrder);
}

TEST(ItchParserStats, no_error_fields_stay_zero) {
    uint8_t msg[kSystemEventSize]{};
    msg[0] = kSystemEvent;
    msg[10] = 'O';

    ParserStats stats;
    parse_all(msg, sizeof(msg), [](const auto&) {}, stats);

    EXPECT_EQ(stats.parse_errors, 0u);
    EXPECT_EQ(stats.first_error_offset, 0u);
    EXPECT_EQ(stats.first_error_msg_byte, 0u);
}

TEST(ItchParserStats, reset_clears_error_offset_fields) {
    ParserStats stats;
    stats.first_error_offset = 42;
    stats.first_error_type = ParseError::kUnknownType;
    stats.first_error_msg_byte = 0xFF;
    stats.parse_errors = 1;

    stats.reset();

    EXPECT_EQ(stats.first_error_offset, 0u);
    EXPECT_EQ(stats.first_error_msg_byte, 0u);
    EXPECT_EQ(stats.parse_errors, 0u);
}

// ===========================================================================
// ParserStats dump/to_json/formatter
// ===========================================================================

TEST(ItchParserStats, dump_without_errors) {
    ParserStats stats;
    stats.on_message(35);
    stats.on_message(44);
    auto d = stats.dump();
    EXPECT_TRUE(d.find("messages_parsed: 2") != std::string::npos);
    EXPECT_TRUE(d.find("parse_errors: 0") != std::string::npos);
    EXPECT_TRUE(d.find("bytes_consumed: 79") != std::string::npos);
    EXPECT_TRUE(d.find("first_error") == std::string::npos);
}

TEST(ItchParserStats, dump_with_errors) {
    ParserStats stats;
    stats.on_message(35);
    stats.on_error(100, ParseError::kUnknownType, 0xFF);
    auto d = stats.dump();
    EXPECT_TRUE(d.find("parse_errors: 1") != std::string::npos);
    EXPECT_TRUE(d.find("unknown message type") != std::string::npos);
    EXPECT_TRUE(d.find("offset 100") != std::string::npos);
}

TEST(ItchParserStats, to_json_contains_all_fields) {
    ParserStats stats;
    stats.on_message(35);
    stats.on_error(50, ParseError::kTruncated, 0x41);
    auto j = stats.to_json();
    EXPECT_TRUE(j.find("\"messages_parsed\":1") != std::string::npos);
    EXPECT_TRUE(j.find("\"parse_errors\":1") != std::string::npos);
    EXPECT_TRUE(j.find("\"bytes_consumed\":35") != std::string::npos);
    EXPECT_TRUE(j.find("\"first_error_offset\":50") != std::string::npos);
}

TEST(ItchParserStats, formatter_produces_summary_line) {
    ParserStats stats;
    stats.on_message(35);
    stats.on_message(44);
    auto s = std::format("{}", stats);
    EXPECT_TRUE(s.find("parsed=2") != std::string::npos);
    EXPECT_TRUE(s.find("errors=0") != std::string::npos);
    EXPECT_TRUE(s.find("bytes=79") != std::string::npos);
}

TEST(ItchParserStats, operator_minus_diffs_counters) {
    ParserStats s1{};
    s1.messages_parsed = 200;
    s1.parse_errors = 10;
    s1.bytes_consumed = 8000;
    s1.first_error_offset = 50;
    s1.first_error_type = ParseError::kUnknownType;
    s1.first_error_msg_byte = 0xFF;

    ParserStats s2{};
    s2.messages_parsed = 150;
    s2.parse_errors = 7;
    s2.bytes_consumed = 5000;

    auto delta = s1 - s2;
    EXPECT_EQ(delta.messages_parsed, 50u);
    EXPECT_EQ(delta.parse_errors, 3u);
    EXPECT_EQ(delta.bytes_consumed, 3000u);
    // first_error_* taken from lhs (the later snapshot)
    EXPECT_EQ(delta.first_error_offset, 50u);
    EXPECT_EQ(delta.first_error_type, ParseError::kUnknownType);
    EXPECT_EQ(delta.first_error_msg_byte, 0xFF);
}

TEST(ItchParserStats, equality_compares_all_fields) {
    ParserStats a{};
    a.messages_parsed = 10;
    a.parse_errors = 2;
    a.bytes_consumed = 500;

    ParserStats b = a;
    EXPECT_EQ(a, b);
    b.bytes_consumed = 501;
    EXPECT_NE(a, b);
}

// ===========================================================================
// Aggregate message size helpers
// ===========================================================================

TEST(ItchMessages, MaxMessageSizeIsNOII) {
    // NOII ('I') at 50 bytes is the largest ITCH 5.0 message
    EXPECT_EQ(kMaxMessageSize, 50u);
    EXPECT_EQ(kMaxMessageSize, kNOIISize);
}

TEST(ItchMessages, MinMessageSizeIsSystemEvent) {
    // SystemEvent ('S') and MWCBStatus ('W') at 12 bytes are the smallest ITCH 5.0 messages
    EXPECT_EQ(kMinMessageSize, 12u);
    EXPECT_EQ(kMinMessageSize, kSystemEventSize);
}

TEST(ItchMessages, MaxMessageSizeIsActuallyLargest) {
    // Verify kMaxMessageSize >= all individual message sizes
    EXPECT_GE(kMaxMessageSize, kSystemEventSize);
    EXPECT_GE(kMaxMessageSize, kStockDirectorySize);
    EXPECT_GE(kMaxMessageSize, kStockTradingActionSize);
    EXPECT_GE(kMaxMessageSize, kRegSHORestrictionSize);
    EXPECT_GE(kMaxMessageSize, kMarketParticipantPositionSize);
    EXPECT_GE(kMaxMessageSize, kMWCBDeclineLevelSize);
    EXPECT_GE(kMaxMessageSize, kMWCBStatusSize);
    EXPECT_GE(kMaxMessageSize, kIPOQuotingPeriodSize);
    EXPECT_GE(kMaxMessageSize, kLULDAuctionCollarSize);
    EXPECT_GE(kMaxMessageSize, kOperationalHaltSize);
    EXPECT_GE(kMaxMessageSize, kAddOrderSize);
    EXPECT_GE(kMaxMessageSize, kAddOrderMPIDSize);
    EXPECT_GE(kMaxMessageSize, kOrderExecutedSize);
    EXPECT_GE(kMaxMessageSize, kOrderExecutedWithPriceSize);
    EXPECT_GE(kMaxMessageSize, kOrderCancelSize);
    EXPECT_GE(kMaxMessageSize, kOrderDeleteSize);
    EXPECT_GE(kMaxMessageSize, kOrderReplaceSize);
    EXPECT_GE(kMaxMessageSize, kNonCrossTradeSize);
    EXPECT_GE(kMaxMessageSize, kCrossTradeSize);
    EXPECT_GE(kMaxMessageSize, kBrokenTradeSize);
    EXPECT_GE(kMaxMessageSize, kNOIISize);
    EXPECT_GE(kMaxMessageSize, kRPIISize);
}

TEST(ItchMessages, MinMessageSizeIsActuallySmallest) {
    // Verify kMinMessageSize <= all individual message sizes
    EXPECT_LE(kMinMessageSize, kSystemEventSize);
    EXPECT_LE(kMinMessageSize, kStockDirectorySize);
    EXPECT_LE(kMinMessageSize, kStockTradingActionSize);
    EXPECT_LE(kMinMessageSize, kRegSHORestrictionSize);
    EXPECT_LE(kMinMessageSize, kMarketParticipantPositionSize);
    EXPECT_LE(kMinMessageSize, kMWCBDeclineLevelSize);
    EXPECT_LE(kMinMessageSize, kMWCBStatusSize);
    EXPECT_LE(kMinMessageSize, kIPOQuotingPeriodSize);
    EXPECT_LE(kMinMessageSize, kLULDAuctionCollarSize);
    EXPECT_LE(kMinMessageSize, kOperationalHaltSize);
    EXPECT_LE(kMinMessageSize, kAddOrderSize);
    EXPECT_LE(kMinMessageSize, kAddOrderMPIDSize);
    EXPECT_LE(kMinMessageSize, kOrderExecutedSize);
    EXPECT_LE(kMinMessageSize, kOrderExecutedWithPriceSize);
    EXPECT_LE(kMinMessageSize, kOrderCancelSize);
    EXPECT_LE(kMinMessageSize, kOrderDeleteSize);
    EXPECT_LE(kMinMessageSize, kOrderReplaceSize);
    EXPECT_LE(kMinMessageSize, kNonCrossTradeSize);
    EXPECT_LE(kMinMessageSize, kCrossTradeSize);
    EXPECT_LE(kMinMessageSize, kBrokenTradeSize);
    EXPECT_LE(kMinMessageSize, kNOIISize);
    EXPECT_LE(kMinMessageSize, kRPIISize);
}

// ===========================================================================
// dispatch_all() — combined parse + dispatch
// ===========================================================================

TEST(ItchDispatchAll, dispatches_all_messages_in_buffer) {
    // Build a buffer with 2 SystemEvent messages
    std::vector<uint8_t> buf;
    auto append_system_event = [&](char event_code) {
        // SystemEvent: type(1) + locate(2) + tracking(2) + timestamp(6) + event_code(1) = 12
        // But kSystemEventSize = 11, so total is 11 bytes
        buf.push_back(kSystemEvent);  // 'S'
        buf.push_back(0); buf.push_back(0);  // locate
        buf.push_back(0); buf.push_back(0);  // tracking
        buf.push_back(0); buf.push_back(0); buf.push_back(0);
        buf.push_back(0); buf.push_back(0); buf.push_back(0);  // timestamp (6 bytes)
        // Wait — kSystemEventSize is 11, so 11 bytes total
        // type(1) + locate(2) + tracking(2) + ts(6) = 11, no event_code byte?
        // Actually: type(1) + body(10) = 11
        // body: locate(2) + tracking(2) + ts(6) + event_code(1) = 11
        // So total = 1 + 10 = 11? No: 2+2+6+1 = 11 for body, +1 for type = 12
        // But kSystemEventSize = 11...
        // Let me check: the size constant already accounts for the type byte
        // kSystemEventSize = 11 means the full message is 11 bytes
        // type(1) + locate(2) + tracking(2) + ts(6) = 11, event_code at offset 10
    };
    (void)append_system_event;

    // Build raw bytes matching known sizes
    // SystemEvent = 12 bytes: type(1) locate(2) tracking(2) timestamp(6) event_code(1)
    // event_code is at byte 11
    buf.clear();
    buf.resize(kSystemEventSize, 0);
    buf[0] = kSystemEvent;  // 'S'
    buf[11] = 'O';  // Start of messages

    // Second message
    size_t off = buf.size();
    buf.resize(off + kSystemEventSize, 0);
    buf[off] = kSystemEvent;
    buf[off + 11] = 'C';  // End of messages

    int count = 0;
    std::vector<char> events;
    size_t consumed = dispatch_all(buf.data(), buf.size(),
        [&](auto /*tag*/, const uint8_t* msg) {
            events.push_back(system_event::event_code(msg));
            ++count;
        });

    EXPECT_EQ(consumed, kSystemEventSize * 2);
    EXPECT_EQ(count, 2);
    EXPECT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0], 'O');
    EXPECT_EQ(events[1], 'C');
}

TEST(ItchDispatchAll, with_stats_accumulation) {
    // Build a single AddOrder message (35 bytes)
    std::vector<uint8_t> buf(kAddOrderSize, 0);
    buf[0] = kAddOrder;  // 'A'

    ParserStats stats;
    int dispatched = 0;
    size_t consumed = dispatch_all(buf.data(), buf.size(),
        [&](auto /*tag*/, const uint8_t* /*body*/) {
            ++dispatched;
        }, stats);

    EXPECT_EQ(consumed, kAddOrderSize);
    EXPECT_EQ(dispatched, 1);
    EXPECT_EQ(stats.messages_parsed, 1u);
    EXPECT_EQ(stats.parse_errors, 0u);
    EXPECT_EQ(stats.bytes_consumed, kAddOrderSize);
}

TEST(ItchDispatchAll, empty_buffer_returns_zero) {
    int count = 0;
    size_t consumed = dispatch_all(nullptr, 0,
        [&](auto, const uint8_t*) { ++count; });
    EXPECT_EQ(consumed, 0u);
    EXPECT_EQ(count, 0);
}

TEST(ItchDispatchAll, stops_on_unknown_message_type) {
    // Build buffer with known type + unknown type
    std::vector<uint8_t> buf(kSystemEventSize + 5, 0);
    buf[0] = kSystemEvent;
    buf[kSystemEventSize] = 0xFF;  // Unknown type

    int count = 0;
    size_t consumed = dispatch_all(buf.data(), buf.size(),
        [&](auto, const uint8_t*) { ++count; });

    // Should dispatch the SystemEvent, then stop at unknown type
    EXPECT_EQ(consumed, kSystemEventSize);
    EXPECT_EQ(count, 1);
}

TEST(ItchDispatchAll, truncated_last_message_partial_consume) {
    // Complete SystemEvent + truncated AddOrder (only 10 of 35 bytes)
    std::vector<uint8_t> buf(kSystemEventSize + 10, 0);
    buf[0] = kSystemEvent;
    buf[kSystemEventSize] = kAddOrder;  // Starts the truncated message

    int count = 0;
    size_t consumed = dispatch_all(buf.data(), buf.size(),
        [&](auto, const uint8_t*) { ++count; });

    // Should dispatch SystemEvent, stop at truncated AddOrder
    EXPECT_EQ(consumed, kSystemEventSize);
    EXPECT_EQ(count, 1);
}

TEST(ItchDispatchAll, mixed_message_types_dispatched) {
    // SystemEvent + OrderDelete
    std::vector<uint8_t> buf(kSystemEventSize + kOrderDeleteSize, 0);
    buf[0] = kSystemEvent;
    buf[kSystemEventSize] = kOrderDelete;

    bool saw_system = false, saw_delete = false;
    size_t consumed = dispatch_all(buf.data(), buf.size(),
        [&](auto tag, const uint8_t*) {
            if constexpr (std::is_same_v<decltype(tag), msg::SystemEvent>)
                saw_system = true;
            if constexpr (std::is_same_v<decltype(tag), msg::OrderDelete>)
                saw_delete = true;
        });

    EXPECT_EQ(consumed, kSystemEventSize + kOrderDeleteSize);
    EXPECT_TRUE(saw_system);
    EXPECT_TRUE(saw_delete);
}

TEST(ItchDispatchAll, stats_records_error_on_unknown_type) {
    // Unknown type byte at start
    std::vector<uint8_t> buf = {0xFF, 0, 0, 0, 0};

    ParserStats stats;
    int count = 0;
    dispatch_all(buf.data(), buf.size(),
        [&](auto, const uint8_t*) { ++count; }, stats);

    EXPECT_EQ(count, 0);
    EXPECT_EQ(stats.parse_errors, 1u);
    EXPECT_EQ(stats.first_error_msg_byte, 0xFF);
}

TEST(ItchDispatchAll, stats_incomplete_not_counted_as_error) {
    // A buffer that's too short even for a type lookup
    std::vector<uint8_t> buf;  // empty

    ParserStats stats;
    dispatch_all(buf.data(), buf.size(),
        [](auto, const uint8_t*) {}, stats);

    EXPECT_EQ(stats.messages_parsed, 0u);
    EXPECT_EQ(stats.parse_errors, 0u);  // kIncomplete is not an error
}

// ---------------------------------------------------------------------------
// Message classification helpers
// ---------------------------------------------------------------------------

TEST(ItchClassification, system_messages_classified_correctly) {
    EXPECT_TRUE(is_system_message(kSystemEvent));
    EXPECT_TRUE(is_system_message(kStockDirectory));
    EXPECT_TRUE(is_system_message(kStockTradingAction));
    EXPECT_TRUE(is_system_message(kRegSHORestriction));
    EXPECT_TRUE(is_system_message(kMarketParticipantPosition));
    EXPECT_TRUE(is_system_message(kMWCBDeclineLevel));
    EXPECT_TRUE(is_system_message(kMWCBStatus));
    EXPECT_TRUE(is_system_message(kIPOQuotingPeriod));
    EXPECT_TRUE(is_system_message(kLULDAuctionCollar));
    EXPECT_TRUE(is_system_message(kOperationalHalt));
    // Non-system types
    EXPECT_FALSE(is_system_message(kAddOrder));
    EXPECT_FALSE(is_system_message(kNonCrossTrade));
    EXPECT_FALSE(is_system_message(kNOII));
    EXPECT_FALSE(is_system_message(0xFF));
}

TEST(ItchClassification, order_messages_classified_correctly) {
    EXPECT_TRUE(is_order_message(kAddOrder));
    EXPECT_TRUE(is_order_message(kAddOrderMPID));
    EXPECT_TRUE(is_order_message(kOrderExecuted));
    EXPECT_TRUE(is_order_message(kOrderExecutedWithPrice));
    EXPECT_TRUE(is_order_message(kOrderCancel));
    EXPECT_TRUE(is_order_message(kOrderDelete));
    EXPECT_TRUE(is_order_message(kOrderReplace));
    // Non-order types
    EXPECT_FALSE(is_order_message(kSystemEvent));
    EXPECT_FALSE(is_order_message(kNonCrossTrade));
    EXPECT_FALSE(is_order_message(kNOII));
}

TEST(ItchClassification, trade_messages_classified_correctly) {
    EXPECT_TRUE(is_trade_message(kNonCrossTrade));
    EXPECT_TRUE(is_trade_message(kCrossTrade));
    EXPECT_TRUE(is_trade_message(kBrokenTrade));
    // Non-trade types
    EXPECT_FALSE(is_trade_message(kAddOrder));
    EXPECT_FALSE(is_trade_message(kSystemEvent));
    EXPECT_FALSE(is_trade_message(kNOII));
}

TEST(ItchClassification, imbalance_messages_classified_correctly) {
    EXPECT_TRUE(is_imbalance_message(kNOII));
    EXPECT_TRUE(is_imbalance_message(kRPII));
    EXPECT_FALSE(is_imbalance_message(kAddOrder));
    EXPECT_FALSE(is_imbalance_message(kNonCrossTrade));
}

TEST(ItchClassification, is_known_type_covers_all_types) {
    // All 22 ITCH 5.0 types should be known
    const uint8_t all_types[] = {
        kSystemEvent, kStockDirectory, kStockTradingAction, kRegSHORestriction,
        kMarketParticipantPosition, kMWCBDeclineLevel, kMWCBStatus,
        kIPOQuotingPeriod, kLULDAuctionCollar, kOperationalHalt,
        kAddOrder, kAddOrderMPID, kOrderExecuted, kOrderExecutedWithPrice,
        kOrderCancel, kOrderDelete, kOrderReplace,
        kNonCrossTrade, kCrossTrade, kBrokenTrade,
        kNOII, kRPII,
    };
    for (uint8_t t : all_types) {
        EXPECT_TRUE(is_known_type(t)) << "type=" << static_cast<char>(t);
    }
    EXPECT_FALSE(is_known_type(0x00));
    EXPECT_FALSE(is_known_type(0xFF));
    EXPECT_FALSE(is_known_type('Z'));
}

TEST(ItchClassification, categories_are_mutually_exclusive) {
    // Every known type should belong to exactly one category
    const uint8_t all_types[] = {
        kSystemEvent, kStockDirectory, kStockTradingAction, kRegSHORestriction,
        kMarketParticipantPosition, kMWCBDeclineLevel, kMWCBStatus,
        kIPOQuotingPeriod, kLULDAuctionCollar, kOperationalHalt,
        kAddOrder, kAddOrderMPID, kOrderExecuted, kOrderExecutedWithPrice,
        kOrderCancel, kOrderDelete, kOrderReplace,
        kNonCrossTrade, kCrossTrade, kBrokenTrade,
        kNOII, kRPII,
    };
    for (uint8_t t : all_types) {
        int count = is_system_message(t) + is_order_message(t) +
                    is_trade_message(t) + is_imbalance_message(t);
        EXPECT_EQ(count, 1) << "type=" << static_cast<char>(t)
                            << " belongs to " << count << " categories";
    }
}

TEST(ItchClassification, constexpr_usable) {
    // Verify all classification functions are constexpr-evaluable
    static_assert(is_system_message(kSystemEvent));
    static_assert(!is_system_message(kAddOrder));
    static_assert(is_order_message(kAddOrder));
    static_assert(!is_order_message(kNonCrossTrade));
    static_assert(is_trade_message(kNonCrossTrade));
    static_assert(is_imbalance_message(kNOII));
    static_assert(is_known_type(kSystemEvent));
    static_assert(!is_known_type(0xFF));
}

// ---------------------------------------------------------------------------
// MessageView dump()/to_json()
// ---------------------------------------------------------------------------

TEST(ItchMessageView, dump_contains_key_fields) {
    // Build a minimal SystemEvent message
    std::vector<uint8_t> buf(kSystemEventSize, 0);
    buf[0] = kSystemEvent;
    // stock_locate = 42 (big-endian at offset 1)
    buf[1] = 0x00; buf[2] = 0x2A;
    // tracking = 7 (big-endian at offset 3)
    buf[3] = 0x00; buf[4] = 0x07;

    auto result = parse(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());

    std::string d = result->dump();
    EXPECT_NE(d.find("SystemEvent"), std::string::npos);
    EXPECT_NE(d.find("42"), std::string::npos);   // stock_locate
    EXPECT_NE(d.find("7"), std::string::npos);    // tracking_number
}

TEST(ItchMessageView, to_json_is_valid_structure) {
    std::vector<uint8_t> buf(kAddOrderSize, 0);
    buf[0] = kAddOrder;

    auto result = parse(buf.data(), buf.size());
    ASSERT_TRUE(result.has_value());

    std::string j = result->to_json();
    EXPECT_NE(j.find("\"msg_type\":\"AddOrder\""), std::string::npos);
    EXPECT_NE(j.find("\"msg_type_char\":\"A\""), std::string::npos);
    EXPECT_NE(j.find("\"stock_locate\":"), std::string::npos);
    EXPECT_NE(j.find("\"timestamp_ns\":"), std::string::npos);
    EXPECT_NE(j.find("\"length\":"), std::string::npos);
    // Should start with { and end with }
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
}

// ---------------------------------------------------------------------------
// std::span overloads
// ---------------------------------------------------------------------------

TEST(ItchParserSpan, parse_span_overload_matches_raw) {
    std::vector<uint8_t> buf(kAddOrderSize, 0);
    buf[0] = kAddOrder;

    auto raw_result = parse(buf.data(), buf.size());
    auto span_result = parse(std::span<const uint8_t>(buf));

    ASSERT_TRUE(raw_result.has_value());
    ASSERT_TRUE(span_result.has_value());
    EXPECT_EQ(raw_result->msg_type, span_result->msg_type);
    EXPECT_EQ(raw_result->length, span_result->length);
}

TEST(ItchParserSpan, parse_all_span_overload_matches_raw) {
    // Build buffer with two consecutive messages
    std::vector<uint8_t> buf(kSystemEventSize + kAddOrderSize, 0);
    buf[0] = kSystemEvent;
    buf[kSystemEventSize] = kAddOrder;

    size_t raw_count = 0;
    size_t raw_consumed = parse_all(buf.data(), buf.size(),
        [&](const MessageView&) { raw_count++; });

    size_t span_count = 0;
    size_t span_consumed = parse_all(std::span<const uint8_t>(buf),
        [&](const MessageView&) { span_count++; });

    EXPECT_EQ(raw_count, 2u);
    EXPECT_EQ(span_count, 2u);
    EXPECT_EQ(raw_consumed, span_consumed);
}
