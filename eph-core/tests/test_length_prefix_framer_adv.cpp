/// @file test_length_prefix_framer_adv.cpp
/// Adversarial / boundary tests for eph::net::LengthPrefixFramer.
///
/// The pre-existing test_length_prefix_framer.cpp covers happy path
/// (round-trip via parameterized test, basic invalid args).  This file
/// fills the gaps:
///
/// * Maximum payload (65535) round-trip
/// * Multi-frame buffer (one decode call returns first frame; caller
///   advances by total_len for the next)
/// * Big-endian length encoding correctness across both header bytes
/// * msg_type extraction from the first payload byte
/// * Boundary lengths: 1, 256 (boundary between low byte and 2-byte),
///   65535
/// * Decode from buffers with trailing bytes (extra unrelated data)

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/core/length_prefix_framer.hpp"

using namespace eph::net;

namespace {

/// Encode a payload and return the resulting wire bytes as a vector.
std::vector<uint8_t> encode_to_vec(LengthPrefixFramer& f,
                                    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out(payload.size() + LengthPrefixFramer::max_overhead());
    size_t n = f.encode(out.data(), payload.data(), payload.size(),
                         /*msg_type=*/0);
    out.resize(n);
    return out;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Maximum payload boundary
// ═══════════════════════════════════════════════════════════════════════

TEST(LengthPrefixFramerAdv, MaxPayloadEncodesAndDecodes) {
    LengthPrefixFramer f;
    std::vector<uint8_t> payload(LengthPrefixFramer::kMaxPayloadLen, 0);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto wire = encode_to_vec(f, payload);
    ASSERT_EQ(wire.size(), payload.size() + 2);

    // Header: 0xFF 0xFF (65535 in big-endian)
    EXPECT_EQ(wire[0], 0xFF);
    EXPECT_EQ(wire[1], 0xFF);

    auto decoded = f.decode(wire.data(), wire.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->payload_len, payload.size());
    EXPECT_EQ(decoded->total_len, wire.size());
    EXPECT_EQ(0, std::memcmp(decoded->payload, payload.data(), payload.size()));
}

TEST(LengthPrefixFramerAdv, MaxPayloadPlusOneEncodeRejected) {
    LengthPrefixFramer f;
    std::vector<uint8_t> payload(LengthPrefixFramer::kMaxPayloadLen + 1, 0);
    std::vector<uint8_t> out(payload.size() + 2);
    size_t n = f.encode(out.data(), payload.data(), payload.size(), 0);
    EXPECT_EQ(n, 0u) << "payload above kMaxPayloadLen must be rejected";
}

// ═══════════════════════════════════════════════════════════════════════
// Big-endian header byte correctness
// ═══════════════════════════════════════════════════════════════════════

TEST(LengthPrefixFramerAdv, EncodeLength1HeaderBytes) {
    LengthPrefixFramer f;
    std::vector<uint8_t> payload{0xAB};
    auto wire = encode_to_vec(f, payload);
    // 0x00 0x01 (length=1, big-endian)
    EXPECT_EQ(wire[0], 0x00);
    EXPECT_EQ(wire[1], 0x01);
    EXPECT_EQ(wire[2], 0xAB);
}

TEST(LengthPrefixFramerAdv, EncodeLength255HeaderBytes) {
    LengthPrefixFramer f;
    std::vector<uint8_t> payload(255, 0xCC);
    auto wire = encode_to_vec(f, payload);
    // 0x00 0xFF (length=255, big-endian)
    EXPECT_EQ(wire[0], 0x00);
    EXPECT_EQ(wire[1], 0xFF);
}

TEST(LengthPrefixFramerAdv, EncodeLength256HeaderBytes) {
    LengthPrefixFramer f;
    std::vector<uint8_t> payload(256, 0xDD);
    auto wire = encode_to_vec(f, payload);
    // 0x01 0x00 (length=256, big-endian — boundary between byte 0 and byte 1)
    EXPECT_EQ(wire[0], 0x01);
    EXPECT_EQ(wire[1], 0x00);
}

TEST(LengthPrefixFramerAdv, EncodeLength32768HeaderBytes) {
    LengthPrefixFramer f;
    std::vector<uint8_t> payload(0x8000, 0xEE);
    auto wire = encode_to_vec(f, payload);
    // 0x80 0x00 — high bit of MSB set (catches signed/unsigned bugs)
    EXPECT_EQ(wire[0], 0x80);
    EXPECT_EQ(wire[1], 0x00);
}

// ═══════════════════════════════════════════════════════════════════════
// msg_type extraction from first payload byte
// ═══════════════════════════════════════════════════════════════════════

TEST(LengthPrefixFramerAdv, MsgTypeIsFirstPayloadByte) {
    LengthPrefixFramer f;
    std::vector<uint8_t> payload{'M', 1, 2, 3};
    auto wire = encode_to_vec(f, payload);
    auto decoded = f.decode(wire.data(), wire.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->msg_type, 'M');
}

TEST(LengthPrefixFramerAdv, MsgTypeZeroAcceptedWhenNonemptyPayload) {
    LengthPrefixFramer f;
    std::vector<uint8_t> payload{0x00, 0x01, 0x02};
    auto wire = encode_to_vec(f, payload);
    auto decoded = f.decode(wire.data(), wire.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->msg_type, 0x00);
    EXPECT_EQ(decoded->payload_len, 3u);
}

// ═══════════════════════════════════════════════════════════════════════
// Multi-frame buffer
// ═══════════════════════════════════════════════════════════════════════

TEST(LengthPrefixFramerAdv, MultiFrameBufferDecodesFirstFrame) {
    LengthPrefixFramer f;
    std::vector<uint8_t> p1{'A', 'B', 'C'};
    std::vector<uint8_t> p2{'X', 'Y'};

    auto w1 = encode_to_vec(f, p1);
    auto w2 = encode_to_vec(f, p2);

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), w1.begin(), w1.end());
    combined.insert(combined.end(), w2.begin(), w2.end());

