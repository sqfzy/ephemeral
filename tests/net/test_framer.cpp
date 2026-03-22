/// @file test_framer.cpp
/// Unit tests for the MessageFramer concept, RawFramer, and LengthPrefixFramer.
/// Covers concept satisfaction, encode/decode paths, error handling, and roundtrips.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/net/framer_concept.hpp"
#include "eph/net/length_prefix_framer.hpp"
#include "eph/net/raw_framer.hpp"

using namespace eph::net;

// ---------------------------------------------------------------------------
// Test 1: Concept satisfaction
// ---------------------------------------------------------------------------

TEST(FramerConcept, RawFramerSatisfiesConcept) {
    static_assert(MessageFramer<RawFramer>,
                  "RawFramer must satisfy MessageFramer concept");
}

TEST(FramerConcept, LengthPrefixFramerSatisfiesConcept) {
    static_assert(MessageFramer<LengthPrefixFramer>,
                  "LengthPrefixFramer must satisfy MessageFramer concept");
}

// ---------------------------------------------------------------------------
// Test 2: RawFramer encode — data in = data out
// ---------------------------------------------------------------------------

TEST(RawFramer, EncodePassthrough) {
    const uint8_t input[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t output[4];

    RawFramer framer;
    size_t written = framer.encode(output, input, sizeof(input), 0);

    EXPECT_EQ(written, sizeof(input));
    EXPECT_EQ(std::memcmp(output, input, sizeof(input)), 0);
}

TEST(RawFramer, EncodeReturnValueMatchesLength) {
    const uint8_t input[] = {0x01, 0x02, 0x03};
    uint8_t output[3];

    RawFramer framer;
    size_t written = framer.encode(output, input, sizeof(input), 42);

    EXPECT_EQ(written, 3u);
}

TEST(RawFramer, MaxOverheadIsZero) {
    EXPECT_EQ(RawFramer::max_overhead(), 0u);
}

// ---------------------------------------------------------------------------
// Test 3: RawFramer decode — returns entire buffer as single frame
// ---------------------------------------------------------------------------

TEST(RawFramer, DecodeReturnsEntireBuffer) {
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    auto result = RawFramer::decode(data, sizeof(data));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload, data);
    EXPECT_EQ(result->payload_len, sizeof(data));
    EXPECT_EQ(result->msg_type, 0);
    EXPECT_EQ(result->is_control, false);
    EXPECT_EQ(result->total_len, sizeof(data));
}

// ---------------------------------------------------------------------------
// Test 4: RawFramer decode empty — returns kIncomplete
// ---------------------------------------------------------------------------

