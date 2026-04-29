/// @file test_websocket_wire.cpp
/// Parser-level unit tests for the WebSocket wire encode/decode helpers
/// (`eph::net::ws::encode_frame`, `decode_frame`, `is_valid_close_code`, etc.).
///
/// These tests cover the header at `eph/net/detail/websocket.hpp` — the
/// same wire helpers used by the
/// stream-level `SocketTransport` / `Channel` WebSocket path. Stream-level
/// behavioural tests (Transport, Channel, SocketTransport) are intentionally
/// excluded — they live in their own files (9.8 territory).
///
/// Critical regression cases preserved from the original baseline:
///   * 1-byte Close-frame rejection per RFC 6455 §5.5.1 (commit 80d5a3b)
///   * 8-byte extended length encoding rules per RFC 6455 §5.2 (commit 3bed1b3)
///   * Forbidden close code handling / is_valid_close_code (commit 08dacb1)
///   * Slow-loris DoS via UINT64_MAX-payload frame (IntegerOverflowProtection)

#include <cstdint>
#include <cstring>
#include <format>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "eph/net/detail/websocket.hpp"

using namespace eph::net::ws;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time validation
// ─────────────────────────────────────────────────────────────────────────────

static_assert(frame_header_size(0) == 6);
static_assert(frame_header_size(1) == 6);
static_assert(frame_header_size(125) == 6);
static_assert(frame_header_size(126) == 8);
static_assert(frame_header_size(65535) == 8);
static_assert(frame_header_size(65536) == 14);

static_assert(total_frame_size(0) == 6);
static_assert(total_frame_size(125) == 131);
static_assert(total_frame_size(126) == 134);

// ─────────────────────────────────────────────────────────────────────────────
// apply_mask / masked_copy
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsMasking, ApplyMaskIsSymmetric) {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t original[sizeof(data)];
    std::memcpy(original, data, sizeof(data));

    uint8_t mask[4] = {0xAB, 0xCD, 0xEF, 0x01};

    apply_mask(data, sizeof(data), mask);
    EXPECT_NE(std::memcmp(data, original, sizeof(data)), 0);

    // Apply again — should restore original
    apply_mask(data, sizeof(data), mask);
    EXPECT_EQ(std::memcmp(data, original, sizeof(data)), 0);
}

TEST(WsMasking, ApplyMaskBoundaryLengths) {
    uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};

    for (size_t len : {0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 31, 32, 63, 64, 255, 256}) {
        std::vector<uint8_t> data(len, 0xAA);
        std::vector<uint8_t> original = data;

        apply_mask(data.data(), len, mask);
        if (len > 0) {
            bool changed = (std::memcmp(data.data(), original.data(), len) != 0);
            EXPECT_TRUE(changed) << "len=" << len;
        }

        apply_mask(data.data(), len, mask);
        EXPECT_EQ(data, original) << "len=" << len;
    }
}

TEST(WsMasking, MaskedCopyMatchesApplyMask) {
    uint8_t mask[4] = {0xDE, 0xAD, 0xBE, 0xEF};

    for (size_t len : {0, 1, 3, 4, 7, 8, 15, 16, 63, 64, 127, 128, 255, 256, 512}) {
        std::vector<uint8_t> src(len);
        for (size_t i = 0; i < len; ++i) src[i] = static_cast<uint8_t>(i & 0xFF);

        std::vector<uint8_t> dst1(len);
        std::memcpy(dst1.data(), src.data(), len);
        apply_mask(dst1.data(), len, mask);

        std::vector<uint8_t> dst2(len);
        masked_copy(dst2.data(), src.data(), len, mask);

        EXPECT_EQ(dst1, dst2) << "Mismatch at len=" << len;
    }
}

TEST(WsMasking, MaskKeyCacheProducesUniqueKeys) {
    std::set<uint32_t> seen;
    for (int i = 0; i < 100; ++i) {
        uint8_t key[4];
        generate_mask_key(key);
        uint32_t k;
        std::memcpy(&k, key, 4);
        seen.insert(k);
    }
    // With CSPRNG, 100 random 32-bit values should have negligible collision
    EXPECT_GT(seen.size(), 90u);
}

// ─────────────────────────────────────────────────────────────────────────────
// encode_frame / decode_frame roundtrip
// ─────────────────────────────────────────────────────────────────────────────

class WsRoundtrip : public ::testing::TestWithParam<size_t> {};

TEST_P(WsRoundtrip, EncodeDecodeRoundtrip) {
    size_t payload_len = GetParam();

    std::vector<uint8_t> payload(payload_len);
    for (size_t i = 0; i < payload_len; ++i) {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }

    std::vector<uint8_t> frame_buf(kMaxFrameHeaderLen + payload_len);
    size_t frame_len = encode_frame(
        frame_buf.data(), opcode::kBinary,
        payload.data(), payload_len);

    ASSERT_GT(frame_len, 0u);
    EXPECT_EQ(frame_len, total_frame_size(payload_len));

    auto result = decode_frame(frame_buf.data(), frame_len);
    ASSERT_TRUE(result.has_value()) << decode_error_name(result.error());

    auto& frame = *result;
    EXPECT_EQ(frame.opcode, opcode::kBinary);
    EXPECT_TRUE(frame.fin);
    EXPECT_TRUE(frame.masked); // Client frames are always masked
    EXPECT_EQ(frame.payload_len, payload_len);
    EXPECT_EQ(frame.total_len, frame_len);

    if (payload_len > 0) {
        std::vector<uint8_t> decoded(payload_len);
        std::memcpy(decoded.data(), frame.payload, payload_len);
        apply_mask(decoded.data(), payload_len, frame.mask_key);
        EXPECT_EQ(decoded, payload);
    }
}

INSTANTIATE_TEST_SUITE_P(
    PayloadSizes, WsRoundtrip,
    ::testing::Values(0, 1, 2, 3, 4, 7, 8, 125, 126, 127, 255, 256,
                      1024, 4096, 65535));

