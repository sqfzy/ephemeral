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

// Regression: after a fragmented message has been delivered, frag_buf_ is
// deliberately retained (so the returned span stays valid until the next
// decode() call) but frag_opcode_ is reset to 0. A subsequent orphan
// continuation must still be rejected — earlier code conjuncted the two
// signals (`frag_buf_.empty() && frag_opcode_ == 0`) and silently appended
// the orphan payload to the stale buffer, delivering a corrupted message
// to the application.
TEST(WsCodec, OrphanContinuationAfterFragmentedMessageIsRejected) {
    auto f1 = mk_frame(kOpBinary,       /*fin=*/false, {'A', 'B'});
    auto f2 = mk_frame(kOpContinuation, /*fin=*/true,  {'C', 'D'});
    auto orphan = mk_frame(kOpContinuation, /*fin=*/true, {'X', 'Y'});

    std::vector<uint8_t> wire;
    wire.insert(wire.end(), f1.begin(), f1.end());
    wire.insert(wire.end(), f2.begin(), f2.end());
    wire.insert(wire.end(), orphan.begin(), orphan.end());

    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;

    // First fragment buffers, returns None.
    auto r1 = codec.decode(view, out);
    ASSERT_TRUE(r1.has_value());
    EXPECT_FALSE(r1->has_value());

    // Final fragment delivers reassembled "ABCD".
    auto r2 = codec.decode(view, out);
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r2->has_value());
    EXPECT_EQ((**r2).size(), 4u);

    // Orphan continuation that follows MUST be rejected as a protocol
    // violation, not silently accumulated onto the stale frag_buf_.
    auto r3 = codec.decode(view, out);
    ASSERT_FALSE(r3.has_value());
    EXPECT_EQ(r3.error().code, Error::WsFrameBad);
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

// Pins down the wire-level opcode contract for `encode()`. Closes the
// gap where prior tests round-tripped through decode() but did not
// observe byte 0 directly — a future refactor that flipped the opcode
// constant would round-trip cleanly (decode is opcode-agnostic for
// data frames) and ship a silent regression. This test fails on any
// such flip.
TEST(WsCodec, EncodeBinary_FirstByteIsFinPlusBinaryOpcode) {
    WsCodec encoder;
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};

    uint8_t wire[32]{};
    auto n = encoder.encode(wire, sizeof(wire),
                             std::span<const uint8_t>{payload, 4});
    ASSERT_TRUE(n.has_value());
    ASSERT_GT(*n, 4u);

    // Byte 0: FIN=1 (high bit) + opcode=Binary(0x2) → 0x82.
    // Both the magic-byte form (catches accidental opcode flips) and
    // the named-constant form (catches accidental opcode constant
    // renumbering) — double belt to anchor the contract.
    EXPECT_EQ(wire[0], 0x82) << "actual byte 0: 0x" << std::hex << int(wire[0]);
    EXPECT_EQ(wire[0] & 0x0F, eph::net::ws::opcode::kBinary)
        << "opcode nibble: 0x" << std::hex << int(wire[0] & 0x0F);
    EXPECT_EQ(wire[0] & 0x80, 0x80)  // FIN bit set
        << "FIN bit not set; byte 0 = 0x" << std::hex << int(wire[0]);
}

// ---------------------------------------------------------------------------
// encode_close — RFC 6455 §5.5.1 / §7.1.1
// ---------------------------------------------------------------------------

TEST(WsCodec, EncodeCloseEmitsValidMaskedFrame) {
    WsCodec encoder;
    uint8_t wire[16]{};
    auto n = encoder.encode_close(
        wire, sizeof(wire),
        eph::net::ws::close_code::kNormal);
    ASSERT_TRUE(n.has_value());
    // Header (2) + mask key (4) + 2-byte status code = 8 bytes
    EXPECT_EQ(*n, 8u);
    // Byte 0: FIN=1 + opcode=Close(0x8) → 0x88
    EXPECT_EQ(wire[0], 0x88);
    // Byte 1: MASK=1 + len=2 → 0x82
    EXPECT_EQ(wire[1], 0x82);
}

TEST(WsCodec, EncodeCloseRoundTripStatusCode) {
    WsCodec encoder;
    uint8_t wire[16]{};
    auto n = encoder.encode_close(wire, sizeof(wire), 1011);
    ASSERT_TRUE(n.has_value());

    // Feed through decoder; close auto-acks and surfaces WsCloseReceived.
    WsCodec decoder;
    SpanPacketView view{wire, *n};
    uint8_t out_storage[32]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};
    auto r = decoder.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsCloseReceived);
    // Auto-ack has been written into `out`. Decode that ack to recover the
    // status code (the ack mirrors the peer's code when it's valid; 1011
    // is in the valid range so it round-trips).
    SpanPacketView ack_view{out_storage, out.size()};
    uint8_t out2_storage[32]{};
    OutputBuffer out2{out2_storage, sizeof(out2_storage)};
    WsCodec ack_decoder;
    auto rr = ack_decoder.decode(ack_view, out2);
    ASSERT_FALSE(rr.has_value());
    EXPECT_EQ(rr.error().code, Error::WsCloseReceived);
}

TEST(WsCodec, EncodeCloseWithReason) {
    WsCodec encoder;
    constexpr std::string_view reason = "going away";
    uint8_t wire[64]{};
    auto n = encoder.encode_close(
        wire, sizeof(wire), 1001, reason);
    ASSERT_TRUE(n.has_value());
    // Header (2) + mask (4) + 2-byte status + reason → 8 + 10 = 18
    EXPECT_EQ(*n, 8u + reason.size());
    EXPECT_EQ(wire[0], 0x88);
    EXPECT_EQ(wire[1] & 0x80, 0x80);  // mask bit
    EXPECT_EQ(wire[1] & 0x7F, 2u + reason.size());  // payload length
}

TEST(WsCodec, EncodeCloseTooSmallBufferReturnsBufferFull) {
    WsCodec encoder;
    uint8_t wire[8]{};  // smaller than kMaxFrameHeaderLen (14) + 2
    auto r = encoder.encode_close(
        wire, sizeof(wire), eph::net::ws::close_code::kNormal);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::BufferFull);
}

TEST(WsCodec, EncodeCloseDefaultStatusCodeIsNormal) {
    WsCodec encoder;
    uint8_t wire[16]{};
    auto n = encoder.encode_close(wire, sizeof(wire));
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 8u);
    // Bytes 6-7 are the masked status code; un-mask using key at bytes 2-5
    const uint16_t hi = static_cast<uint16_t>(wire[6] ^ wire[2]);
    const uint16_t lo = static_cast<uint16_t>(wire[7] ^ wire[3]);
    const uint16_t code = static_cast<uint16_t>((hi << 8) | lo);
    EXPECT_EQ(code, eph::net::ws::close_code::kNormal);
}