TEST(RawFramer, DecodeEmptyReturnsIncomplete) {
    auto result = RawFramer::decode(nullptr, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

// ---------------------------------------------------------------------------
// Test 5: LengthPrefixFramer encode — 2-byte BE header prepended
// ---------------------------------------------------------------------------

TEST(LengthPrefixFramer, EncodePrepends2ByteHeader) {
    const uint8_t payload[] = {'H', 'e', 'l', 'l', 'o'};
    uint8_t output[7]; // 2 header + 5 payload

    LengthPrefixFramer framer;
    size_t written = framer.encode(output, payload, sizeof(payload), 0);

    EXPECT_EQ(written, 7u);

    // Verify big-endian length prefix: 5 = 0x0005
    EXPECT_EQ(output[0], 0x00);
    EXPECT_EQ(output[1], 0x05);

    // Verify payload copied correctly
    EXPECT_EQ(std::memcmp(output + 2, payload, sizeof(payload)), 0);
}

TEST(LengthPrefixFramer, EncodeLargePayloadLength) {
    // Verify a payload of length 300 (0x012C) encodes the header correctly
    std::vector<uint8_t> payload(300, 0xAB);
    std::vector<uint8_t> output(302);

    LengthPrefixFramer framer;
    size_t written = framer.encode(output.data(), payload.data(), payload.size(), 0);

    EXPECT_EQ(written, 302u);
    EXPECT_EQ(output[0], 0x01); // high byte of 300
    EXPECT_EQ(output[1], 0x2C); // low byte of 300
}

TEST(LengthPrefixFramer, MaxOverheadIsTwo) {
    EXPECT_EQ(LengthPrefixFramer::max_overhead(), 2u);
}

// ---------------------------------------------------------------------------
// Test 6: LengthPrefixFramer decode happy path
// ---------------------------------------------------------------------------

TEST(LengthPrefixFramer, DecodeHappyPath) {
    // Construct a framed message: length=3, payload={0xAA, 0xBB, 0xCC}
    const uint8_t data[] = {0x00, 0x03, 0xAA, 0xBB, 0xCC};
    auto result = LengthPrefixFramer::decode(data, sizeof(data));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload, data + 2);
    EXPECT_EQ(result->payload_len, 3u);
    EXPECT_EQ(result->msg_type, 0xAA); // first payload byte
    EXPECT_EQ(result->is_control, false);
    EXPECT_EQ(result->total_len, 5u);
}

// ---------------------------------------------------------------------------
// Test 7: LengthPrefixFramer decode incomplete header
// ---------------------------------------------------------------------------

TEST(LengthPrefixFramer, DecodeIncompleteHeaderZeroBytes) {
    auto result = LengthPrefixFramer::decode(nullptr, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

TEST(LengthPrefixFramer, DecodeIncompleteHeaderOneByte) {
    const uint8_t data[] = {0x00};
    auto result = LengthPrefixFramer::decode(data, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

// ---------------------------------------------------------------------------
// Test 8: LengthPrefixFramer decode incomplete payload
// ---------------------------------------------------------------------------

TEST(LengthPrefixFramer, DecodeIncompletePayload) {
    // Header says 100 bytes, but only 50 payload bytes available (52 total)
    std::vector<uint8_t> data(52, 0x00);
    data[0] = 0x00;
    data[1] = 0x64; // length = 100

    auto result = LengthPrefixFramer::decode(data.data(), data.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

TEST(LengthPrefixFramer, DecodeIncompletePayloadByOneByte) {
    // Header says 10 bytes, only 9 payload bytes available (11 total)
    std::vector<uint8_t> data(11, 0xFF);
    data[0] = 0x00;
    data[1] = 0x0A; // length = 10

    auto result = LengthPrefixFramer::decode(data.data(), data.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

// ---------------------------------------------------------------------------
// Test 9: LengthPrefixFramer decode zero length — kInvalidFormat
// ---------------------------------------------------------------------------

TEST(LengthPrefixFramer, DecodeZeroLengthReturnsInvalidFormat) {
    const uint8_t data[] = {0x00, 0x00, 0xAA}; // length=0
    auto result = LengthPrefixFramer::decode(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kInvalidFormat);
}

// ---------------------------------------------------------------------------
// Test 10: LengthPrefixFramer roundtrip
// ---------------------------------------------------------------------------

TEST(LengthPrefixFramer, Roundtrip) {
    const uint8_t payload[] = {0x41, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t encoded[9]; // 2 header + 7 payload

    LengthPrefixFramer framer;
    size_t written = framer.encode(encoded, payload, sizeof(payload), 0);
    ASSERT_EQ(written, 9u);

    auto result = LengthPrefixFramer::decode(encoded, written);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, sizeof(payload));
    EXPECT_EQ(result->total_len, written);
    EXPECT_EQ(std::memcmp(result->payload, payload, sizeof(payload)), 0);
}

TEST(LengthPrefixFramer, RoundtripSingleByte) {
    const uint8_t payload[] = {0x42};
    uint8_t encoded[3];

    LengthPrefixFramer framer;
    size_t written = framer.encode(encoded, payload, 1, 0);
    ASSERT_EQ(written, 3u);

    auto result = LengthPrefixFramer::decode(encoded, written);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, 1u);
    EXPECT_EQ(result->payload[0], 0x42);
}

// ---------------------------------------------------------------------------
// Test 11: LengthPrefixFramer msg_type — first payload byte exposed
// ---------------------------------------------------------------------------

TEST(LengthPrefixFramer, MsgTypeIsFirstPayloadByte) {
    // First payload byte = 'A' (0x41)
    const uint8_t data[] = {0x00, 0x04, 'A', 0x01, 0x02, 0x03};
    auto result = LengthPrefixFramer::decode(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, 'A');
}

TEST(LengthPrefixFramer, MsgTypeWithDifferentTypes) {
    // Test with 'E' (OrderExecuted)
    const uint8_t data[] = {0x00, 0x02, 'E', 0xFF};
    auto result = LengthPrefixFramer::decode(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, 'E');
}

// ---------------------------------------------------------------------------
// Test 12: FrameError names
// ---------------------------------------------------------------------------

TEST(FrameError, ErrorNames) {
    EXPECT_EQ(frame_error_name(FrameError::kIncomplete), "incomplete");
    EXPECT_EQ(frame_error_name(FrameError::kInvalidFormat), "invalid format");
    EXPECT_EQ(frame_error_name(FrameError::kPayloadTooLarge), "payload too large");
}