// ─────────────────────────────────────────────────────────────────────────────
// decode_frame edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsDecode, TooShortReturnsIncomplete) {
    uint8_t data[1] = {0x82};
    auto result = decode_frame(data, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

TEST(WsDecode, EmptyInputReturnsIncomplete) {
    auto result = decode_frame(nullptr, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

TEST(WsDecode, TruncatedPayloadReturnsIncomplete) {
    // Binary frame, payload_len=10, but only 6 bytes provided (header only)
    uint8_t data[] = {0x82, 0x0A, 0, 0, 0, 0};
    auto result = decode_frame(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

// REGRESSION (commit 3bed1b3): 8-byte extended length rejection per RFC 6455 §5.2.
// A peer advertising payload_len = UINT64_MAX would previously pin the decoder
// in kIncomplete forever — slow-loris DoS via the WS frame layer.
TEST(WsDecode, IntegerOverflowProtection) {
    uint8_t data[14] = {};
    data[0] = 0x82; // FIN + binary
    data[1] = 127;  // 8-byte extended length
    std::memset(data + 2, 0xFF, 8); // 0xFFFF...FF

    auto result = decode_frame(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kInvalidLengthEncoding);
}

TEST(WsDecode, UnmaskedServerFrame) {
    uint8_t data[] = {0x82, 0x03, 'a', 'b', 'c'};
    auto result = decode_frame(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->masked);
    EXPECT_EQ(result->payload_len, 3u);
    EXPECT_EQ(result->payload[0], 'a');
    EXPECT_EQ(result->payload[1], 'b');
    EXPECT_EQ(result->payload[2], 'c');
}

TEST(WsDecode, ExtendedLength126) {
    // Payload length = 200 (uses 2-byte extended length)
    std::vector<uint8_t> data(4 + 200);
    data[0] = 0x82;
    data[1] = 126;
    data[2] = 0x00;
    data[3] = 200;

    auto result = decode_frame(data.data(), data.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, 200u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Control frames
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsControlFrames, CloseFrame) {
    uint8_t buf[128];
    size_t len = build_close_frame(buf, close_code::kNormal, "bye");
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_close());
    EXPECT_TRUE(result->is_control());
    EXPECT_FALSE(result->is_data());
}

TEST(WsControlFrames, PingPong) {
    uint8_t payload[] = {1, 2, 3, 4};

    uint8_t ping_buf[64];
    size_t ping_len = build_ping_frame(ping_buf, payload, sizeof(payload));
    ASSERT_GT(ping_len, 0u);

    auto ping = decode_frame(ping_buf, ping_len);
    ASSERT_TRUE(ping.has_value());
    EXPECT_TRUE(ping->is_ping());

    uint8_t pong_buf[64];
    size_t pong_len = build_pong_frame(pong_buf, payload, sizeof(payload));
    ASSERT_GT(pong_len, 0u);

    auto pong = decode_frame(pong_buf, pong_len);
    ASSERT_TRUE(pong.has_value());
    EXPECT_TRUE(pong->is_pong());
}

TEST(WsControlFrames, PingNullPayloadWithLengthTreatedAsEmpty) {
    uint8_t buf[64];
    size_t len = build_ping_frame(buf, nullptr, 10);
    ASSERT_GT(len, 0u);
    auto frame = decode_frame(buf, len);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->is_ping());
    EXPECT_EQ(frame->payload_len, 0u);
}

TEST(WsControlFrames, PongNullPayloadWithLengthTreatedAsEmpty) {
    uint8_t buf[64];
    size_t len = build_pong_frame(buf, nullptr, 5);
    ASSERT_GT(len, 0u);
    auto frame = decode_frame(buf, len);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->is_pong());
    EXPECT_EQ(frame->payload_len, 0u);
}

TEST(WsControlFrames, PingNullPayloadZeroLengthIsValidEmpty) {
    uint8_t buf[64];
    size_t len = build_ping_frame(buf, nullptr, 0);
    ASSERT_GT(len, 0u);
    auto frame = decode_frame(buf, len);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->is_ping());
    EXPECT_EQ(frame->payload_len, 0u);
}

TEST(WsControlFrames, PingDefaultArgumentsProduceEmptyFrame) {
    uint8_t buf[64];
    size_t len = build_ping_frame(buf);
    ASSERT_GT(len, 0u);
    auto frame = decode_frame(buf, len);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->is_ping());
    EXPECT_EQ(frame->payload_len, 0u);
}

TEST(WsControlFrames, PingMaxControlPayload125Bytes) {
    uint8_t payload[125];
    for (int i = 0; i < 125; ++i) payload[i] = static_cast<uint8_t>(i);

    uint8_t buf[256];
    size_t len = build_ping_frame(buf, payload, 125);
    ASSERT_GT(len, 0u);
    auto frame = decode_frame(buf, len);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->is_ping());
    EXPECT_EQ(frame->payload_len, 125u);
    EXPECT_TRUE(frame->masked);
    std::vector<uint8_t> unmasked(frame->payload, frame->payload + 125);
    apply_mask(unmasked.data(), unmasked.size(), frame->mask_key);
    EXPECT_EQ(std::memcmp(unmasked.data(), payload, 125), 0);
}

TEST(WsControlFrames, PongPayloadRoundtrip) {
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t buf[64];
    size_t len = build_pong_frame(buf, payload, sizeof(payload));
    ASSERT_GT(len, 0u);
    auto frame = decode_frame(buf, len);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->is_pong());
    EXPECT_EQ(frame->payload_len, sizeof(payload));
    EXPECT_TRUE(frame->masked);
    std::vector<uint8_t> unmasked(frame->payload,
                                   frame->payload + sizeof(payload));
    apply_mask(unmasked.data(), unmasked.size(), frame->mask_key);
    EXPECT_EQ(std::memcmp(unmasked.data(), payload, sizeof(payload)), 0);
}

TEST(WsControlFrames, PongNullPayloadZeroLengthIsValidEmpty) {
    uint8_t buf[64];
    size_t len = build_pong_frame(buf, nullptr, 0);
    ASSERT_GT(len, 0u);
    auto frame = decode_frame(buf, len);
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->is_pong());
    EXPECT_EQ(frame->payload_len, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// FrameTemplate
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsFrameTemplate, BinaryTemplate) {
    auto tmpl = FrameTemplate::for_binary();
    EXPECT_EQ(tmpl.opcode_val, opcode::kBinary);
    EXPECT_TRUE(tmpl.fin);

    uint8_t payload[] = {0xAA, 0xBB, 0xCC};
    uint8_t buf[64];
    size_t len = tmpl.encode(buf, payload, sizeof(payload));
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kBinary);
    EXPECT_TRUE(result->fin);
}

TEST(WsFrameTemplate, TextTemplate) {
    auto tmpl = FrameTemplate::for_text();
    EXPECT_EQ(tmpl.opcode_val, opcode::kText);

    uint8_t payload[] = "hello";
    uint8_t buf[64];
    size_t len = tmpl.encode(buf, payload, 5);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kText);
}

// ─────────────────────────────────────────────────────────────────────────────
// Boundary tests — payload length transitions
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsBoundary, PayloadLen125_SmallHeader) {
    std::vector<uint8_t> payload(125, 0xAA);
    uint8_t buf[256];
    size_t len = encode_frame(buf, opcode::kBinary, payload.data(), 125);

    EXPECT_EQ(frame_header_size(125), 6u);
    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, 125u);
}

TEST(WsBoundary, PayloadLen126_ExtendedHeader) {
    std::vector<uint8_t> payload(126, 0xBB);
    uint8_t buf[256];
    size_t len = encode_frame(buf, opcode::kBinary, payload.data(), 126);

    EXPECT_EQ(frame_header_size(126), 8u);
    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, 126u);
}

TEST(WsBoundary, PayloadLen65535_MaxMediumHeader) {
    EXPECT_EQ(frame_header_size(65535), 8u);
    EXPECT_EQ(frame_header_size(65536), 14u);
}

