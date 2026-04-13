/// @file test_ws_codec_edge.cpp
/// Supplemental edge-case tests for WsCodec — oversized messages, extended
/// lengths, reserved opcodes, invalid close frames, and boundary conditions.
///
/// These complement the core test_ws_codec.cpp which covers the happy path
/// and basic protocol violations.

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

static constexpr uint8_t kOpBinary      = 0x2;
static constexpr uint8_t kOpClose       = 0x8;
static constexpr uint8_t kOpPing        = 0x9;
static constexpr uint8_t kOpContinuation = 0x0;
static constexpr uint8_t kFinBit        = 0x80;

// Build a frame with payload_len < 126 (simple header)
static std::vector<uint8_t> mk_frame(uint8_t opcode, bool fin,
                                      std::vector<uint8_t> payload) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>((fin ? kFinBit : 0) | (opcode & 0x0F)));
    out.push_back(static_cast<uint8_t>(payload.size() & 0x7F));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Build a frame with 16-bit extended length (payload_len = 126..65535)
static std::vector<uint8_t> mk_frame_ext16(uint8_t opcode, bool fin,
                                            std::vector<uint8_t> payload) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>((fin ? kFinBit : 0) | (opcode & 0x0F)));
    out.push_back(126);  // signals 16-bit extended length
    uint16_t len = static_cast<uint16_t>(payload.size());
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ---------------------------------------------------------------------------
// Oversized single-frame message → CodecOverflow
// ---------------------------------------------------------------------------

TEST(WsCodecEdge, OversizedSingleFrameReturnsCodecOverflow) {
    // Configure a tiny max_message_size for this test
    WsCodec codec{WsCodecConfig{.max_message_size = 10}};

    // Build a frame with 20 bytes of payload (exceeds limit of 10)
    std::vector<uint8_t> payload(20, 0x42);
    auto wire = mk_frame(kOpBinary, /*fin=*/true, payload);

    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

TEST(WsCodecEdge, ExactMaxSizeSucceeds) {
    WsCodec codec{WsCodecConfig{.max_message_size = 5}};

    std::vector<uint8_t> payload(5, 0xAA);
    auto wire = mk_frame(kOpBinary, /*fin=*/true, payload);

    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((**r).size(), 5u);
}

TEST(WsCodecEdge, OneOverMaxSizeFails) {
    WsCodec codec{WsCodecConfig{.max_message_size = 5}};

    std::vector<uint8_t> payload(6, 0xBB);
    auto wire = mk_frame(kOpBinary, /*fin=*/true, payload);

    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

// ---------------------------------------------------------------------------
// Oversized reassembled fragmented message → CodecOverflow
// ---------------------------------------------------------------------------

TEST(WsCodecEdge, OversizedReassembledMessageReturnsCodecOverflow) {
    WsCodec codec{WsCodecConfig{.max_message_size = 8}};

    // Fragment 1: 5 bytes (OK)
    auto f1 = mk_frame(kOpBinary, /*fin=*/false, {1, 2, 3, 4, 5});
    // Fragment 2: 5 bytes → total 10 > max 8 → overflow
    auto f2 = mk_frame(kOpContinuation, /*fin=*/true, {6, 7, 8, 9, 10});

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());

    SpanPacketView view{combined.data(), combined.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    // First fragment: buffered
    auto r1 = codec.decode(view, out);
    ASSERT_TRUE(r1.has_value());
    EXPECT_FALSE(r1->has_value());

    // Second fragment: overflow
    auto r2 = codec.decode(view, out);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, Error::CodecOverflow);
}

// ---------------------------------------------------------------------------
// 16-bit extended length (126..65535 payload)
// ---------------------------------------------------------------------------

TEST(WsCodecEdge, ExtendedLength16BitDecodes) {
    // 200-byte payload, encoded with 16-bit extended length
    std::vector<uint8_t> payload(200, 0xCC);
    auto wire = mk_frame_ext16(kOpBinary, /*fin=*/true, payload);

    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value()) << "error: " << int(r.error().code);
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((**r).size(), 200u);
}

TEST(WsCodecEdge, ExtendedLength16BitIncomplete) {
    // 200-byte payload, but only send header + 50 bytes
    std::vector<uint8_t> payload(200, 0xCC);
    auto wire = mk_frame_ext16(kOpBinary, /*fin=*/true, payload);
    // Truncate to header (4 bytes) + 50 payload = 54
    const size_t truncated = 54;

    SpanPacketView view{wire.data(), truncated};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value()); // incomplete
}

// ---------------------------------------------------------------------------
// All reserved opcodes (0x3-0x7, 0xB-0xF) → WsFrameBad
// ---------------------------------------------------------------------------

class WsCodecReservedOpcode : public ::testing::TestWithParam<uint8_t> {};

