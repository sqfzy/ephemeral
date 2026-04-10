/// @file test_mold64_codec.cpp
/// Unit tests for `eph::codec::Mold64Codec`.

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/mold64_codec.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"
#include "eph/itch/moldudp64.hpp"

using eph::codec::Mold64Codec;
using eph::codec::Mold64Config;
using eph::codec::SpanPacketView;
using eph::core::DatagramCodec;
using eph::core::Error;
using eph::core::OutputBuffer;

static_assert(DatagramCodec<Mold64Codec>,
              "Mold64Codec must satisfy DatagramCodec");

// ---------------------------------------------------------------------------
// Helper: build a valid MoldUDP64 datagram with N messages.
// Layout: 10 session bytes | 8 seq (BE) | 2 count (BE) | N × [2 len BE + body]
// ---------------------------------------------------------------------------
static std::vector<uint8_t> build_mold64(
    const char session[10],
    uint64_t    seq,
    const std::vector<std::vector<uint8_t>>& messages)
{
    std::vector<uint8_t> out;
    out.resize(eph::itch::moldudp64::kHeaderLen);
    // Session
    std::memcpy(out.data(), session, 10);
    // Seq (BE)
    for (int i = 0; i < 8; ++i) {
        out[10 + i] = static_cast<uint8_t>((seq >> ((7 - i) * 8)) & 0xFF);
    }
    // Count (BE)
    const auto count = static_cast<uint16_t>(messages.size());
    out[18] = static_cast<uint8_t>((count >> 8) & 0xFF);
    out[19] = static_cast<uint8_t>(count & 0xFF);
    for (const auto& m : messages) {
        uint16_t len = static_cast<uint16_t>(m.size());
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
        out.insert(out.end(), m.begin(), m.end());
    }
    return out;
}

// Build an end-of-session marker datagram (count == 0xFFFF).
static std::vector<uint8_t> build_mold64_eos(const char session[10], uint64_t seq) {
    std::vector<uint8_t> out(eph::itch::moldudp64::kHeaderLen);
    std::memcpy(out.data(), session, 10);
    for (int i = 0; i < 8; ++i) {
        out[10 + i] = static_cast<uint8_t>((seq >> ((7 - i) * 8)) & 0xFF);
    }
    out[18] = 0xFF;
    out[19] = 0xFF;
    return out;
}

// ---------------------------------------------------------------------------
// decode — normal delivery
// ---------------------------------------------------------------------------

TEST(Mold64Codec, DecodeDeliversAllMessagesInOrder) {
    auto wire = build_mold64("SESS000001", /*seq=*/1,
        {{0x41, 0x42}, {0x43, 0x44, 0x45}, {0x46}});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    std::vector<Mold64Codec::Frame> delivered;
    Mold64Codec codec;
    auto r = codec.decode(view, out, [&](Mold64Codec::Frame f) {
        delivered.push_back(f);
    });
    ASSERT_TRUE(r.has_value()) << "code=" << int(r.error().code);
    EXPECT_EQ(*r, 3u);
    ASSERT_EQ(delivered.size(), 3u);
    EXPECT_EQ(delivered[0].seq_num, 1u);
    EXPECT_EQ(delivered[0].payload.size(), 2u);
    EXPECT_EQ(delivered[0].payload[0], 0x41);
    EXPECT_EQ(delivered[1].seq_num, 2u);
    EXPECT_EQ(delivered[1].payload.size(), 3u);
    EXPECT_EQ(delivered[2].seq_num, 3u);
    EXPECT_EQ(delivered[2].payload.size(), 1u);
    EXPECT_EQ(delivered[2].payload[0], 0x46);
    EXPECT_EQ(view.length(), 0u);

    // Expected seq advanced, no gaps.
    EXPECT_EQ(codec.expected_seq(), 4u);
    EXPECT_EQ(codec.gap_count(), 0u);
}

// ---------------------------------------------------------------------------
// decode — gap detection
// ---------------------------------------------------------------------------