TEST(WsBoundary, DecodeIncomplete_OnlyFirstByte) {
    uint8_t buf[1] = {0x82};
    auto result = decode_frame(buf, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

TEST(WsBoundary, DecodeIncomplete_ExtLen126_MissingBytes) {
    uint8_t buf[4] = {0x82, 0xFE, 0x00}; // 0xFE = mask|126
    auto result = decode_frame(buf, 3);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

TEST(WsBoundary, DecodeIncomplete_MaskKeyMissing) {
    uint8_t buf[2] = {0x82, 0x85}; // 0x85 = mask|5
    auto result = decode_frame(buf, 2);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

TEST(WsBoundary, DecodeIncomplete_PayloadTruncated) {
    uint8_t buf[64];
    [[maybe_unused]] size_t len = encode_frame(buf, opcode::kBinary,
                               (const uint8_t*)"hello", 5);
    auto result = decode_frame(buf, 6);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

// ─────────────────────────────────────────────────────────────────────────────
// Masking edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsMasking, MaskedCopy_ExactMultipleOf8) {
    uint8_t src[16], dst[16];
    std::memset(src, 0xFF, 16);
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

    masked_copy(dst, src, 16, mask);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(dst[i], static_cast<uint8_t>(0xFF ^ mask[i % 4])) << "i=" << i;
    }
}

TEST(WsMasking, MaskedCopy_OddTailBytes) {
    for (size_t len : {1, 2, 3, 5, 6, 7, 9, 13}) {
        std::vector<uint8_t> src(len, 0xAA), dst(len);
        uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};

        masked_copy(dst.data(), src.data(), len, mask);
        for (size_t i = 0; i < len; ++i) {
            EXPECT_EQ(dst[i], static_cast<uint8_t>(0xAA ^ mask[i % 4]))
                << "len=" << len << " i=" << i;
        }
    }
}

TEST(WsMasking, MaskedCopy_ZeroLength) {
    uint8_t src[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t dst[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};

    masked_copy(dst, src, 0, mask);
    EXPECT_EQ(dst[0], 0xFF);
    EXPECT_EQ(dst[1], 0xFF);
}

TEST(WsMasking, ApplyMask_ZeroLength) {
    uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};

    apply_mask(data, 0, mask);
    EXPECT_EQ(data[0], 0xAA);
    EXPECT_EQ(data[1], 0xBB);
}

TEST(WsMasking, MaskedCopy_ExactlyFourBytes) {
    uint8_t src[4] = {0x01, 0x02, 0x03, 0x04};
    uint8_t dst[4];
    uint8_t mask[4] = {0xFF, 0x00, 0xFF, 0x00};

    masked_copy(dst, src, 4, mask);
    EXPECT_EQ(dst[0], 0xFE);
    EXPECT_EQ(dst[1], 0x02);
    EXPECT_EQ(dst[2], 0xFC);
    EXPECT_EQ(dst[3], 0x04);
}

TEST(WsMasking, MaskKeyCacheRefill) {
    MaskKeyCache cache;
    std::set<uint32_t> keys;

    for (size_t i = 0; i < MaskKeyCache::kPoolSize + 10; ++i) {
        uint8_t key[4];
        cache.next_key(key);
        uint32_t k;
        std::memcpy(&k, key, 4);
        keys.insert(k);
    }

    EXPECT_GT(keys.size(), 500u)
        << "Mask keys should be mostly unique (CSPRNG)";
}

// ─────────────────────────────────────────────────────────────────────────────
// Close frame
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsCloseFrame, MaxReasonLength) {
    uint8_t buf[256];
    std::string reason(123, 'R');
    size_t len = build_close_frame(buf, close_code::kNormal, reason);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kClose);
    EXPECT_TRUE(result->is_control());

    ASSERT_TRUE(result->masked);
    uint8_t unmasked[125];
    std::memcpy(unmasked, result->payload, result->payload_len);
    apply_mask(unmasked, result->payload_len, result->mask_key);
    uint16_t code = (static_cast<uint16_t>(unmasked[0]) << 8) | unmasked[1];
    EXPECT_EQ(code, close_code::kNormal);
}

TEST(WsCloseFrame, LongReasonTruncated) {
    uint8_t buf[256];
    std::string long_reason(200, 'X');
    size_t len = build_close_frame(buf, close_code::kGoingAway, long_reason);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_LE(result->payload_len, 125u)
        << "Control frame payload must not exceed 125 bytes";
}

TEST(WsFrame, ContinuationOpcode) {
    uint8_t buf[64];
    uint8_t payload[] = {0x01, 0x02};
    size_t len = encode_frame(buf, opcode::kContinuation, payload, 2, false);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kContinuation);
    EXPECT_TRUE(result->is_data());
    EXPECT_FALSE(result->is_control());
    EXPECT_FALSE(result->fin);
}

// ─────────────────────────────────────────────────────────────────────────────
// Close frame edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsCloseFrame, EmptyReason) {
    uint8_t buf[64];
    size_t len = build_close_frame(buf, close_code::kNormal);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_close());
    EXPECT_EQ(result->payload_len, 2u);
}

TEST(WsCloseFrame, ExactMaxReason123Bytes) {
    uint8_t buf[256];
    std::string reason(123, 'X');
    size_t len = build_close_frame(buf, close_code::kGoingAway, reason);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, 125u);
}

// REGRESSION (commit 80d5a3b): reject 1-byte Close-frame body per RFC 6455 §5.5.1.
// Pre-fix: 1-byte close bodies silently decoded, close_status_code() returned
// 0, and the protocol violation was hidden in close-reason logging.
TEST(WsCloseFrame, RejectsOneByteCloseBodyPerRfc6455_5_5_1) {
    // Manually build an unmasked close frame with exactly 1 byte payload.
    uint8_t buf[4];
    buf[0] = 0x88;  // FIN=1, opcode=close(0x08)
    buf[1] = 0x01;  // unmasked, payload_len=1
    buf[2] = 0xE8;  // bogus single byte (high byte of 1000 would have been 0x03)

    auto result = decode_frame(buf, 3);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kInvalidCloseFrame)
        << "1-byte close body must be rejected per RFC 6455 §5.5.1";
}

TEST(WsCloseFrame, ZeroByteCloseBodyIsAccepted) {
    // RFC 6455 §5.5.1: close body "MAY" be absent. 0-byte is legal.
    uint8_t buf[2];
    buf[0] = 0x88;
    buf[1] = 0x00;  // unmasked, payload_len=0

    auto result = decode_frame(buf, 2);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_close());
    EXPECT_EQ(result->payload_len, 0u);
    EXPECT_EQ(result->close_status_code(), 0);
}

TEST(WsCloseFrame, TwoByteCloseBodyIsAcceptedAndStatusExtracted) {
    // Exactly 2 bytes = status code only, no reason. Must be accepted.
    uint8_t buf[4];
    buf[0] = 0x88;
    buf[1] = 0x02;
    buf[2] = 0x03; buf[3] = 0xE8;  // status = 1000 (Normal)

    auto result = decode_frame(buf, 4);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_close());
    EXPECT_EQ(result->payload_len, 2u);
    EXPECT_EQ(result->close_status_code(), 1000);
}

// ─────────────────────────────────────────────────────────────────────────────
// Large payload frame encoding
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsBoundary, PayloadLen65536_LargeHeader) {
    EXPECT_EQ(frame_header_size(65536), 14u);
    EXPECT_EQ(total_frame_size(65536), 65536u + 14u);
}

// ─────────────────────────────────────────────────────────────────────────────
// RFC 6455 compliance validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsFrame, RejectNonZeroRsvBits) {
    uint8_t buf[16] = {};
    buf[0] = kFinBit | 0x40 | opcode::kBinary; // FIN=1, RSV1=1
    buf[1] = 0x00;

    auto result = decode_frame(buf, 2);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kReservedBits);

    buf[0] = kFinBit | 0x20 | opcode::kText;
    result = decode_frame(buf, 2);
    ASSERT_FALSE(result.has_value());

    buf[0] = kFinBit | 0x10 | opcode::kBinary;
    result = decode_frame(buf, 2);
    ASSERT_FALSE(result.has_value());
}

TEST(WsFrame, RejectFragmentedControlFrame) {
    uint8_t buf[8] = {};
    buf[0] = opcode::kPing; // FIN=0, opcode=Ping
    buf[1] = 0x00;

    auto result = decode_frame(buf, 2);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kFragmentedControl);
}

TEST(WsFrame, RejectOversizedControlFrame) {
    uint8_t buf[256] = {};
    buf[0] = kFinBit | opcode::kClose;
    buf[1] = 126;
    buf[2] = 0x00;
    buf[3] = 126;

    auto result = decode_frame(buf, 256);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kControlPayloadTooLarge);
}

TEST(WsFrame, RejectReservedDataOpcodes) {
    for (uint8_t op = 0x3; op <= 0x7; ++op) {
        uint8_t buf[64] = {};
        buf[0] = kFinBit | op;
        buf[1] = 0;
        auto result = decode_frame(buf, 2);
        ASSERT_FALSE(result.has_value()) << "opcode 0x" << std::hex << (int)op;
        EXPECT_EQ(result.error(), DecodeError::kInvalidOpcode);
    }
}

TEST(WsFrame, RejectReservedControlOpcodes) {
    for (uint8_t op = 0xB; op <= 0xF; ++op) {
        uint8_t buf[64] = {};
        buf[0] = kFinBit | op;
        buf[1] = 0;
        auto result = decode_frame(buf, 2);
        ASSERT_FALSE(result.has_value()) << "opcode 0x" << std::hex << (int)op;
        EXPECT_EQ(result.error(), DecodeError::kInvalidOpcode);
    }
}

