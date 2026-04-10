/// @file test_ws_codec.cpp
/// Unit tests for `eph::codec::WsCodec`.
///
/// Tests craft raw WebSocket frames inline (unmasked, server-to-client form)
/// rather than depending on a separate encoder, so that we can exercise
/// edge cases like ping, close, fragmentation, and malformed input without
/// coupling the test to one encode path. Where round-trip is needed, we
/// use WsCodec::encode itself.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/codec/detail/span_packet_view.hpp"
#include "eph/codec/ws_codec.hpp"
#include "eph/core/codec.hpp"
#include "eph/core/error.hpp"

using eph::codec::SpanPacketView;
using eph::codec::WsCodec;
using eph::codec::WsCodecConfig;
using eph::core::Error;
using eph::core::OutputBuffer;
using eph::core::StreamCodec;

static_assert(StreamCodec<WsCodec>,
              "WsCodec must satisfy StreamCodec");

// ---------------------------------------------------------------------------
// Helpers for building raw WS frames (unmasked, single-frame form).
// Frame layout (RFC 6455):
//   Byte 0: FIN(1) | RSV(3) | opcode(4)
//   Byte 1: MASK(1=0) | payload_len(7)
//   Bytes 2..: extended length if 126/127
//   Bytes next: payload
// ---------------------------------------------------------------------------

static constexpr uint8_t kOpText        = 0x1;
static constexpr uint8_t kOpBinary      = 0x2;
static constexpr uint8_t kOpClose       = 0x8;
static constexpr uint8_t kOpPing        = 0x9;
static constexpr uint8_t kOpPong        = 0xA;
static constexpr uint8_t kOpContinuation = 0x0;
static constexpr uint8_t kFinBit        = 0x80;

// Build a single unmasked server-to-client frame with ≤125 payload bytes.
static std::vector<uint8_t> mk_frame(uint8_t opcode, bool fin,
                                      std::vector<uint8_t> payload) {
    std::vector<uint8_t> out;
    out.reserve(2 + payload.size());
    out.push_back(static_cast<uint8_t>((fin ? kFinBit : 0) | (opcode & 0x0F)));
    // Payload length must be < 126 for this helper (keep tests simple).
    out.push_back(static_cast<uint8_t>(payload.size() & 0x7F));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ---------------------------------------------------------------------------
// Concept + basic decode
// ---------------------------------------------------------------------------

TEST(WsCodec, DecodeBinaryDataFrame) {
    auto wire = mk_frame(kOpBinary, /*fin=*/true,
                         {0xDE, 0xAD, 0xBE, 0xEF});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[32]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    auto frame = **r;
    ASSERT_EQ(frame.size(), 4u);
    EXPECT_EQ(frame[0], 0xDE);
    EXPECT_EQ(frame[3], 0xEF);
    EXPECT_EQ(view.length(), 0u);
    EXPECT_EQ(out.size(), 0u) << "Data frame must not generate auto-response";
}

// ---------------------------------------------------------------------------
// Partial / incomplete
// ---------------------------------------------------------------------------

TEST(WsCodec, DecodeIncompleteFrameReturnsNone) {
    auto wire = mk_frame(kOpBinary, /*fin=*/true,
                         {1, 2, 3, 4, 5});
    // Pass only the first 3 bytes (header + 1 payload byte).
    SpanPacketView view{wire.data(), 3};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    EXPECT_EQ(view.length(), 3u);  // not consumed
}

TEST(WsCodec, DecodeHeaderShorterThanTwoReturnsNone) {
    uint8_t wire[1] = {0x82};
    SpanPacketView view{wire, 1};
    uint8_t out_storage[4]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

// ---------------------------------------------------------------------------
// Ping → auto-pong
// ---------------------------------------------------------------------------

TEST(WsCodec, PingTriggersAutoPongAndReturnsNone) {
    // Ping with payload "hi"
    auto wire = mk_frame(kOpPing, /*fin=*/true, {'h', 'i'});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[64]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    EXPECT_EQ(view.length(), 0u);

    // Pong should have been appended. It will be masked (client-side),
    // so we can't trivially compare bytes, but we can at least verify
    // something was written and the first byte is FIN + pong opcode.
    ASSERT_GT(out.size(), 0u);
    EXPECT_EQ(out.data()[0], static_cast<uint8_t>(kFinBit | kOpPong));
}

TEST(WsCodec, PingWithAutoPongDisabledWritesNothing) {
    auto wire = mk_frame(kOpPing, /*fin=*/true, {});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[64]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec{WsCodecConfig{.auto_pong = false}};
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    EXPECT_EQ(out.size(), 0u);
}

// ---------------------------------------------------------------------------
// Pong passthrough
// ---------------------------------------------------------------------------

TEST(WsCodec, PongIsSilentlyConsumed) {
    auto wire = mk_frame(kOpPong, /*fin=*/true, {});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    EXPECT_EQ(view.length(), 0u);
    EXPECT_EQ(out.size(), 0u);
}

// ---------------------------------------------------------------------------
// Close → auto close-ack + error signal
// ---------------------------------------------------------------------------

TEST(WsCodec, CloseFrameTriggersAckAndReturnsWsCloseReceived) {
    // Close frame with status 1000 (normal).
    auto wire = mk_frame(kOpClose, /*fin=*/true,
                         {0x03, 0xE8});  // 1000 BE
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[64]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsCloseReceived);
    EXPECT_EQ(view.length(), 0u);
    // Close-ack should be written.
    ASSERT_GT(out.size(), 0u);
    EXPECT_EQ(out.data()[0], static_cast<uint8_t>(kFinBit | kOpClose));
}

TEST(WsCodec, CloseFrameWithAutoAckDisabled) {
    auto wire = mk_frame(kOpClose, /*fin=*/true, {0x03, 0xE8});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec{WsCodecConfig{.auto_close_ack = false}};
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsCloseReceived);
    EXPECT_EQ(out.size(), 0u);
}