TEST(Mold64Codec, DecodeDetectsGap) {
    Mold64Codec codec;

    // First datagram seq 1..2 — steady state.
    auto w1 = build_mold64("SESS000001", 1, {{0x01}, {0x02}});
    SpanPacketView v1{w1.data(), w1.size()};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};
    auto r1 = codec.decode(v1, out, [](auto) {});
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, 2u);
    EXPECT_EQ(codec.expected_seq(), 3u);
    EXPECT_EQ(codec.gap_count(), 0u);

    // Second datagram starts at seq 7 — gap of 4 (expected was 3, got 7).
    auto w2 = build_mold64("SESS000001", 7, {{0x07}});
    SpanPacketView v2{w2.data(), w2.size()};
    auto r2 = codec.decode(v2, out, [](auto) {});
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, 1u);
    EXPECT_EQ(codec.gap_count(), 4u);
    EXPECT_EQ(codec.expected_seq(), 8u);
}

// ---------------------------------------------------------------------------
// decode — duplicate/replay
// ---------------------------------------------------------------------------

TEST(Mold64Codec, DecodeReplayPacketDoesNotRewindExpectedSeq) {
    Mold64Codec codec;
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    auto w1 = build_mold64("SESS000001", 1, {{1}, {2}, {3}});
    SpanPacketView v1{w1.data(), w1.size()};
    (void)codec.decode(v1, out, [](auto) {});
    EXPECT_EQ(codec.expected_seq(), 4u);

    // Replay seq=2 — deliver but don't rewind.
    auto w2 = build_mold64("SESS000001", 2, {{2}});
    SpanPacketView v2{w2.data(), w2.size()};
    int hits = 0;
    auto r = codec.decode(v2, out, [&](auto) { ++hits; });
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(codec.expected_seq(), 4u);  // unchanged
    EXPECT_EQ(codec.gap_count(), 0u);     // replay is not a gap
}

// ---------------------------------------------------------------------------
// decode — end of session marker
// ---------------------------------------------------------------------------

TEST(Mold64Codec, DecodeEndOfSessionDeliversZero) {
    Mold64Codec codec;
    auto wire = build_mold64_eos("SESS000001", 100);
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    int hits = 0;
    auto r = codec.decode(view, out, [&](auto) { ++hits; });
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0u);
    EXPECT_EQ(hits, 0);
    EXPECT_EQ(view.length(), 0u);
}

// ---------------------------------------------------------------------------
// decode — malformed / truncated header
// ---------------------------------------------------------------------------

TEST(Mold64Codec, DecodeTruncatedHeaderReturnsCodecBad) {
    uint8_t wire[10] = {0};  // shorter than 20-byte header
    SpanPacketView view{wire, sizeof(wire)};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    Mold64Codec codec;
    auto r = codec.decode(view, out, [](auto) {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
    // Contract: datagram codec must still consume on error.
    EXPECT_EQ(view.length(), 0u);
}

// ---------------------------------------------------------------------------
// encode + round trip
// ---------------------------------------------------------------------------

TEST(Mold64Codec, EncodeRoundTrip) {
    Mold64Codec enc;
    const uint8_t msg[] = {0xDE, 0xAD, 0xBE, 0xEF};
    Mold64Codec::Frame in_frame{
        .seq_num = 42,
        .payload = std::span<const uint8_t>{msg, 4},
    };
    uint8_t buf[64]{};
    auto n = enc.encode(buf, sizeof(buf), in_frame);
    ASSERT_TRUE(n.has_value());

    // Round trip: decode it back. Start the decoder's expected_seq at 42
    // so no gap is reported.
    Mold64Codec dec{Mold64Config{.initial_expected_seq = 42}};
    SpanPacketView view{buf, *n};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    std::vector<Mold64Codec::Frame> got;
    auto r = dec.decode(view, out,
                         [&](Mold64Codec::Frame f) { got.push_back(f); });
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1u);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].seq_num, 42u);
    ASSERT_EQ(got[0].payload.size(), 4u);
    EXPECT_EQ(std::memcmp(got[0].payload.data(), msg, 4), 0);
    EXPECT_EQ(dec.gap_count(), 0u);
}

TEST(Mold64Codec, EncodeBufferFull) {
    Mold64Codec enc;
    const uint8_t msg[] = {0xAA, 0xBB};
    uint8_t buf[10]{};  // too small
    auto r = enc.encode(buf, sizeof(buf), Mold64Codec::Frame{
        .seq_num = 1,
        .payload = std::span<const uint8_t>{msg, 2},
    });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
}