TEST(WsFrame, AcceptMaxSizeControlFrame) {
    uint8_t buf[256] = {};
    buf[0] = kFinBit | opcode::kPing;
    buf[1] = 125;

    auto result = decode_frame(buf, 127);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kPing);
    EXPECT_EQ(result->payload_len, 125u);
    EXPECT_TRUE(result->fin);
}

TEST(WsFrame, ZeroPayloadBinaryFrame) {
    uint8_t buf[64];
    size_t len = encode_frame(buf, opcode::kBinary, nullptr, 0);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kBinary);
    EXPECT_EQ(result->payload_len, 0u);
    EXPECT_TRUE(result->fin);
}

// ─────────────────────────────────────────────────────────────────────────────
// Encoder validation (payload_len bounds checking)
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsEncodeValidation, RejectPayloadLenWithMsbSet) {
    uint8_t buf[16];
    uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
    uint64_t bad_len = uint64_t{1} << 63;
    size_t result = encode_frame_header(buf, opcode::kBinary, bad_len, true, mask);
    EXPECT_EQ(result, 0u) << "Should reject payload_len with MSB set";
}

TEST(WsEncodeValidation, RejectControlFramePayloadOver125) {
    uint8_t buf[256];
    uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
    size_t result = encode_frame_header(buf, opcode::kPing, 126, true, mask);
    EXPECT_EQ(result, 0u);

    result = encode_frame_header(buf, opcode::kClose, 126, true, mask);
    EXPECT_EQ(result, 0u);

    result = encode_frame_header(buf, opcode::kPong, 126, true, mask);
    EXPECT_EQ(result, 0u);
}

TEST(WsEncodeValidation, AcceptControlFramePayloadExactly125) {
    uint8_t buf[16];
    uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
    size_t result = encode_frame_header(buf, opcode::kPing, 125, true, mask);
    EXPECT_GT(result, 0u);
}

TEST(WsEncodeValidation, AcceptMaxPayloadLen) {
    uint8_t buf[16];
    uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
    uint64_t max_len = kMaxPayloadLen;
    size_t result = encode_frame_header(buf, opcode::kBinary, max_len, true, mask);
    EXPECT_EQ(result, 14u);
}

TEST(WsEncodeValidation, EncodeFrameReturnsZeroOnInvalidPayload) {
    uint8_t buf[64];
    uint8_t payload[130] = {};
    size_t result = encode_frame(buf, opcode::kPing, payload, 130);
    EXPECT_EQ(result, 0u);
}

TEST(WsEncodeValidation, IsValidPayloadLen) {
    EXPECT_TRUE(is_valid_payload_len(opcode::kBinary, 0));
    EXPECT_TRUE(is_valid_payload_len(opcode::kBinary, 65536));
    EXPECT_TRUE(is_valid_payload_len(opcode::kBinary, kMaxPayloadLen));
    EXPECT_FALSE(is_valid_payload_len(opcode::kBinary, kMaxPayloadLen + 1));

    EXPECT_TRUE(is_valid_payload_len(opcode::kPing, 0));
    EXPECT_TRUE(is_valid_payload_len(opcode::kPing, 125));
    EXPECT_FALSE(is_valid_payload_len(opcode::kPing, 126));
    EXPECT_FALSE(is_valid_payload_len(opcode::kClose, 126));
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket fragmentation (RFC 6455 §5.4)
// ─────────────────────────────────────────────────────────────────────────────

/// Encode a frame with explicit FIN bit control (for fragmentation tests).
/// Returns total bytes written. Uses server-style encoding (unmasked) for easy
/// decode_frame testing without needing to unmask.
static size_t encode_unmasked_frame(uint8_t* out, uint8_t opcode_val,
                                      const uint8_t* payload, size_t payload_len,
                                      bool fin) {
    size_t pos = 0;
    out[pos++] = (fin ? kFinBit : 0) | (opcode_val & 0x0F);
    if (payload_len < 126) {
        out[pos++] = static_cast<uint8_t>(payload_len);
    } else if (payload_len <= 65535) {
        out[pos++] = 126;
        out[pos++] = static_cast<uint8_t>(payload_len >> 8);
        out[pos++] = static_cast<uint8_t>(payload_len & 0xFF);
    } else {
        out[pos++] = 127;
        for (int i = 7; i >= 0; --i)
            out[pos++] = static_cast<uint8_t>((payload_len >> (i * 8)) & 0xFF);
    }
    if (payload && payload_len > 0) {
        std::memcpy(out + pos, payload, payload_len);
    }
    return pos + payload_len;
}

TEST(WsFragmentation, FirstFragmentHasOpcodeAndNoFin) {
    uint8_t buf[64];
    const uint8_t payload[] = "Hel";
    size_t len = encode_unmasked_frame(buf, opcode::kText, payload, 3, false);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kText);
    EXPECT_FALSE(result->fin);
    EXPECT_TRUE(result->is_data());
    EXPECT_EQ(result->payload_len, 3u);
}

TEST(WsFragmentation, ContinuationFrameHasOpcodeZero) {
    uint8_t buf[64];
    const uint8_t payload[] = "lo ";
    size_t len = encode_unmasked_frame(buf, opcode::kContinuation, payload, 3, false);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kContinuation);
    EXPECT_FALSE(result->fin);
    EXPECT_TRUE(result->is_data());
}

TEST(WsFragmentation, FinalFragmentHasFin) {
    uint8_t buf[64];
    const uint8_t payload[] = "World";
    size_t len = encode_unmasked_frame(buf, opcode::kContinuation, payload, 5, true);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kContinuation);
    EXPECT_TRUE(result->fin);
    EXPECT_TRUE(result->is_data());
    EXPECT_EQ(result->payload_len, 5u);
}

TEST(WsFragmentation, ThreeFrameReassemblySequence) {
    uint8_t frame_buf[256];
    std::vector<uint8_t> reassembled;

    const uint8_t p1[] = {'H', 'e', 'l'};
    size_t len1 = encode_unmasked_frame(frame_buf, opcode::kText, p1, 3, false);
    auto f1 = decode_frame(frame_buf, len1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->opcode, opcode::kText);
    EXPECT_FALSE(f1->fin);
    reassembled.insert(reassembled.end(), f1->payload, f1->payload + f1->payload_len);

    const uint8_t p2[] = {'l', 'o', ' '};
    size_t len2 = encode_unmasked_frame(frame_buf, opcode::kContinuation, p2, 3, false);
    auto f2 = decode_frame(frame_buf, len2);
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->opcode, opcode::kContinuation);
    EXPECT_FALSE(f2->fin);
    reassembled.insert(reassembled.end(), f2->payload, f2->payload + f2->payload_len);

    const uint8_t p3[] = {'W', 'o', 'r', 'l', 'd'};
    size_t len3 = encode_unmasked_frame(frame_buf, opcode::kContinuation, p3, 5, true);
    auto f3 = decode_frame(frame_buf, len3);
    ASSERT_TRUE(f3.has_value());
    EXPECT_EQ(f3->opcode, opcode::kContinuation);
    EXPECT_TRUE(f3->fin);
    reassembled.insert(reassembled.end(), f3->payload, f3->payload + f3->payload_len);

    std::string msg(reassembled.begin(), reassembled.end());
    EXPECT_EQ(msg, "Hello World");
}

