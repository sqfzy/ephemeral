/// @file test_raw_datagram_codec.cpp
/// Unit tests for `eph::codec::RawDatagramCodec`.

#include <cstdint>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/raw_datagram_codec.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"

using eph::codec::RawDatagramCodec;
using eph::codec::SpanPacketView;
using eph::core::DatagramCodec;
using eph::core::Error;
using eph::core::OutputBuffer;

static_assert(DatagramCodec<RawDatagramCodec>,
              "RawDatagramCodec must satisfy DatagramCodec");

// ---------------------------------------------------------------------------
// decode
// ---------------------------------------------------------------------------

TEST(RawDatagramCodec, DecodeEmitsOneFramePerDatagram) {
    std::vector<uint8_t> dgram{1, 2, 3, 4};
    SpanPacketView view{dgram.data(), dgram.size()};
    uint8_t out_storage[8]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    std::vector<std::vector<uint8_t>> sunk;
    auto sink = [&](std::span<const uint8_t> f) {
        sunk.emplace_back(f.begin(), f.end());
    };

    RawDatagramCodec codec;
    auto r = codec.decode(view, out, sink);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1u);
    ASSERT_EQ(sunk.size(), 1u);
    ASSERT_EQ(sunk[0].size(), 4u);
    EXPECT_EQ(sunk[0][0], 1);
    EXPECT_EQ(sunk[0][3], 4);
    // Datagram must be fully consumed.
    EXPECT_EQ(view.length(), 0u);
}

TEST(RawDatagramCodec, DecodeEmptyDatagramReturnsCodecBad) {
    uint8_t storage[1]{};
    SpanPacketView view{storage, 0};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    int sink_hits = 0;
    RawDatagramCodec codec;
    auto r = codec.decode(view, out, [&](auto) { ++sink_hits; });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
    EXPECT_EQ(sink_hits, 0);
}

// ---------------------------------------------------------------------------
// encode + round trip
// ---------------------------------------------------------------------------

TEST(RawDatagramCodec, EncodeCopiesPayload) {
    uint8_t buf[8]{};
    RawDatagramCodec codec;
    const uint8_t payload[] = {9, 8, 7};
    auto r = codec.encode(buf, sizeof(buf),
                          std::span<const uint8_t>{payload, 3});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 3u);
    EXPECT_EQ(buf[0], 9);
    EXPECT_EQ(buf[2], 7);
}

TEST(RawDatagramCodec, EncodeBufferFull) {
    uint8_t buf[2]{};
    RawDatagramCodec codec;
    const uint8_t payload[] = {1, 2, 3};
    auto r = codec.encode(buf, sizeof(buf),
                          std::span<const uint8_t>{payload, 3});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
}
