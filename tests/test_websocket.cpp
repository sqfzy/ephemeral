/// @file test_websocket.cpp
/// Unit tests for WebSocket frame encode/decode, masking, and edge cases.

#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "eph/dpdk/websocket.hpp"

using namespace eph::dpdk::ws;

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
    // After one mask, data should differ from original
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
            // Verify mask was applied (XOR changes the data)
            bool changed = (std::memcmp(data.data(), original.data(), len) != 0);
            EXPECT_TRUE(changed) << "len=" << len;
        }

        // Verify symmetry
        apply_mask(data.data(), len, mask);
        EXPECT_EQ(data, original) << "len=" << len;
    }
}

TEST(WsMasking, MaskedCopyMatchesApplyMask) {
    uint8_t mask[4] = {0xDE, 0xAD, 0xBE, 0xEF};

    for (size_t len : {0, 1, 3, 4, 7, 8, 15, 16, 63, 64, 127, 128, 255, 256, 512}) {
        std::vector<uint8_t> src(len);
        for (size_t i = 0; i < len; ++i) src[i] = static_cast<uint8_t>(i & 0xFF);

        // Method 1: memcpy + apply_mask
        std::vector<uint8_t> dst1(len);
        std::memcpy(dst1.data(), src.data(), len);
        apply_mask(dst1.data(), len, mask);

        // Method 2: masked_copy
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

    // Encode
    std::vector<uint8_t> frame_buf(kMaxFrameHeaderLen + payload_len);
    size_t frame_len = encode_frame(
        frame_buf.data(), opcode::kBinary,
        payload.data(), payload_len);

    ASSERT_GT(frame_len, 0u);
    EXPECT_EQ(frame_len, total_frame_size(payload_len));

    // Decode
    auto result = decode_frame(frame_buf.data(), frame_len);
    ASSERT_TRUE(result.has_value()) << result.error();

    auto& frame = *result;
    EXPECT_EQ(frame.opcode, opcode::kBinary);
    EXPECT_TRUE(frame.fin);
    EXPECT_TRUE(frame.masked); // Client frames are always masked
    EXPECT_EQ(frame.payload_len, payload_len);
    EXPECT_EQ(frame.total_len, frame_len);

    // Unmask and verify payload content
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
    EXPECT_EQ(result.error(), "incomplete");
}

TEST(WsDecode, EmptyInputReturnsIncomplete) {
    auto result = decode_frame(nullptr, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "incomplete");
}

TEST(WsDecode, TruncatedPayloadReturnsIncomplete) {
    // Binary frame, payload_len=10, but only 6 bytes provided (header only)
    uint8_t data[] = {0x82, 0x0A, 0, 0, 0, 0};
    auto result = decode_frame(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "incomplete");
}

TEST(WsDecode, IntegerOverflowProtection) {
    // Craft a frame with 8-byte extended length = UINT64_MAX
    uint8_t data[14] = {};
    data[0] = 0x82; // FIN + binary
    data[1] = 127;  // 8-byte extended length
    // Set all 8 length bytes to 0xFF
    std::memset(data + 2, 0xFF, 8);

    auto result = decode_frame(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "incomplete");
}

TEST(WsDecode, UnmaskedServerFrame) {
    // Server frames are unmasked: FIN + binary, len=3, payload "abc"
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
    data[0] = 0x82; // FIN + binary
    data[1] = 126;  // 2-byte extended
    data[2] = 0x00; // 200 >> 8
    data[3] = 200;  // 200 & 0xFF

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