TEST(WsFragmentation, ControlFrameInterleaved) {
    uint8_t frame_buf[256];

    const uint8_t p1[] = {0xDE, 0xAD};
    size_t len1 = encode_unmasked_frame(frame_buf, opcode::kBinary, p1, 2, false);
    auto f1 = decode_frame(frame_buf, len1);
    ASSERT_TRUE(f1.has_value());
    EXPECT_FALSE(f1->fin);
    EXPECT_TRUE(f1->is_data());

    size_t ping_len = encode_unmasked_frame(frame_buf, opcode::kPing, nullptr, 0, true);
    auto ping = decode_frame(frame_buf, ping_len);
    ASSERT_TRUE(ping.has_value());
    EXPECT_TRUE(ping->is_ping());
    EXPECT_TRUE(ping->fin);
    EXPECT_TRUE(ping->is_control());

    const uint8_t p2[] = {0xBE, 0xEF};
    size_t len2 = encode_unmasked_frame(frame_buf, opcode::kContinuation, p2, 2, true);
    auto f2 = decode_frame(frame_buf, len2);
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->opcode, opcode::kContinuation);
    EXPECT_TRUE(f2->fin);
}

TEST(WsFragmentation, EmptyFirstFragment) {
    uint8_t buf[64];
    size_t len = encode_unmasked_frame(buf, opcode::kText, nullptr, 0, false);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kText);
    EXPECT_FALSE(result->fin);
    EXPECT_EQ(result->payload_len, 0u);
}

TEST(WsFragmentation, SingleFrameMessageHasFin) {
    uint8_t buf[64];
    const uint8_t payload[] = {0x42};
    size_t len = encode_unmasked_frame(buf, opcode::kBinary, payload, 1, true);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kBinary);
    EXPECT_TRUE(result->fin);
    EXPECT_EQ(result->payload_len, 1u);
}

TEST(WsFragmentation, MultipleFramesInSingleBuffer) {
    uint8_t buf[256];
    const uint8_t p1[] = {'A', 'B'};
    const uint8_t p2[] = {'C', 'D', 'E'};

    size_t len1 = encode_unmasked_frame(buf, opcode::kText, p1, 2, false);
    size_t len2 = encode_unmasked_frame(buf + len1, opcode::kContinuation, p2, 3, true);

    auto f1 = decode_frame(buf, len1 + len2);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->opcode, opcode::kText);
    EXPECT_EQ(f1->payload_len, 2u);
    EXPECT_EQ(f1->total_len, len1);

    auto f2 = decode_frame(buf + f1->total_len, len1 + len2 - f1->total_len);
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->opcode, opcode::kContinuation);
    EXPECT_EQ(f2->payload_len, 3u);
    EXPECT_TRUE(f2->fin);
}

TEST(WsFragmentation, FirstFragmentHasFinFalse) {
    const uint8_t payload[] = "part1";
    uint8_t buf[64];
    auto written = encode_frame(buf, opcode::kText, payload, 5, /*fin=*/false);
    ASSERT_GT(written, 0u);

    auto result = decode_frame(buf, written);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->fin);
    EXPECT_EQ(result->opcode, opcode::kText);
}

TEST(WsFragmentation, MultiPartFragmentRoundtrip) {
    const uint8_t p1[] = "hello";
    uint8_t buf1[64];
    auto w1 = encode_frame(buf1, opcode::kText, p1, 5, /*fin=*/false);
    ASSERT_GT(w1, 0u);

    const uint8_t p2[] = " world";
    uint8_t buf2[64];
    auto w2 = encode_frame(buf2, opcode::kContinuation, p2, 6, /*fin=*/true);
    ASSERT_GT(w2, 0u);

    auto r1 = decode_frame(buf1, w1);
    auto r2 = decode_frame(buf2, w2);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());

    EXPECT_FALSE(r1->fin);
    EXPECT_EQ(r1->opcode, opcode::kText);
    EXPECT_TRUE(r2->fin);
    EXPECT_EQ(r2->opcode, opcode::kContinuation);
}

// ─────────────────────────────────────────────────────────────────────────────
// UTF-8 validation (RFC 6455 §5.6)
// ─────────────────────────────────────────────────────────────────────────────

TEST(Utf8Validation, EmptyString) {
    EXPECT_TRUE(is_valid_utf8(nullptr, 0));
    EXPECT_TRUE(is_valid_utf8(std::string_view("")));
}

TEST(Utf8Validation, AsciiOnly) {
    EXPECT_TRUE(is_valid_utf8(std::string_view("Hello, World!")));
    EXPECT_TRUE(is_valid_utf8(std::string_view("\t\n\r ")));
}

TEST(Utf8Validation, ValidMultibyte) {
    // 2-byte: U+00E9 (é) = C3 A9
    EXPECT_TRUE(is_valid_utf8(std::string_view("caf\xC3\xA9")));
    // 3-byte: U+4E16 (世) = E4 B8 96
    EXPECT_TRUE(is_valid_utf8(std::string_view("\xE4\xB8\x96\xE7\x95\x8C")));
    // 4-byte: U+1F600 (😀) = F0 9F 98 80
    EXPECT_TRUE(is_valid_utf8(std::string_view("\xF0\x9F\x98\x80")));
}

TEST(Utf8Validation, InvalidContinuationByte) {
    uint8_t data[] = {0x80};
    EXPECT_FALSE(is_valid_utf8(data, 1));
    uint8_t data2[] = {0xC3}; // 2-byte start, no continuation
    EXPECT_FALSE(is_valid_utf8(data2, 1));
}

TEST(Utf8Validation, OverlongEncoding) {
    // Overlong 2-byte encoding of U+0000: C0 80
    uint8_t data[] = {0xC0, 0x80};
    EXPECT_FALSE(is_valid_utf8(data, 2));
    // Overlong 3-byte encoding of U+002F: E0 80 AF
    uint8_t data2[] = {0xE0, 0x80, 0xAF};
    EXPECT_FALSE(is_valid_utf8(data2, 3));
}

TEST(Utf8Validation, SurrogateHalves) {
    // U+D800 (high surrogate): ED A0 80 — invalid
    uint8_t data[] = {0xED, 0xA0, 0x80};
    EXPECT_FALSE(is_valid_utf8(data, 3));
    // U+DFFF (low surrogate): ED BF BF
    uint8_t data2[] = {0xED, 0xBF, 0xBF};
    EXPECT_FALSE(is_valid_utf8(data2, 3));
}

TEST(Utf8Validation, TruncatedSequence) {
    uint8_t data[] = {0xE4, 0xB8};
    EXPECT_FALSE(is_valid_utf8(data, 2));
    uint8_t data2[] = {0xF0, 0x9F, 0x98};
    EXPECT_FALSE(is_valid_utf8(data2, 3));
}

TEST(Utf8Validation, MaxCodepoints) {
    uint8_t data[] = {0xEF, 0xBF, 0xBF};
    EXPECT_TRUE(is_valid_utf8(data, 3));
    // U+10FFFF (max Unicode): F4 8F BF BF
    uint8_t data2[] = {0xF4, 0x8F, 0xBF, 0xBF};
    EXPECT_TRUE(is_valid_utf8(data2, 4));
    // U+110000: beyond max — invalid
    uint8_t data3[] = {0xF4, 0x90, 0x80, 0x80};
    EXPECT_FALSE(is_valid_utf8(data3, 4));
}

TEST(Utf8Validation, SpanOverload) {
    std::vector<uint8_t> valid = {0x48, 0x65, 0x6C, 0x6C, 0x6F};
    EXPECT_TRUE(is_valid_utf8(std::span<const uint8_t>(valid)));

    std::vector<uint8_t> invalid = {0xFF, 0xFE};
    EXPECT_FALSE(is_valid_utf8(std::span<const uint8_t>(invalid)));
}

TEST(Utf8Validation, MaxUtf8Codepoint) {
    uint8_t valid[] = {0xF4, 0x8F, 0xBF, 0xBF};
    EXPECT_TRUE(is_valid_utf8(valid, 4));
}

TEST(Utf8Validation, BeyondMaxCodepoint) {
    uint8_t invalid[] = {0xF4, 0x90, 0x80, 0x80};
    EXPECT_FALSE(is_valid_utf8(invalid, 4));
}

