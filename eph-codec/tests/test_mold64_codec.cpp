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

// Payload-size check must run BEFORE `needed = 22 + payload.size()` to
// prevent a SIZE_MAX-class wrap from slipping past the cap guard. We
// can't materialise a SIZE_MAX-byte buffer, so we just assert that an
// oversized span (size > 0xFFFF) is rejected with CodecOverflow even
// when the destination buffer would be "large enough" by wrapped math.
TEST(Mold64Codec, EncodeRejectsOversizedPayloadBeforeSizeMath) {
    Mold64Codec enc;
    uint8_t buf[256]{};
    // Synthesise a span whose size exceeds the 16-bit on-wire limit.
    // pointer is dummy (encode rejects before any read of payload data).
    const uint8_t dummy[1]{};
    std::span<const uint8_t> oversized{dummy, std::size_t{0x10000}};
    auto r = enc.encode(buf, sizeof(buf), Mold64Codec::Frame{
        .seq_num = 1,
        .payload = oversized,
    });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

// ─── round-53 gap-detection boundary additions ─────────────────────
//
// Existing tests cover gap-of-4, replay, EOS (0xFFFF count), but these
// boundary cases were untested:
//
//   - Heartbeat datagram (count == 0, but NOT EOS) — a degenerate but
//     valid-on-the-wire MoldUDP64 form.
//   - Gap of exactly 1 — pins the strict `>` in "got > expected" check
//     so a refactor to `>=` would not silently treat steady-state as a
//     1-msg gap.
//   - encode at exact-fit cap — pin the strict `>` rejection guard.
//   - decode does NOT mutate `expected_seq_` past the rewind boundary
//     when `seq + delivered <= expected_seq_` (i.e. a partial replay).

TEST(Mold64Codec, DecodeHeartbeatDatagramCountZeroDeliversNothing) {
    // count == 0 is a valid wire form (e.g. a heartbeat / silence
    // marker between message bursts); only count == 0xFFFF is EOS.
    // The existing implementation routes this to the for-loop with
    // 0 iterations → 0 frames delivered, expected_seq_ stays put.
    Mold64Codec codec;
    auto wire = build_mold64("SESS000001", /*seq=*/100, /*messages=*/{});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    int hits = 0;
    auto r = codec.decode(view, out, [&](auto) { ++hits; });
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0u);
    EXPECT_EQ(hits, 0);
    EXPECT_EQ(view.length(), 0u);
    // gap of (100 - 1) = 99 IS surfaced even with no payload — the
    // sequence-number jump alone counts as a gap, regardless of the
    // count. This pins that side of the contract too.
    EXPECT_EQ(codec.gap_count(), 99u);
}

TEST(Mold64Codec, DecodeGapOfOneMessageDetected) {
    // Pin the strict `>` boundary at expected_seq_ comparison: with
    // initial expected=1 and incoming seq=2, the codec must record a
    // gap of 1 (not 0).
    Mold64Codec codec;
    auto wire = build_mold64("SESS000001", /*seq=*/2, {{0x55}});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    auto r = codec.decode(view, out, [](auto) {});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1u);
    EXPECT_EQ(codec.gap_count(), 1u)
        << "gap of exactly 1 must be detected; the strict `>` guard "
           "must distinguish steady-state (==) from gap (>)";
    EXPECT_EQ(codec.expected_seq(), 3u);
}

TEST(Mold64Codec, EncodeExactlyFillsBuffer) {
    // header (20) + length-prefix (2) + payload (4) = 26 bytes
    Mold64Codec enc;
    const uint8_t msg[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t buf[26]{};
    auto r = enc.encode(buf, sizeof(buf), Mold64Codec::Frame{
        .seq_num = 1,
        .payload = std::span<const uint8_t>{msg, 4},
    });
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 26u);
}

TEST(Mold64Codec, DecodeReplayWithPartialOverlapPreservesExpectedSeq) {
    // After receiving seq 1..3 (expected=4), a replay of seq 2 with
    // 2 messages would deliver seq 2 and 3 again. The codec's advance
    // guard at line 170 uses `>` — so 2+2 = 4, NOT > 4, expected_seq_
    // stays at 4. Pins that boundary precisely.
    Mold64Codec codec;
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    auto w1 = build_mold64("SESS000001", 1, {{1}, {2}, {3}});
    SpanPacketView v1{w1.data(), w1.size()};
    (void)codec.decode(v1, out, [](auto) {});
    EXPECT_EQ(codec.expected_seq(), 4u);

    // Replay seq=2 with 2 messages — 2+2=4, NOT > 4 → no rewind.
    auto w2 = build_mold64("SESS000001", 2, {{2}, {3}});
    SpanPacketView v2{w2.data(), w2.size()};
    int hits = 0;
    auto r = codec.decode(v2, out, [&](auto) { ++hits; });
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(hits, 2);
    EXPECT_EQ(codec.expected_seq(), 4u)
        << "boundary case 2+2==4 must NOT trigger advance "
           "(guard uses `>`, not `>=`)";
    EXPECT_EQ(codec.gap_count(), 0u);
}