// ---------------------------------------------------------------------------
// Fragmentation reassembly
// ---------------------------------------------------------------------------

TEST(WsCodec, FragmentedMessageReassembles) {
    // First fragment: opcode=binary, FIN=0, payload="AB"
    auto f1 = mk_frame(kOpBinary, /*fin=*/false, {'A', 'B'});
    // Continuation: opcode=continuation (0), FIN=0, payload="CD"
    auto f2 = mk_frame(kOpContinuation, /*fin=*/false, {'C', 'D'});
    // Final: opcode=continuation, FIN=1, payload="E"
    auto f3 = mk_frame(kOpContinuation, /*fin=*/true, {'E'});

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());
    combined.insert(combined.end(), f3.begin(), f3.end());

    SpanPacketView view{combined.data(), combined.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;

    // First frame — buffered, returns None.
    auto r1 = codec.decode(view, out);
    ASSERT_TRUE(r1.has_value());
    EXPECT_FALSE(r1->has_value());

    // Second frame — still buffered, None.
    auto r2 = codec.decode(view, out);
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->has_value());

    // Final fragment — delivers the full reassembled "ABCDE".
    auto r3 = codec.decode(view, out);
    ASSERT_TRUE(r3.has_value());
    ASSERT_TRUE(r3->has_value());
    auto frame = **r3;
    ASSERT_EQ(frame.size(), 5u);
    EXPECT_EQ(frame[0], 'A');
    EXPECT_EQ(frame[1], 'B');
    EXPECT_EQ(frame[2], 'C');
    EXPECT_EQ(frame[3], 'D');
    EXPECT_EQ(frame[4], 'E');
    EXPECT_EQ(view.length(), 0u);
}

TEST(WsCodec, OrphanContinuationIsRejected) {
    // Continuation frame with no preceding initial fragment.
    auto wire = mk_frame(kOpContinuation, /*fin=*/true, {'X'});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsFrameBad);
}

// ---------------------------------------------------------------------------
// Malformed / protocol violations
// ---------------------------------------------------------------------------

TEST(WsCodec, ReservedBitsTriggerWsFrameBad) {
    // Set RSV1 bit (0x40) without negotiated extension.
    uint8_t wire[] = {
        static_cast<uint8_t>(kFinBit | 0x40 | kOpBinary),
        0x02,
        0xAA, 0xBB,
    };
    SpanPacketView view{wire, sizeof(wire)};
    uint8_t out_storage[8]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsFrameBad);
}

TEST(WsCodec, InvalidOpcodeTriggersWsFrameBad) {
    // Opcode 0x3 is reserved (RFC 6455 §5.2).
    uint8_t wire[] = {
        static_cast<uint8_t>(kFinBit | 0x3),
        0x00,
    };
    SpanPacketView view{wire, sizeof(wire)};
    uint8_t out_storage[8]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsFrameBad);
}

// ---------------------------------------------------------------------------
// encode → decode round trip
// ---------------------------------------------------------------------------

TEST(WsCodec, EncodeProducesDecodableFrame) {
    WsCodec encoder;
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8};

    uint8_t wire[32]{};
    auto n = encoder.encode(wire, sizeof(wire),
                             std::span<const uint8_t>{payload, 8});
    ASSERT_TRUE(n.has_value());
    ASSERT_GT(*n, 8u);

    // The encoder produces a client-side masked frame. Feed it back
    // through a fresh decoder and verify we recover the original payload.
    WsCodec decoder;
    SpanPacketView view{wire, *n};
    uint8_t out_storage[32]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    auto r = decoder.decode(view, out);
    ASSERT_TRUE(r.has_value()) << "decode failed: code="
                               << int(r.error().code);
    ASSERT_TRUE(r->has_value());
    auto frame = **r;
    ASSERT_EQ(frame.size(), 8u);
    EXPECT_EQ(std::memcmp(frame.data(), payload, 8), 0);
}

TEST(WsCodec, EncodeBufferFull) {
    WsCodec encoder;
    const uint8_t payload[8] = {};
    uint8_t wire[4]{};  // header alone won't fit
    auto r = encoder.encode(wire, sizeof(wire),
                             std::span<const uint8_t>{payload, 8});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
}