TEST(Utf8Validation, NullByteIsValid) {
    uint8_t data[] = {0x00};
    EXPECT_TRUE(is_valid_utf8(data, 1));
}

TEST(Utf8Validation, EmptyInputIsValid) {
    EXPECT_TRUE(is_valid_utf8(nullptr, 0));
}

TEST(Utf8Validation, ExactByteSequenceLengths) {
    uint8_t one[] = {0x41};
    EXPECT_TRUE(is_valid_utf8(one, 1));
    uint8_t two[] = {0xC3, 0x80};
    EXPECT_TRUE(is_valid_utf8(two, 2));
    uint8_t three[] = {0xE4, 0xB8, 0x96};
    EXPECT_TRUE(is_valid_utf8(three, 3));
    uint8_t four[] = {0xF0, 0x9F, 0x98, 0x80};
    EXPECT_TRUE(is_valid_utf8(four, 4));
}

// ─────────────────────────────────────────────────────────────────────────────
// DecodeError std::formatter
// ─────────────────────────────────────────────────────────────────────────────

TEST(DecodeErrorFormatter, AllValuesFormat) {
    EXPECT_EQ(std::format("{}", DecodeError::kIncomplete), "incomplete");
    EXPECT_EQ(std::format("{}", DecodeError::kReservedBits),
              "non-zero RSV bits without negotiated extension");
    EXPECT_EQ(std::format("{}", DecodeError::kFragmentedControl),
              "fragmented control frame");
    EXPECT_EQ(std::format("{}", DecodeError::kControlPayloadTooLarge),
              "control frame payload exceeds 125 bytes");
    EXPECT_EQ(std::format("{}", DecodeError::kInvalidOpcode),
              "reserved opcode (RFC 6455 §5.2)");
}

TEST(DecodeErrorName, AllErrorNamesAreDistinct) {
    auto name_incomplete = decode_error_name(DecodeError::kIncomplete);
    auto name_rsv = decode_error_name(DecodeError::kReservedBits);
    auto name_frag = decode_error_name(DecodeError::kFragmentedControl);
    auto name_large = decode_error_name(DecodeError::kControlPayloadTooLarge);
    auto name_opcode = decode_error_name(DecodeError::kInvalidOpcode);

    EXPECT_FALSE(name_incomplete.empty());
    EXPECT_FALSE(name_rsv.empty());
    EXPECT_FALSE(name_frag.empty());
    EXPECT_FALSE(name_large.empty());
    EXPECT_FALSE(name_opcode.empty());

    EXPECT_NE(name_incomplete, name_rsv);
    EXPECT_NE(name_incomplete, name_frag);
    EXPECT_NE(name_incomplete, name_large);
    EXPECT_NE(name_incomplete, name_opcode);
}

// ─────────────────────────────────────────────────────────────────────────────
// opcode_name() and Opcode formatter
// ─────────────────────────────────────────────────────────────────────────────

TEST(OpcodeName, StandardOpcodes) {
    EXPECT_EQ(opcode_name(opcode::kContinuation), "CONTINUATION");
    EXPECT_EQ(opcode_name(opcode::kText), "TEXT");
    EXPECT_EQ(opcode_name(opcode::kBinary), "BINARY");
    EXPECT_EQ(opcode_name(opcode::kClose), "CLOSE");
    EXPECT_EQ(opcode_name(opcode::kPing), "PING");
    EXPECT_EQ(opcode_name(opcode::kPong), "PONG");
}

TEST(OpcodeName, UnknownOpcode) {
    auto name = opcode_name(0x03);
    EXPECT_NE(name.find("UNKNOWN"), std::string::npos);
    EXPECT_NE(name.find("0x03"), std::string::npos);
}

TEST(OpcodeFormatter, FormatsViaWrapper) {
    EXPECT_EQ(std::format("{}", Opcode{opcode::kText}), "TEXT");
    EXPECT_EQ(std::format("{}", Opcode{opcode::kPing}), "PING");
    EXPECT_EQ(std::format("{}", Opcode{opcode::kBinary}), "BINARY");
}

TEST(OpcodeFormatter, UnknownFormatted) {
    auto s = std::format("{}", Opcode{0x05});
    EXPECT_NE(s.find("UNKNOWN"), std::string::npos);
}

TEST(OpcodeName, AllReservedOpcodes) {
    for (uint8_t op : {0x03, 0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}) {
        auto name = opcode_name(op);
        EXPECT_TRUE(name.find("UNKNOWN") != std::string_view::npos)
            << "opcode 0x" << std::hex << static_cast<int>(op) << " should be UNKNOWN";
    }
}

TEST(OpcodeName, MaxOpcode) {
    auto name = opcode_name(0xFF);
    EXPECT_TRUE(name.find("UNKNOWN") != std::string_view::npos);
    EXPECT_TRUE(name.find("FF") != std::string_view::npos
             || name.find("ff") != std::string_view::npos);
}

TEST(OpcodeName, ConsecutiveCallsReturnCorrectValues) {
    auto n1 = opcode_name(opcode::kPing);
    EXPECT_EQ(n1, "PING");
    auto n2 = opcode_name(0x03);
    EXPECT_TRUE(n2.find("UNKNOWN") != std::string_view::npos);
    auto n3 = opcode_name(opcode::kBinary);
    EXPECT_EQ(n3, "BINARY");
}

// ─────────────────────────────────────────────────────────────────────────────
// close_code_name
// ─────────────────────────────────────────────────────────────────────────────

TEST(CloseCodeName, StandardCodes) {
    EXPECT_EQ(close_code_name(close_code::kNormal), "NORMAL_CLOSURE");
    EXPECT_EQ(close_code_name(close_code::kGoingAway), "GOING_AWAY");
    EXPECT_EQ(close_code_name(close_code::kProtocolError), "PROTOCOL_ERROR");
    EXPECT_EQ(close_code_name(close_code::kUnsupportedData), "UNSUPPORTED_DATA");
    EXPECT_EQ(close_code_name(close_code::kAbnormalClosure), "ABNORMAL_CLOSURE");
    EXPECT_EQ(close_code_name(close_code::kInvalidPayload), "INVALID_PAYLOAD");
    EXPECT_EQ(close_code_name(close_code::kPolicyViolation), "POLICY_VIOLATION");
    EXPECT_EQ(close_code_name(close_code::kMessageTooBig), "MESSAGE_TOO_BIG");
    EXPECT_EQ(close_code_name(close_code::kMandatoryExtension), "MANDATORY_EXTENSION");
    EXPECT_EQ(close_code_name(close_code::kInternalError), "INTERNAL_ERROR");
}

TEST(CloseCodeName, RegisteredRange) {
    auto name = close_code_name(3000);
    EXPECT_NE(name.find("REGISTERED"), std::string::npos);
    EXPECT_NE(name.find("3000"), std::string::npos);
}

TEST(CloseCodeName, PrivateRange) {
    auto name = close_code_name(4000);
    EXPECT_NE(name.find("PRIVATE"), std::string::npos);
    EXPECT_NE(name.find("4000"), std::string::npos);
}

TEST(CloseCodeName, UnknownCode) {
    auto name = close_code_name(1234);
    EXPECT_NE(name.find("UNKNOWN"), std::string::npos);
    EXPECT_NE(name.find("1234"), std::string::npos);
}

TEST(CloseCodeName, RangeBoundaries) {
    auto reg_end = close_code_name(3999);
    EXPECT_TRUE(reg_end.find("REGISTERED") != std::string_view::npos);
    EXPECT_TRUE(reg_end.find("3999") != std::string_view::npos);

    auto priv_end = close_code_name(4999);
    EXPECT_TRUE(priv_end.find("PRIVATE") != std::string_view::npos);
    EXPECT_TRUE(priv_end.find("4999") != std::string_view::npos);

    auto below_reg = close_code_name(2999);
    EXPECT_TRUE(below_reg.find("UNKNOWN") != std::string_view::npos);

    auto above_priv = close_code_name(5000);
    EXPECT_TRUE(above_priv.find("UNKNOWN") != std::string_view::npos);
}