TEST_P(WsCodecReservedOpcode, ReservedOpcodeIsRejected) {
    uint8_t opcode = GetParam();
    uint8_t wire[] = {
        static_cast<uint8_t>(kFinBit | opcode),
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

INSTANTIATE_TEST_SUITE_P(
    AllReservedOpcodes,
    WsCodecReservedOpcode,
    ::testing::Values(0x3, 0x4, 0x5, 0x6, 0x7, 0xB, 0xC, 0xD, 0xE, 0xF));

// ---------------------------------------------------------------------------
// Close frame edge cases
// ---------------------------------------------------------------------------

TEST(WsCodecEdge, CloseFrameWithNoPayload) {
    // A close frame with 0-byte payload is valid (no status code).
    auto wire = mk_frame(kOpClose, /*fin=*/true, {});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[64]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsCloseReceived);
    // Should have written a close-ack with code=1000 (kNormal)
    ASSERT_GT(out.size(), 0u);
}

TEST(WsCodecEdge, CloseFrameWithInvalidCode1004) {
    // 1004 is reserved per RFC 6455 §7.4.1
    auto wire = mk_frame(kOpClose, /*fin=*/true, {0x03, 0xEC}); // 1004 BE
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[64]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsCloseReceived);
    // Close-ack should use kProtocolError (1002), not echo 1004
    ASSERT_GT(out.size(), 0u);
}

TEST(WsCodecEdge, CloseFrameWithValidCode1001) {
    // 1001 = Going Away, valid
    auto wire = mk_frame(kOpClose, /*fin=*/true, {0x03, 0xE9}); // 1001 BE
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[64]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsCloseReceived);
}

TEST(WsCodecEdge, CloseFrameWithStatusAndReason) {
    // Close with status 1000 + UTF-8 reason "bye"
    auto wire = mk_frame(kOpClose, /*fin=*/true, {0x03, 0xE8, 'b', 'y', 'e'});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[64]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::WsCloseReceived);
}

// ---------------------------------------------------------------------------
// Ping with maximum control-frame payload (125 bytes)
// ---------------------------------------------------------------------------

TEST(WsCodecEdge, PingWithMaxPayload) {
    std::vector<uint8_t> payload(125, 0x55);
    auto wire = mk_frame(kOpPing, /*fin=*/true, payload);
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[256]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value()); // ping → None
    EXPECT_GT(out.size(), 0u);    // pong generated
    EXPECT_EQ(view.length(), 0u);
}

// ---------------------------------------------------------------------------
// Empty data frame
// ---------------------------------------------------------------------------

TEST(WsCodecEdge, EmptyDataFrameReturnsEmptySpan) {
    auto wire = mk_frame(kOpBinary, /*fin=*/true, {});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[8]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;
    auto r = codec.decode(view, out);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((**r).size(), 0u);
}

// ---------------------------------------------------------------------------
// max_message_size = 0 rejects everything
// ---------------------------------------------------------------------------

TEST(WsCodecEdge, ZeroMaxMessageSizeRejectsAllData) {
    WsCodec codec{WsCodecConfig{.max_message_size = 0}};

    auto wire = mk_frame(kOpBinary, /*fin=*/true, {0x01});
    SpanPacketView view{wire.data(), wire.size()};
    uint8_t out_storage[8]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    auto r = codec.decode(view, out);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

// ---------------------------------------------------------------------------
// Multiple frames in a single buffer
// ---------------------------------------------------------------------------

TEST(WsCodecEdge, TwoFramesInOneBuffer) {
    auto f1 = mk_frame(kOpBinary, /*fin=*/true, {0xAA});
    auto f2 = mk_frame(kOpBinary, /*fin=*/true, {0xBB});

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());

    SpanPacketView view{combined.data(), combined.size()};
    uint8_t out_storage[16]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;

    // First call: get first frame
    auto r1 = codec.decode(view, out);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value());
    EXPECT_EQ((**r1).size(), 1u);
    EXPECT_EQ((**r1)[0], 0xAA);

    // Second call: get second frame
    auto r2 = codec.decode(view, out);
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r2->has_value());
    EXPECT_EQ((**r2).size(), 1u);
    EXPECT_EQ((**r2)[0], 0xBB);

    // Nothing left
    EXPECT_EQ(view.length(), 0u);
}

// ---------------------------------------------------------------------------
// Interleaved ping in data flow
// ---------------------------------------------------------------------------

TEST(WsCodecEdge, PingBetweenDataFrames) {
    auto d1 = mk_frame(kOpBinary, /*fin=*/true, {'A'});
    auto p  = mk_frame(kOpPing,   /*fin=*/true, {});
    auto d2 = mk_frame(kOpBinary, /*fin=*/true, {'B'});

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), d1.begin(), d1.end());
    combined.insert(combined.end(), p.begin(), p.end());
    combined.insert(combined.end(), d2.begin(), d2.end());

    SpanPacketView view{combined.data(), combined.size()};
    uint8_t out_storage[64]{};
    OutputBuffer out{out_storage, sizeof(out_storage)};

    WsCodec codec;

    // Data frame 1
    auto r1 = codec.decode(view, out);
    ASSERT_TRUE(r1.has_value() && r1->has_value());
    EXPECT_EQ((**r1)[0], 'A');

    // Ping → None + auto-pong
    auto r2 = codec.decode(view, out);
    ASSERT_TRUE(r2.has_value());
    EXPECT_FALSE(r2->has_value());

    // Data frame 2
    auto r3 = codec.decode(view, out);
    ASSERT_TRUE(r3.has_value() && r3->has_value());
    EXPECT_EQ((**r3)[0], 'B');
}
