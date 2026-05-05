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

// ─── round-52 boundary additions ─────────────────────────────────────
//
// Gaps in the existing matrix: empty-payload encode (no-op memcpy
// path), exact-fit boundary at `payload.size() == cap` (strict `>` in
// the rejection guard), error-message detail, and an end-to-end
// round trip that exercises encode→decode.

TEST(RawDatagramCodec, EncodeEmptyPayloadReturnsZero) {
    // The `if (!payload.empty())` guard short-circuits memcpy; verify
    // the path returns 0 without touching `buf`. Also exercises the
    // `payload.size() > cap` branch with cap=0 / payload.size()=0
    // (0 > 0 is false → success).
    uint8_t sentinel = 0xAB;
    RawDatagramCodec codec;
    auto r = codec.encode(&sentinel, /*cap=*/0,
                          std::span<const uint8_t>{});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0u);
    EXPECT_EQ(sentinel, 0xAB) << "buf must NOT have been written";
}

TEST(RawDatagramCodec, EncodeExactlyFillsBuffer) {
    // Pin the strict `>` in the guard at line 92: `payload.size()
    // == cap` must succeed.
    uint8_t buf[3]{};
    RawDatagramCodec codec;
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE};
    auto r = codec.encode(buf, sizeof(buf),
                          std::span<const uint8_t>{payload, 3});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 3u);
    EXPECT_EQ(buf[0], 0xDE);
    EXPECT_EQ(buf[1], 0xAD);
    EXPECT_EQ(buf[2], 0xBE);
}

TEST(RawDatagramCodec, DecodeEmptyDatagramErrorMessageMentionsEmpty) {
    // Per CLAUDE rules, error messages must be actionable. Pin the
    // substring so a refactor that changes the detail text doesn't
    // silently degrade to a vague "decode failed".
    uint8_t storage[1]{};
    SpanPacketView view{storage, 0};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};
    RawDatagramCodec codec;

    auto r = codec.decode(view, out, [](auto) {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
    EXPECT_NE(std::string_view(r.error().detail).find("empty"),
              std::string_view::npos)
        << "actual: " << r.error().detail;
}

TEST(RawDatagramCodec, EncodeDecodeRoundTrip) {
    // End-to-end check that encode + decode preserve bytes exactly.
    // The codec's identity contract (1 datagram = 1 frame, no
    // overhead) means the encode output equals the decode input
    // byte-for-byte.
    uint8_t buf[16]{};
    RawDatagramCodec codec;
    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40, 0x50};

    auto enc = codec.encode(buf, sizeof(buf),
                             std::span<const uint8_t>{payload, 5});
    ASSERT_TRUE(enc.has_value());
    EXPECT_EQ(*enc, 5u);

    SpanPacketView view{buf, *enc};
    uint8_t out_storage[8]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};
    std::vector<uint8_t> received;
    auto dec = codec.decode(view, out, [&](std::span<const uint8_t> f) {
        received.assign(f.begin(), f.end());
    });
    ASSERT_TRUE(dec.has_value());
    EXPECT_EQ(*dec, 1u);
    ASSERT_EQ(received.size(), 5u);
    EXPECT_EQ(received[0], 0x10);
    EXPECT_EQ(received[4], 0x50);
}

// ---------------------------------------------------------------------------
// Empty std::function sink — R56 contract
// ---------------------------------------------------------------------------

TEST(RawDatagramCodec, DecodeRejectsEmptySinkWithInvalidConfig) {
    // R56: an empty std::function would throw bad_function_call inside
    // the decode callback path, terminating the process via the
    // noexcept boundary. The codec must reject with a typed
    // InvalidConfig instead. Note: per the DatagramCodec contract,
    // the codec consumes the datagram (trim_front) BEFORE the sink
    // is invoked, so the input span IS drained on the rejection
    // path; the typed error is the only signal back to the caller.
    std::vector<uint8_t> dgram{0xCA, 0xFE, 0xBA, 0xBE};
    SpanPacketView view{dgram.data(), dgram.size()};
    uint8_t out_storage[8]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    std::function<void(std::span<const uint8_t>)> empty_sink{};
    RawDatagramCodec codec;
    auto r = codec.decode(view, out, empty_sink);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
    // Documented contract: trim_front happens before the sink check,
    // so the datagram IS consumed on this rejection path.
    EXPECT_EQ(view.length(), 0u);
}

TEST(RawDatagramCodec, DecodeRejectsMovedFromSink) {
    // After std::move(sink), the source is in a no-target state. The
    // codec's empty-sink defense must catch this same shape.
    std::function<void(std::span<const uint8_t>)> sink = [](auto) {};
    EXPECT_TRUE(static_cast<bool>(sink));
    auto sink_moved_out = std::move(sink);  // sink now empty
    EXPECT_FALSE(static_cast<bool>(sink));

    std::vector<uint8_t> dgram{0x01};
    SpanPacketView view{dgram.data(), dgram.size()};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    RawDatagramCodec codec;
    auto r = codec.decode(view, out, sink);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}