TEST(CloseCodeName, ReservedCodesThatAreNotStandard) {
    auto c1004 = close_code_name(1004);
    EXPECT_TRUE(c1004.find("UNKNOWN") != std::string_view::npos);

    auto c1005 = close_code_name(1005);
    EXPECT_TRUE(c1005.find("UNKNOWN") != std::string_view::npos);

    auto c1012 = close_code_name(1012);
    EXPECT_TRUE(c1012.find("UNKNOWN") != std::string_view::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// CloseCode formatter
// ─────────────────────────────────────────────────────────────────────────────

TEST(CloseCodeFormatter, FormatsViaWrapper) {
    EXPECT_EQ(std::format("{}", CloseCode{close_code::kNormal}), "NORMAL_CLOSURE");
    EXPECT_EQ(std::format("{}", CloseCode{close_code::kGoingAway}), "GOING_AWAY");
}

TEST(CloseCodeFormatter, UnknownFormatted) {
    auto s = std::format("{}", CloseCode{9999});
    EXPECT_NE(s.find("UNKNOWN"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// DecodedFrame::close_reason
// ─────────────────────────────────────────────────────────────────────────────

TEST(DecodedFrameCloseReason, ExtractsReason) {
    uint8_t buf[256];
    size_t len = build_close_frame(buf, close_code::kNormal, "bye");
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->is_close());

    if (result->masked && result->payload_len > 0) {
        std::vector<uint8_t> payload(result->payload,
                                     result->payload + result->payload_len);
        apply_mask(payload.data(), payload.size(), result->mask_key);
        uint16_t code = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
        EXPECT_EQ(code, close_code::kNormal);
        std::string_view reason(reinterpret_cast<const char*>(payload.data() + 2),
                                payload.size() - 2);
        EXPECT_EQ(reason, "bye");
    }
}

TEST(DecodedFrameCloseReason, EmptyReasonReturnsEmpty) {
    uint8_t buf[256];
    size_t len = build_close_frame(buf, close_code::kNormal);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->close_reason().size(), 0u);
}

TEST(DecodedFrameCloseReason, NonCloseFrameReturnsEmpty) {
    DecodedFrame frame;
    frame.opcode = opcode::kBinary;
    frame.payload_len = 10;
    EXPECT_TRUE(frame.close_reason().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// is_valid_close_code — RFC 6455 §7.4 boundary testing
// REGRESSION (commit 08dacb1): is_valid_close_code gates forbidden codes
// (1004-1006, 1012-1015) so echoed close responses never carry them.
// ─────────────────────────────────────────────────────────────────────────────

TEST(CloseCodeValidation, StandardCodesAreValid) {
    EXPECT_TRUE(is_valid_close_code(1000));
    EXPECT_TRUE(is_valid_close_code(1001));
    EXPECT_TRUE(is_valid_close_code(1002));
    EXPECT_TRUE(is_valid_close_code(1003));
}

TEST(CloseCodeValidation, ReservedCodesAreInvalid) {
    // 1004-1006 are forbidden per RFC 6455 §7.4
    EXPECT_FALSE(is_valid_close_code(1004));
    EXPECT_FALSE(is_valid_close_code(1005));
    EXPECT_FALSE(is_valid_close_code(1006));
}

TEST(CloseCodeValidation, StandardErrorCodesAreValid) {
    // 1007 (invalid payload) and 1011 (internal error) are the codes
    // specifically echoed by the stream-level close handler after fix 08dacb1.
    EXPECT_TRUE(is_valid_close_code(1007));
    EXPECT_TRUE(is_valid_close_code(1008));
    EXPECT_TRUE(is_valid_close_code(1009));
    EXPECT_TRUE(is_valid_close_code(1010));
    EXPECT_TRUE(is_valid_close_code(1011));
}

TEST(CloseCodeValidation, ReservedCodesBetweenRangesAreInvalid) {
    // The strict eph::net::ws::is_valid_close_code returns false for all of
    // [1012..1015]. The motivation per code:
    //   * 1015 — RFC 6455 §7.4.1: explicitly reserved, MUST NOT be sent on
    //     the wire (it's a synthetic "TLS handshake failure" indicator the
    //     LIBRARY uses internally; sending it as a Close payload is a
    //     protocol violation).
    //   * 1012 / 1013 / 1014 — undefined in RFC 6455 itself; subsequently
    //     IANA-registered (Service Restart / Try Again Later / Bad Gateway)
    //     in the WebSocket Close Code Number Registry. eph deliberately
    //     stays on the conservative RFC-6455-strict reading and rejects
    //     them: HFT venues do not send these codes, and accepting them
    //     would dilute the rare-code branch's negative-test value. If a
    //     future deployment needs interop with a server that does send
    //     1012/1013/1014, broaden `is_valid_close_code`'s second branch
    //     (line 138 of detail/websocket.hpp) — this test pins the current
    //     conservative shape.
    EXPECT_FALSE(is_valid_close_code(1012));
    EXPECT_FALSE(is_valid_close_code(1013));
    EXPECT_FALSE(is_valid_close_code(1014));
    EXPECT_FALSE(is_valid_close_code(1015));
}

TEST(CloseCodeValidation, RegisteredRangeIsValid) {
    EXPECT_TRUE(is_valid_close_code(3000));
    EXPECT_TRUE(is_valid_close_code(4000));
    EXPECT_TRUE(is_valid_close_code(4999));
}

TEST(CloseCodeValidation, BoundaryValuesOutsideRanges) {
    EXPECT_FALSE(is_valid_close_code(999));
    EXPECT_FALSE(is_valid_close_code(2999));
    EXPECT_FALSE(is_valid_close_code(5000));
    EXPECT_FALSE(is_valid_close_code(0));
    EXPECT_FALSE(is_valid_close_code(65535));
}

TEST(CloseCodeValidation, UnusedRangesAreInvalid) {
    EXPECT_FALSE(is_valid_close_code(1016));
    EXPECT_FALSE(is_valid_close_code(2000));
    EXPECT_FALSE(is_valid_close_code(2999));
}

// ─────────────────────────────────────────────────────────────────────────────
// DecodedFrame::close_status_code()
// ─────────────────────────────────────────────────────────────────────────────

TEST(CloseStatusCode, ExtractsStatusCodeFromCloseFrame) {
    uint8_t buf[128];
    auto len = build_close_frame(buf, 1000);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_close());

    EXPECT_EQ(result->close_status_code(), 1000);
}

TEST(CloseStatusCode, ExtractsVariousStatusCodes) {
    for (uint16_t code : {1000, 1001, 1002, 1008, 1011, 3000, 4999}) {
        uint8_t buf[128];
        auto len = build_close_frame(buf, code);
        ASSERT_GT(len, 0u) << "Failed to build close frame for code " << code;

        auto result = decode_frame(buf, len);
        ASSERT_TRUE(result.has_value());

        EXPECT_EQ(result->close_status_code(), code)
            << "Status code mismatch for " << code;
    }
}

TEST(CloseStatusCode, ReturnsZeroForNonCloseFrame) {
    const uint8_t payload[] = "hello";
    uint8_t buf[128];
    auto len = encode_frame(buf, opcode::kText, payload, 5);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->is_close());
    EXPECT_EQ(result->close_status_code(), 0);
}

TEST(CloseStatusCode, ReturnsZeroForEmptyClosePayload) {
    uint8_t buf[2] = {};
    buf[0] = 0x88;
    buf[1] = 0x00;

    auto result = decode_frame(buf, 2);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_close());
    EXPECT_EQ(result->payload_len, 0u);
    EXPECT_EQ(result->close_status_code(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8-byte extended length (payload > 65535)
// REGRESSION (commit 3bed1b3): enforce RFC 6455 §5.2 extended length encoding.
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsLargeFrame, EncodeDecodePayloadOver65535) {
    constexpr size_t payload_size = 65536;
    std::vector<uint8_t> payload(payload_size, 0xAB);
    std::vector<uint8_t> buf(total_frame_size(payload_size) + 16);

    auto written = encode_frame(buf.data(), opcode::kBinary,
                                 payload.data(), payload_size);
    ASSERT_GT(written, 0u);

    // Header should be 14 bytes (2 base + 8 extended length + 4 mask key)
    EXPECT_EQ(frame_header_size(payload_size), 14u);

    auto result = decode_frame(buf.data(), written);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kBinary);
    EXPECT_EQ(result->payload_len, payload_size);
    EXPECT_TRUE(result->fin);
}

TEST(WsLargeFrame, EncodeDecodePayloadExactly65535) {
    constexpr size_t payload_size = 65535;
    std::vector<uint8_t> payload(payload_size, 0xCD);
    std::vector<uint8_t> buf(total_frame_size(payload_size) + 16);

    auto written = encode_frame(buf.data(), opcode::kBinary,
                                 payload.data(), payload_size);
    ASSERT_GT(written, 0u);

    EXPECT_EQ(frame_header_size(payload_size), 8u);

    auto result = decode_frame(buf.data(), written);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, payload_size);
}

// Non-minimal 2-byte extended length (value < 126) is illegal — the sender
// should have used the 7-bit inline form. RFC 6455 §5.2.
TEST(WsDecode, RejectsNonMinimalTwoByteExtendedLengthRfc6455_5_2) {
    uint8_t buf[4];
    buf[0] = 0x82;  // FIN + binary
    buf[1] = 126;   // 2-byte extended length follows
    buf[2] = 0x00; buf[3] = 0x7D;  // value = 125 (< 126, should have inlined)

    auto result = decode_frame(buf, 4);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kInvalidLengthEncoding);
}

// Non-minimal 8-byte extended length (value <= 65535) is illegal — the sender
// should have used the 2-byte form. RFC 6455 §5.2.
TEST(WsDecode, RejectsNonMinimal8ByteExtendedLengthRfc6455_5_2) {
    uint8_t buf[10] = {};
    buf[0] = 0x82;
    buf[1] = 127;   // 8-byte extended length follows
    // 8-byte value = 65535 (fits in 2-byte form, so non-minimal)
    buf[8] = 0xFF; buf[9] = 0xFF;

    auto result = decode_frame(buf, 10);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kInvalidLengthEncoding);
}

