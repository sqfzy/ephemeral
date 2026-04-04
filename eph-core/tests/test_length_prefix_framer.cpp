/// @file test_length_prefix_framer.cpp
/// Unit tests for LengthPrefixFramer: encode/decode round-trip, edge cases, and
/// concept satisfaction.

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/length_prefix_framer.hpp"

using namespace eph::net;

// ----------------------------------------------------------------------------
// Concept satisfaction
// ----------------------------------------------------------------------------

static_assert(MessageFramer<LengthPrefixFramer>,
    "LengthPrefixFramer must satisfy the MessageFramer concept");

// ----------------------------------------------------------------------------
// Round-trip: encode() then decode() recovers the original payload
// ----------------------------------------------------------------------------

class LengthPrefixFramerRoundTrip
    : public ::testing::TestWithParam<size_t> {};

TEST_P(LengthPrefixFramerRoundTrip, EncodeDecodePaysloadPreserved) {
    const size_t payload_len = GetParam();
    std::vector<uint8_t> payload(payload_len);
    // Fill with a recognizable pattern (0, 1, 2, ..., 255, 0, 1, ...)
    std::iota(payload.begin(), payload.end(), uint8_t{0});

    std::vector<uint8_t> wire(payload_len + 2);
    LengthPrefixFramer framer;

    const size_t written = framer.encode(wire.data(), payload.data(),
                                         payload_len, /*msg_type=*/0);
    ASSERT_EQ(written, payload_len + 2)
        << "encode() should return header + payload length";

    auto result = framer.decode(wire.data(), wire.size());
    ASSERT_TRUE(result.has_value()) << "decode() should succeed";
    EXPECT_EQ(result->payload_len, payload_len);
    EXPECT_EQ(result->total_len, payload_len + 2);
    EXPECT_EQ(result->msg_type, payload[0]);
    EXPECT_FALSE(result->is_control);
    EXPECT_EQ(std::memcmp(result->payload, payload.data(), payload_len), 0)
        << "round-trip payload mismatch";
}

INSTANTIATE_TEST_SUITE_P(
    VariousPayloadSizes,
    LengthPrefixFramerRoundTrip,
    ::testing::Values(1, 100, 1000, LengthPrefixFramer::kMaxPayloadLen));

// ----------------------------------------------------------------------------
// decode(): zero-length payload returns kInvalidFormat
// ----------------------------------------------------------------------------

TEST(LengthPrefixFramerDecode, ZeroLengthPayloadReturnsInvalidFormat) {
    // Wire: length = 0 encoded as big-endian
    uint8_t wire[] = {0x00, 0x00};
    LengthPrefixFramer framer;

    auto result = framer.decode(wire, sizeof(wire));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kInvalidFormat);
}

// ----------------------------------------------------------------------------
// decode(): truncated header (< 2 bytes) returns kIncomplete
// ----------------------------------------------------------------------------

TEST(LengthPrefixFramerDecode, TruncatedHeaderReturnsIncomplete_Zero) {
    LengthPrefixFramer framer;
    auto result = framer.decode(nullptr, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

TEST(LengthPrefixFramerDecode, TruncatedHeaderReturnsIncomplete_One) {
    uint8_t wire[] = {0x00};
    LengthPrefixFramer framer;
    auto result = framer.decode(wire, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

// ----------------------------------------------------------------------------
// decode(): header declares more data than available returns kIncomplete
// ----------------------------------------------------------------------------

TEST(LengthPrefixFramerDecode, InsufficientPayloadReturnsIncomplete) {
    // Header says 256 bytes, but we only supply 10 bytes total (2 header + 8 payload)
    uint8_t wire[10] = {};
    wire[0] = 0x01;  // length high byte
    wire[1] = 0x00;  // length low byte  => 256
    LengthPrefixFramer framer;

    auto result = framer.decode(wire, sizeof(wire));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

// ----------------------------------------------------------------------------
// encode(): payload exceeding kMaxPayloadLen returns 0
// ----------------------------------------------------------------------------

TEST(LengthPrefixFramerEncode, OversizedPayloadReturnsZero) {
    const size_t too_large = LengthPrefixFramer::kMaxPayloadLen + 1;
    std::vector<uint8_t> payload(too_large, 0xAA);
    std::vector<uint8_t> wire(too_large + 2);
    LengthPrefixFramer framer;

    const size_t written = framer.encode(wire.data(), payload.data(),
                                         too_large, /*msg_type=*/0);
    EXPECT_EQ(written, 0u) << "encode() must reject payloads > kMaxPayloadLen";
}

// ----------------------------------------------------------------------------
// encode(): zero-length payload returns 0
// ----------------------------------------------------------------------------

TEST(LengthPrefixFramerEncode, ZeroLengthPayloadReturnsZero) {
    uint8_t dummy = 0;
    uint8_t wire[4] = {};
    LengthPrefixFramer framer;

    const size_t written = framer.encode(wire, &dummy, 0, /*msg_type=*/0);
    EXPECT_EQ(written, 0u) << "encode() must reject zero-length payloads";
}

// ----------------------------------------------------------------------------
// encode(): null data or null out returns 0
// ----------------------------------------------------------------------------

TEST(LengthPrefixFramerEncode, NullDataReturnsZero) {
    uint8_t wire[4] = {};
    LengthPrefixFramer framer;
    EXPECT_EQ(framer.encode(wire, nullptr, 1, 0), 0u);
}

TEST(LengthPrefixFramerEncode, NullOutReturnsZero) {
    uint8_t data[] = {0x42};
    LengthPrefixFramer framer;
    EXPECT_EQ(framer.encode(nullptr, data, 1, 0), 0u);
}

// ----------------------------------------------------------------------------
// max_overhead() is 2
// ----------------------------------------------------------------------------

TEST(LengthPrefixFramerMisc, MaxOverheadIsTwo) {
    EXPECT_EQ(LengthPrefixFramer::max_overhead(), 2u);
}
