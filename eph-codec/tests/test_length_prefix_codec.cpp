/// @file test_length_prefix_codec.cpp
/// Unit tests for `eph::codec::LengthPrefixCodec`.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/length_prefix_codec.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"

using eph::codec::LengthPrefixCodec;
using eph::codec::SpanPacketView;
using eph::core::Error;
using eph::core::OutputBuffer;
using eph::core::StreamCodec;

static_assert(StreamCodec<LengthPrefixCodec>,
              "LengthPrefixCodec must satisfy StreamCodec");

// Helper: encode one BE uint32 prefix + payload into a vector.
static std::vector<uint8_t> build_frame(std::vector<uint8_t> payload) {
    std::vector<uint8_t> out;
    out.reserve(4 + payload.size());
    uint32_t n = static_cast<uint32_t>(payload.size());
    out.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((n >>  8) & 0xFF));
    out.push_back(static_cast<uint8_t>((n      ) & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ---------------------------------------------------------------------------
// decode — complete frames
// ---------------------------------------------------------------------------

TEST(LengthPrefixCodec, DecodeCompleteFrame) {
    auto wire = build_frame({1, 2, 3, 4, 5});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    LengthPrefixCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    auto frame = **r;
    ASSERT_EQ(frame.size(), 5u);
    EXPECT_EQ(frame[0], 1);
    EXPECT_EQ(frame[4], 5);
    EXPECT_EQ(view.length(), 0u);  // fully consumed
}

TEST(LengthPrefixCodec, DecodeZeroLengthFrameAllowed) {
    auto wire = build_frame({});  // 4 header bytes, no payload
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    LengthPrefixCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((**r).size(), 0u);
    EXPECT_EQ(view.length(), 0u);
}

// ---------------------------------------------------------------------------
// decode — partial
// ---------------------------------------------------------------------------

TEST(LengthPrefixCodec, DecodeHeaderOnlyReturnsNone) {
    auto wire = build_frame({1, 2, 3, 4});
    // Deliver only the first 4 header bytes — payload missing.
    SpanPacketView view{wire.data(), 4};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    LengthPrefixCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    // View unchanged — decoder did not consume bytes.
    EXPECT_EQ(view.length(), 4u);
}

TEST(LengthPrefixCodec, DecodeHeaderShorterThanFourReturnsNone) {
    uint8_t wire[3] = {0, 0, 0};
    SpanPacketView view{wire, 3};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    LengthPrefixCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    EXPECT_EQ(view.length(), 3u);
}

TEST(LengthPrefixCodec, DecodePartialPayloadReturnsNone) {
    auto wire = build_frame({1, 2, 3, 4, 5});
    // 4 header + 3 of 5 payload bytes.
    SpanPacketView view{wire.data(), 7};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    LengthPrefixCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    EXPECT_EQ(view.length(), 7u);
}

// ---------------------------------------------------------------------------
// decode — multiple frames in one view
// ---------------------------------------------------------------------------

TEST(LengthPrefixCodec, DecodeMultipleFramesInOneView) {
    auto wire1 = build_frame({10, 20, 30});
    auto wire2 = build_frame({40, 50});
    std::vector<uint8_t> combined;
    combined.insert(combined.end(), wire1.begin(), wire1.end());
    combined.insert(combined.end(), wire2.begin(), wire2.end());

    SpanPacketView view{combined.data(), combined.size()};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    LengthPrefixCodec codec;
    auto r1 = codec.decode(view, out);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value());
    ASSERT_EQ((**r1).size(), 3u);
    EXPECT_EQ((**r1)[0], 10);
    EXPECT_EQ(view.length(), 4u + 2u);  // second frame remains

    auto r2 = codec.decode(view, out);
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r2->has_value());
    ASSERT_EQ((**r2).size(), 2u);
    EXPECT_EQ((**r2)[0], 40);
    EXPECT_EQ(view.length(), 0u);

    // Third call on exhausted view → None.
    auto r3 = codec.decode(view, out);
    ASSERT_TRUE(r3.has_value());
    EXPECT_FALSE(r3->has_value());
}

// ---------------------------------------------------------------------------
// decode — oversize
// ---------------------------------------------------------------------------

TEST(LengthPrefixCodec, DecodeOversizeReturnsCodecOverflow) {
    // Craft a header with length just above kMaxFrameLen.
    const uint32_t bogus_len = LengthPrefixCodec::kMaxFrameLen + 1;
    std::vector<uint8_t> wire(4);
    wire[0] = static_cast<uint8_t>((bogus_len >> 24) & 0xFF);
    wire[1] = static_cast<uint8_t>((bogus_len >> 16) & 0xFF);
    wire[2] = static_cast<uint8_t>((bogus_len >>  8) & 0xFF);
    wire[3] = static_cast<uint8_t>((bogus_len      ) & 0xFF);

    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    LengthPrefixCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

// ---------------------------------------------------------------------------
// encode + round trip
// ---------------------------------------------------------------------------

TEST(LengthPrefixCodec, EncodeWritesBigEndianPrefix) {
    uint8_t buf[32]{};
    LengthPrefixCodec codec;
    const uint8_t payload[] = {0xAA, 0xBB, 0xCC};
    auto r = codec.encode(buf, sizeof(buf),
                          std::span<const uint8_t>{payload, 3});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 7u);
    EXPECT_EQ(buf[0], 0);
    EXPECT_EQ(buf[1], 0);
    EXPECT_EQ(buf[2], 0);
    EXPECT_EQ(buf[3], 3);
    EXPECT_EQ(buf[4], 0xAA);
    EXPECT_EQ(buf[6], 0xCC);
}

TEST(LengthPrefixCodec, EncodeBufferFull) {
    uint8_t buf[5]{};  // header fits, payload doesn't
    LengthPrefixCodec codec;
    const uint8_t payload[] = {1, 2, 3};
    auto r = codec.encode(buf, sizeof(buf),
                          std::span<const uint8_t>{payload, 3});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
}

TEST(LengthPrefixCodec, RoundTrip) {
    LengthPrefixCodec encoder;
    const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    uint8_t wire[16]{};
    auto n = encoder.encode(wire, sizeof(wire),
                             std::span<const uint8_t>{payload, 5});
    ASSERT_TRUE(n.has_value());

    LengthPrefixCodec decoder;
    SpanPacketView view{wire, *n};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};
    auto r = decoder.decode(view, out);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    ASSERT_EQ((**r).size(), 5u);
    EXPECT_EQ(std::memcmp((**r).data(), payload, 5), 0);
}