// ─────────────────────────────────────────────────────────────────────────────
// decode_frame edge cases — extended length boundaries
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsDecode, IncompleteExtendedLengthReturnsIncomplete) {
    uint8_t buf[3] = {};
    buf[0] = 0x82;
    buf[1] = 126;

    auto result = decode_frame(buf, 3);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

TEST(WsDecode, IncompletePayloadReturnsIncomplete) {
    uint8_t buf[7] = {};
    buf[0] = 0x81;  // FIN+text, unmasked
    buf[1] = 10;

    auto result = decode_frame(buf, 7);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

TEST(WsDecode, ExtendedLength8ByteEncoding) {
    constexpr size_t payload_len = 65536;
    std::vector<uint8_t> frame(10 + payload_len, 0x42);
    frame[0] = 0x82;
    frame[1] = 127;
    frame[2] = 0; frame[3] = 0; frame[4] = 0; frame[5] = 0;
    frame[6] = 0; frame[7] = 1; frame[8] = 0; frame[9] = 0;  // 65536

    auto result = decode_frame(frame.data(), frame.size());
    ASSERT_TRUE(result.has_value()) << decode_error_name(result.error());
    EXPECT_EQ(result->payload_len, payload_len);
    EXPECT_EQ(result->opcode, opcode::kBinary);
}

TEST(WsDecode, ExtendedLength2ByteBoundary65535) {
    constexpr size_t payload_len = 65535;
    std::vector<uint8_t> frame(4 + payload_len, 0xAA);
    frame[0] = 0x82;
    frame[1] = 126;
    frame[2] = 0xFF; frame[3] = 0xFF;

    auto result = decode_frame(frame.data(), frame.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, payload_len);
}

TEST(WsDecode, ExtendedLength2ByteMinimum126) {
    constexpr size_t payload_len = 126;
    std::vector<uint8_t> frame(4 + payload_len, 0xBB);
    frame[0] = 0x82;
    frame[1] = 126;
    frame[2] = 0x00; frame[3] = 0x7E;

    auto result = decode_frame(frame.data(), frame.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, payload_len);
}

TEST(WsDecode, Incomplete8ByteExtendedLengthReturnsIncomplete) {
    uint8_t buf[6] = {};
    buf[0] = 0x82;
    buf[1] = 127;

    auto result = decode_frame(buf, 6);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

// ─────────────────────────────────────────────────────────────────────────────
// DecodedFrame convenience methods
// ─────────────────────────────────────────────────────────────────────────────

TEST(DecodedFrame, IsControlClassifiesCorrectly) {
    DecodedFrame frame{};
    frame.opcode = opcode::kPing;
    EXPECT_TRUE(frame.is_control());
    EXPECT_FALSE(frame.is_data());

    frame.opcode = opcode::kPong;
    EXPECT_TRUE(frame.is_control());

    frame.opcode = opcode::kClose;
    EXPECT_TRUE(frame.is_control());
    EXPECT_TRUE(frame.is_close());
    EXPECT_FALSE(frame.is_ping());
    EXPECT_FALSE(frame.is_pong());
}

TEST(DecodedFrame, IsDataClassifiesCorrectly) {
    DecodedFrame frame{};
    frame.opcode = opcode::kText;
    EXPECT_TRUE(frame.is_data());
    EXPECT_FALSE(frame.is_control());

    frame.opcode = opcode::kBinary;
    EXPECT_TRUE(frame.is_data());

    frame.opcode = opcode::kContinuation;
    EXPECT_TRUE(frame.is_data());
}

TEST(DecodedFrame, IsPingPongCloseIsolated) {
    DecodedFrame frame{};
    frame.opcode = opcode::kPing;
    EXPECT_TRUE(frame.is_ping());
    EXPECT_FALSE(frame.is_pong());
    EXPECT_FALSE(frame.is_close());

    frame.opcode = opcode::kPong;
    EXPECT_FALSE(frame.is_ping());
    EXPECT_TRUE(frame.is_pong());
    EXPECT_FALSE(frame.is_close());

    frame.opcode = opcode::kClose;
    EXPECT_FALSE(frame.is_ping());
    EXPECT_FALSE(frame.is_pong());
    EXPECT_TRUE(frame.is_close());
}

// ─────────────────────────────────────────────────────────────────────────────
// apply_mask — additional boundary conditions
// ─────────────────────────────────────────────────────────────────────────────

TEST(WsMasking, ApplyMaskZeroKeyIsNoop) {
    uint8_t data[] = {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48};
    uint8_t original[8];
    std::memcpy(original, data, 8);
    uint8_t mask[4] = {0, 0, 0, 0};

    apply_mask(data, 8, mask);
    EXPECT_EQ(std::memcmp(data, original, 8), 0)
        << "Zero mask key should leave data unchanged";
}

TEST(WsMasking, MaskedCopySmallSizes) {
    uint8_t mask[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    for (size_t len = 1; len <= 3; ++len) {
        std::vector<uint8_t> src(len, 0xFF);
        std::vector<uint8_t> dst(len, 0x00);
        masked_copy(dst.data(), src.data(), len, mask);
        for (size_t i = 0; i < len; ++i) {
            EXPECT_EQ(dst[i], static_cast<uint8_t>(0xFF ^ mask[i % 4]))
                << "Mismatch at byte " << i << " for len=" << len;
        }
    }
}