    auto first = f.decode(combined.data(), combined.size());
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->payload_len, p1.size());
    EXPECT_EQ(first->total_len, w1.size());
    EXPECT_EQ(0, std::memcmp(first->payload, p1.data(), p1.size()));
}

TEST(LengthPrefixFramerAdv, MultiFrameBufferAdvanceDecodesSecondFrame) {
    LengthPrefixFramer f;
    std::vector<uint8_t> p1{'A', 'B', 'C'};
    std::vector<uint8_t> p2{'X', 'Y'};
    auto w1 = encode_to_vec(f, p1);
    auto w2 = encode_to_vec(f, p2);

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), w1.begin(), w1.end());
    combined.insert(combined.end(), w2.begin(), w2.end());

    // Decode the FIRST frame to learn its total_len, then advance and
    // decode the second.
    auto first = f.decode(combined.data(), combined.size());
    ASSERT_TRUE(first.has_value());
    size_t consumed = first->total_len;
    ASSERT_LT(consumed, combined.size());

    auto second = f.decode(combined.data() + consumed,
                            combined.size() - consumed);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->payload_len, p2.size());
    EXPECT_EQ(0, std::memcmp(second->payload, p2.data(), p2.size()));
}

TEST(LengthPrefixFramerAdv, FrameWithTrailingGarbageDecodesCleanly) {
    LengthPrefixFramer f;
    std::vector<uint8_t> payload{'A', 'B', 'C'};
    auto wire = encode_to_vec(f, payload);
    // Append unrelated bytes after the frame.
    wire.insert(wire.end(), {0xFF, 0xEE, 0xDD});

    auto decoded = f.decode(wire.data(), wire.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->payload_len, 3u);
    EXPECT_EQ(decoded->total_len, 5u); // 2 header + 3 payload — does NOT include garbage
}

// ═══════════════════════════════════════════════════════════════════════
// Encode boundary: exact buffer fit
// ═══════════════════════════════════════════════════════════════════════

TEST(LengthPrefixFramerAdv, EncodeIntoExactCapacityBuffer) {
    LengthPrefixFramer f;
    std::vector<uint8_t> payload{1, 2, 3, 4, 5};
    std::vector<uint8_t> out(payload.size() + 2); // exact capacity
    size_t n = f.encode(out.data(), payload.data(), payload.size(), 0);
    EXPECT_EQ(n, 7u);
    EXPECT_EQ(out[0], 0x00);
    EXPECT_EQ(out[1], 0x05);
}

// ═══════════════════════════════════════════════════════════════════════
// Decode partial-header / partial-body cases
// ═══════════════════════════════════════════════════════════════════════

TEST(LengthPrefixFramerAdv, DecodeWithHeaderClaimingMoreThanAvailable) {
    LengthPrefixFramer f;
    // Header says 100 bytes but only 5 follow.
    uint8_t buf[] = {0x00, 100, 'a', 'b', 'c', 'd', 'e'};
    auto r = f.decode(buf, sizeof(buf));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), FrameError::kIncomplete);
}

TEST(LengthPrefixFramerAdv, DecodeWithExactlyHeaderPlusOneBytePayload) {
    LengthPrefixFramer f;
    uint8_t buf[] = {0x00, 0x01, 0x42};
    auto r = f.decode(buf, sizeof(buf));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->payload_len, 1u);
    EXPECT_EQ(r->payload[0], 0x42);
    EXPECT_EQ(r->msg_type, 0x42);
}

TEST(LengthPrefixFramerAdv, DecodeWithExactlyHeaderNoPayloadIncomplete) {
    LengthPrefixFramer f;
    // Header says length=1 but no payload bytes follow.
    uint8_t buf[] = {0x00, 0x01};
    auto r = f.decode(buf, sizeof(buf));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), FrameError::kIncomplete);
}

// ═══════════════════════════════════════════════════════════════════════
// Round-trip across many lengths
// ═══════════════════════════════════════════════════════════════════════

TEST(LengthPrefixFramerAdv, RoundTripSweep) {
    LengthPrefixFramer f;
    static const size_t kLengths[] = {1, 2, 3, 16, 64, 255, 256, 1023, 1024,
                                       4096, 16384, 32768, 65534, 65535};
    for (size_t len : kLengths) {
        SCOPED_TRACE("len=" + std::to_string(len));
        std::vector<uint8_t> payload(len);
        for (size_t i = 0; i < len; ++i) {
            payload[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFF);
        }
        auto wire = encode_to_vec(f, payload);
        ASSERT_EQ(wire.size(), len + 2);

        auto decoded = f.decode(wire.data(), wire.size());
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded->payload_len, len);
        EXPECT_EQ(decoded->total_len, len + 2);
        EXPECT_EQ(0, std::memcmp(decoded->payload, payload.data(), len));
    }
}
