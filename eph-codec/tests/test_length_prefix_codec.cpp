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

// Symmetric encode-then-decode roundtrip with empty payload — the
// decoded Frame must point to data+4 and have payload_len=0 (the
// `DecodeZeroLengthFrameAllowed` test already covers this independently
// but with hand-built bytes; round-trip pins encode/decode together).
TEST(LengthPrefixCodec, EncodeDecodeEmptyPayloadRoundtrip) {
    uint8_t buf[8]{};
    LengthPrefixCodec codec;
    auto enc = codec.encode(buf, sizeof(buf), std::span<const uint8_t>{});
    ASSERT_TRUE(enc.has_value());
    EXPECT_EQ(*enc, 4u);

    SpanPacketView view{buf, 4};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};
    auto dec = codec.decode(view, out);
    ASSERT_TRUE(dec.has_value());
    ASSERT_TRUE(dec->has_value());
    EXPECT_EQ((*dec)->size(), 0u);
}

// Encode buffer EXACTLY equal to header+payload size — the `>` boundary
// check (`needed > cap`) must accept; pins it's not a `>=` regression.
TEST(LengthPrefixCodec, EncodeExactBufferSizeAccepted) {
    uint8_t buf[7]{};   // 4 header + 3 payload
    LengthPrefixCodec codec;
    const uint8_t payload[] = {0x01, 0x02, 0x03};
    auto r = codec.encode(buf, sizeof(buf),
                          std::span<const uint8_t>{payload, 3});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 7u);
    EXPECT_EQ(buf[3], 3);                     // len byte
    EXPECT_EQ(buf[6], 0x03);                  // last payload byte
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

TEST(LengthPrefixCodec, EncodeOversizePayloadReturnsCodecOverflow) {
    // The encode-side mirror of `DecodeOversizeReturnsCodecOverflow`.
    // Pre-flight rejection of bogusly-large payloads keeps callers from
    // emitting a header the decoder would refuse — i.e. the symmetric
    // contract that a frame any valid encoder produces is always
    // accepted by a valid decoder. We don't actually allocate the
    // (>16 MiB) backing storage; encode() rejects on payload.size()
    // before reading any bytes, so a bogus span pointing at nullptr
    // with len = kMaxFrameLen + 1 exercises the same path safely.
    uint8_t buf[16]{};
    LengthPrefixCodec codec;
    std::span<const uint8_t> oversize{
        static_cast<const uint8_t*>(nullptr),
        LengthPrefixCodec::kMaxFrameLen + 1};
    auto r = codec.encode(buf, sizeof(buf), oversize);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

TEST(LengthPrefixCodec, EncodeEmptyPayloadProducesFourByteZeroHeader) {
    // Empty payload is a legal frame on the wire (zero-length data
    // notification). The encoder must produce exactly 4 bytes — the
    // BE uint32 prefix of zero — and not touch any byte past it. The
    // matching decode path is covered by `DecodeZeroLengthFrameAllowed`;
    // this locks the symmetric encode contract.
    uint8_t buf[16];
    std::memset(buf, 0xCC, sizeof(buf));  // sentinel
    LengthPrefixCodec codec;
    auto r = codec.encode(buf, sizeof(buf), std::span<const uint8_t>{});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 4u);
    EXPECT_EQ(buf[0], 0);
    EXPECT_EQ(buf[1], 0);
    EXPECT_EQ(buf[2], 0);
    EXPECT_EQ(buf[3], 0);
    // Sentinel must survive past the 4-byte header.
    EXPECT_EQ(buf[4], 0xCCu);
    EXPECT_EQ(buf[15], 0xCCu);

    // Round-trip: decode the just-encoded zero-length frame back.
    LengthPrefixCodec decoder;
    SpanPacketView view{buf, *r};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};
    auto dr = decoder.decode(view, out);
    ASSERT_TRUE(dr.has_value());
    ASSERT_TRUE(dr->has_value());
    EXPECT_EQ((**dr).size(), 0u);
    // View fully consumed.
    EXPECT_EQ(view.length(), 0u);
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
