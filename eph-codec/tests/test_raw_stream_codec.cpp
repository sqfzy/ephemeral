/// @file test_raw_stream_codec.cpp
/// Unit tests for `eph::codec::RawStreamCodec`.

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/raw_stream_codec.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"

using eph::codec::RawStreamCodec;
using eph::codec::SpanPacketView;
using eph::core::Error;
using eph::core::ErrorInfo;
using eph::core::OutputBuffer;
using eph::core::StreamCodec;

// Compile-time concept conformance — the primary contract under test.
static_assert(StreamCodec<RawStreamCodec>,
              "RawStreamCodec must satisfy StreamCodec");

// ---------------------------------------------------------------------------
// decode
// ---------------------------------------------------------------------------

TEST(RawStreamCodec, DecodeFullBufferReturnsWholeFrame) {
    std::vector<uint8_t> buf{0xDE, 0xAD, 0xBE, 0xEF};
    SpanPacketView view{buf.data(), buf.size()};

    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    RawStreamCodec codec;
    auto result = codec.decode(view, out);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->has_value());
    auto frame = **result;
    EXPECT_EQ(frame.size(), 4u);
    EXPECT_EQ(frame[0], 0xDE);
    EXPECT_EQ(frame[3], 0xEF);
    // The codec must fully consume the view.
    EXPECT_EQ(view.length(), 0u);
    // No auto-response for a raw codec.
    EXPECT_EQ(out.size(), 0u);
}

TEST(RawStreamCodec, DecodeEmptyViewReturnsNone) {
    std::array<uint8_t, 1> storage{};
    SpanPacketView view{storage.data(), 0};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    RawStreamCodec codec;
    auto result = codec.decode(view, out);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_value());
}

TEST(RawStreamCodec, DecodeIsIdempotentAfterConsumption) {
    std::vector<uint8_t> buf{1, 2, 3};
    SpanPacketView view{buf.data(), buf.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    RawStreamCodec codec;
    auto r1 = codec.decode(view, out);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value());

    auto r2 = codec.decode(view, out);
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->has_value());  // nothing left
}

// ---------------------------------------------------------------------------
// encode
// ---------------------------------------------------------------------------

TEST(RawStreamCodec, EncodeCopiesPayload) {
    uint8_t buf[16]{};
    RawStreamCodec codec;
    const uint8_t payload[] = {0xCA, 0xFE, 0xBA, 0xBE};
    auto r = codec.encode(buf, sizeof(buf), std::span<const uint8_t>{payload, 4});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 4u);
    EXPECT_EQ(buf[0], 0xCA);
    EXPECT_EQ(buf[3], 0xBE);
}

TEST(RawStreamCodec, EncodeBufferFullReturnsError) {
    uint8_t buf[2]{};
    RawStreamCodec codec;
    const uint8_t payload[] = {1, 2, 3};
    auto r = codec.encode(buf, sizeof(buf), std::span<const uint8_t>{payload, 3});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
}

TEST(RawStreamCodec, EncodeEmptyPayloadReturnsZero) {
    uint8_t buf[4]{};
    RawStreamCodec codec;
    auto r = codec.encode(buf, sizeof(buf), std::span<const uint8_t>{});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0u);
}

// ---------------------------------------------------------------------------
// round trip
// ---------------------------------------------------------------------------

TEST(RawStreamCodec, EncodeDecodeRoundTrip) {
    const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44, 0x55};

    uint8_t wire[8]{};
    RawStreamCodec encoder;
    auto n = encoder.encode(wire, sizeof(wire),
                             std::span<const uint8_t>{payload, 5});
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 5u);

    RawStreamCodec decoder;
    SpanPacketView view{wire, *n};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};
    auto r = decoder.decode(view, out);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    auto frame = **r;
    ASSERT_EQ(frame.size(), 5u);
    EXPECT_EQ(std::memcmp(frame.data(), payload, 5), 0);
}
