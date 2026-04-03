/// @file test_websocket.cpp
/// Tests for WebSocket protocol utilities: frame encode/decode, close handshake,
/// masking, fragmentation helpers, and control frame builders.

#include <array>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "eph/transport/detail/websocket.hpp"

using namespace eph::net::ws;

// =======================================================================
// DecodedFrame::close_status_code — masking correctness
// =======================================================================

TEST(DecodedFrameCloseStatusCode, UnmaskedFrameReturnsCorrectCode) {
    // Simulate an unmasked Close frame with status code 1000 (0x03E8)
    uint8_t payload[] = {0x03, 0xE8};
    DecodedFrame frame;
    frame.opcode = opcode::kClose;
    frame.payload = payload;
    frame.payload_len = 2;
    frame.masked = false;
    EXPECT_EQ(frame.close_status_code(), 1000);
}

TEST(DecodedFrameCloseStatusCode, MaskedFrameUnmasksCorrectly) {
    // Simulate a masked Close frame with status code 1001 (0x03E9)
    // Mask key: {0xAA, 0xBB, 0xCC, 0xDD}
    // Masked bytes: 0x03^0xAA=0xA9, 0xE9^0xBB=0x52
    uint8_t payload[] = {0x03 ^ 0xAA, 0xE9 ^ 0xBB};
    DecodedFrame frame;
    frame.opcode = opcode::kClose;
    frame.payload = payload;
    frame.payload_len = 2;
    frame.masked = true;
    frame.mask_key[0] = 0xAA;
    frame.mask_key[1] = 0xBB;
    frame.mask_key[2] = 0xCC;
    frame.mask_key[3] = 0xDD;
    EXPECT_EQ(frame.close_status_code(), 1001);
}

TEST(DecodedFrameCloseStatusCode, NonCloseFrameReturnsZero) {
    uint8_t payload[] = {0x03, 0xE8};
    DecodedFrame frame;
    frame.opcode = opcode::kBinary;
    frame.payload = payload;
    frame.payload_len = 2;
    EXPECT_EQ(frame.close_status_code(), 0);
}

TEST(DecodedFrameCloseStatusCode, EmptyPayloadReturnsZero) {
    DecodedFrame frame;
    frame.opcode = opcode::kClose;
    frame.payload = nullptr;
    frame.payload_len = 0;
    EXPECT_EQ(frame.close_status_code(), 0);
}

TEST(DecodedFrameCloseStatusCode, SingleBytePayloadReturnsZero) {
    uint8_t payload[] = {0x03};
    DecodedFrame frame;
    frame.opcode = opcode::kClose;
    frame.payload = payload;
    frame.payload_len = 1;
    EXPECT_EQ(frame.close_status_code(), 0);
}

// =======================================================================
// DecodedFrame::close_reason_unmasked
// =======================================================================

TEST(DecodedFrameCloseReason, UnmaskedReasonExtractedCorrectly) {
    // Close frame: code 1000 + reason "bye"
    uint8_t payload[] = {0x03, 0xE8, 'b', 'y', 'e'};
    DecodedFrame frame;
    frame.opcode = opcode::kClose;
    frame.payload = payload;
    frame.payload_len = 5;
    frame.masked = false;
    EXPECT_EQ(frame.close_reason(), "bye");
    EXPECT_EQ(frame.close_reason_unmasked(), "bye");
}

TEST(DecodedFrameCloseReason, MaskedReasonUnmaskedCorrectly) {
    // Mask key: {0xAA, 0xBB, 0xCC, 0xDD}
    // Payload "bye" at offset 2: mask key rotates: CC, DD, AA
    uint8_t payload[] = {
        0x03 ^ 0xAA, 0xE8 ^ 0xBB,
        'b' ^ 0xCC, 'y' ^ 0xDD, 'e' ^ 0xAA
    };
    DecodedFrame frame;
    frame.opcode = opcode::kClose;
    frame.payload = payload;
    frame.payload_len = 5;
    frame.masked = true;
    frame.mask_key[0] = 0xAA;
    frame.mask_key[1] = 0xBB;
    frame.mask_key[2] = 0xCC;
    frame.mask_key[3] = 0xDD;
    EXPECT_EQ(frame.close_status_code(), 1000);
    EXPECT_EQ(frame.close_reason_unmasked(), "bye");
}

TEST(DecodedFrameCloseReason, NoReasonReturnsEmpty) {
    uint8_t payload[] = {0x03, 0xE8};
    DecodedFrame frame;
    frame.opcode = opcode::kClose;
    frame.payload = payload;
    frame.payload_len = 2;
    frame.masked = false;
    EXPECT_TRUE(frame.close_reason().empty());
    EXPECT_TRUE(frame.close_reason_unmasked().empty());
}

TEST(DecodedFrameCloseReason, NonCloseReturnsEmpty) {
    DecodedFrame frame;
    frame.opcode = opcode::kPing;
    frame.payload_len = 0;
    EXPECT_TRUE(frame.close_reason_unmasked().empty());
}

// =======================================================================
// decode_frame — edge cases
// =======================================================================

TEST(DecodeFrame, MinimalUnmaskedPingFrame) {
    // FIN=1, opcode=0x9 (ping), no mask, payload_len=0
    uint8_t data[] = {0x89, 0x00};
    auto result = decode_frame(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_ping());
    EXPECT_TRUE(result->fin);
    EXPECT_FALSE(result->masked);
    EXPECT_EQ(result->payload_len, 0u);
    EXPECT_EQ(result->total_len, 2u);
}

TEST(DecodeFrame, MaskedBinaryFrameSmallPayload) {
    // FIN=1, opcode=0x2 (binary), MASK=1, payload_len=4
    uint8_t data[] = {
        0x82,                         // FIN + binary
        0x84,                         // MASK + len=4
        0x01, 0x02, 0x03, 0x04,       // mask key
        'h' ^ 0x01, 'e' ^ 0x02,      // masked payload
        'l' ^ 0x03, 'l' ^ 0x04,
    };
    auto result = decode_frame(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->fin);
    EXPECT_EQ(result->opcode, opcode::kBinary);
    EXPECT_TRUE(result->masked);
    EXPECT_EQ(result->payload_len, 4u);
    EXPECT_EQ(result->total_len, sizeof(data));
}

TEST(DecodeFrame, ExtendedPayloadLen126) {
    // FIN=1, opcode=0x2, no mask, payload_len=200 (uses 126 extended format)
    std::vector<uint8_t> data(4 + 200);
    data[0] = 0x82;         // FIN + binary
    data[1] = 126;          // extended 16-bit length
    data[2] = 0x00;         // length high byte
    data[3] = 200;          // length low byte
    auto result = decode_frame(data.data(), data.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, 200u);
    EXPECT_EQ(result->total_len, 4u + 200u);
}

TEST(DecodeFrame, ReservedBitsRejected) {
    // RSV1 set (0x40) — should be rejected
    uint8_t data[] = {0xC2, 0x00};  // FIN + RSV1 + binary, len=0
    auto result = decode_frame(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kReservedBits);
}

TEST(DecodeFrame, ReservedOpcodeRejected) {
    // Opcode 0x03 is reserved
    uint8_t data[] = {0x83, 0x00};  // FIN + opcode=3, len=0
    auto result = decode_frame(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kInvalidOpcode);
}

TEST(DecodeFrame, FragmentedControlFrameRejected) {
    // Ping with FIN=0 — RFC 6455 says control frames MUST NOT be fragmented
    uint8_t data[] = {0x09, 0x00};  // no FIN + ping, len=0
    auto result = decode_frame(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kFragmentedControl);
}

TEST(DecodeFrame, ControlFramePayloadTooLarge) {
    // Ping with 126-byte payload (> 125 limit)
    std::vector<uint8_t> data(2 + 126);
    data[0] = 0x89;         // FIN + ping
    data[1] = 126;          // extended length indicator (actual len in next 2 bytes)
    // This triggers the >125 check for control frames
    // Actually, len_byte=126 means extended 16-bit length:
    // We need to set a 16-bit length > 125
    data.resize(4 + 126);
    data[0] = 0x89;
    data[1] = 126;          // extended 16-bit
    data[2] = 0x00;
    data[3] = 126;          // 126 bytes > 125 limit
    auto result = decode_frame(data.data(), data.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kControlPayloadTooLarge);
}

TEST(DecodeFrame, IncompleteHeaderReturnsIncomplete) {
    uint8_t data[] = {0x82};  // just one byte
    auto result = decode_frame(data, 1);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

TEST(DecodeFrame, IncompletePayloadReturnsIncomplete) {
    // Header says 10 bytes payload but only 5 available
    uint8_t data[] = {0x82, 0x0A, 0, 0, 0, 0, 0};
    auto result = decode_frame(data, sizeof(data));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), DecodeError::kIncomplete);
}

TEST(DecodeFrame, CloseFrameWithReasonDecoded) {
    // Build a close frame with code 1000 and reason "going away"
    uint8_t buf[128];
    size_t len = build_close_frame(buf, close_code::kNormal, "going away");
    ASSERT_GT(len, 0u);

    // Decode it back (it's a masked client frame)
    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_close());
    EXPECT_TRUE(result->fin);
    // Status code should unmask correctly
    EXPECT_EQ(result->close_status_code(), 1000);
    // Reason should unmask correctly
    auto reason = result->close_reason_unmasked();
    EXPECT_EQ(reason, "going away");
}

TEST(DecodeFrame, ContinuationFrameAccepted) {
    // FIN=0, opcode=0 (continuation), no mask, len=5
    uint8_t data[] = {0x00, 0x05, 'h', 'e', 'l', 'l', 'o'};
    auto result = decode_frame(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kContinuation);
    EXPECT_FALSE(result->fin);
    EXPECT_EQ(result->payload_len, 5u);
}

// =======================================================================
// encode_frame — roundtrip
// =======================================================================

TEST(EncodeFrame, EncodeDecodeRoundtrip) {
    uint8_t payload[] = "hello world";
    uint8_t buf[128];
    size_t len = encode_frame(buf, opcode::kBinary, payload, sizeof(payload) - 1);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kBinary);
    EXPECT_TRUE(result->fin);
    EXPECT_TRUE(result->masked);  // encode_frame always masks (client)
    EXPECT_EQ(result->payload_len, sizeof(payload) - 1);

    // Unmask and verify payload
    std::vector<uint8_t> unmasked(result->payload, result->payload + result->payload_len);
    apply_mask(unmasked.data(), unmasked.size(), result->mask_key);
    EXPECT_EQ(std::memcmp(unmasked.data(), payload, sizeof(payload) - 1), 0);
}

TEST(EncodeFrame, TextFrameRoundtrip) {
    uint8_t payload[] = "utf8 text";
    uint8_t buf[128];
    size_t len = encode_frame(buf, opcode::kText, payload, sizeof(payload) - 1);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kText);
}

TEST(EncodeFrame, ZeroLengthPayload) {
    uint8_t buf[32];
    size_t len = encode_frame(buf, opcode::kBinary, nullptr, 0);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, 0u);
}

TEST(EncodeFrame, ControlFramePayloadTooLargeReturnsZero) {
    // Ping with 126-byte payload should fail (> 125)
    std::array<uint8_t, 126> big_payload{};
    uint8_t buf[256];
    size_t len = encode_frame(buf, opcode::kPing, big_payload.data(), big_payload.size());
    EXPECT_EQ(len, 0u);
}

// =======================================================================
// build_close_frame / build_ping_frame / build_pong_frame
// =======================================================================

TEST(BuildCloseFrame, NormalCloseNoReason) {
    uint8_t buf[64];
    size_t len = build_close_frame(buf, close_code::kNormal);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_close());
    EXPECT_EQ(result->close_status_code(), 1000);
    EXPECT_TRUE(result->close_reason_unmasked().empty());
}

TEST(BuildCloseFrame, ReasonTruncatedAt123Bytes) {
    std::string long_reason(200, 'x');
    uint8_t buf[256];
    size_t len = build_close_frame(buf, close_code::kGoingAway, long_reason);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->close_status_code(), 1001);
    // 2 bytes code + 123 bytes reason = 125 payload
    EXPECT_EQ(result->payload_len, 125u);
}

TEST(BuildPingFrame, EmptyPing) {
    uint8_t buf[32];
    size_t len = build_ping_frame(buf);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_ping());
    EXPECT_EQ(result->payload_len, 0u);
}

TEST(BuildPingFrame, PingWithPayload) {
    uint8_t payload[] = {0x01, 0x02, 0x03};
    uint8_t buf[32];
    size_t len = build_ping_frame(buf, payload, sizeof(payload));
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_ping());
    EXPECT_EQ(result->payload_len, 3u);
}

TEST(BuildPingFrame, NullPayloadClampsLenToZero) {
    uint8_t buf[32];
    size_t len = build_ping_frame(buf, nullptr, 10);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload_len, 0u);
}

TEST(BuildPongFrame, NullPayloadClampsLenToZero) {
    uint8_t buf[32];
    size_t len = build_pong_frame(buf, nullptr, 5);
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_pong());
    EXPECT_EQ(result->payload_len, 0u);
}

// =======================================================================
// is_valid_close_code — RFC 6455 §7.4
// =======================================================================

TEST(IsValidCloseCode, StandardCodesValid) {
    EXPECT_TRUE(is_valid_close_code(1000));
    EXPECT_TRUE(is_valid_close_code(1001));
    EXPECT_TRUE(is_valid_close_code(1002));
    EXPECT_TRUE(is_valid_close_code(1003));
    EXPECT_TRUE(is_valid_close_code(1007));
    EXPECT_TRUE(is_valid_close_code(1008));
    EXPECT_TRUE(is_valid_close_code(1009));
    EXPECT_TRUE(is_valid_close_code(1010));
    EXPECT_TRUE(is_valid_close_code(1011));
}

TEST(IsValidCloseCode, ReservedCodesInvalid) {
    EXPECT_FALSE(is_valid_close_code(1004));
    EXPECT_FALSE(is_valid_close_code(1005));
    EXPECT_FALSE(is_valid_close_code(1006));  // kAbnormalClosure (reserved, must not be sent)
    EXPECT_FALSE(is_valid_close_code(1015));
}

TEST(IsValidCloseCode, RegisteredAndPrivateRanges) {
    EXPECT_TRUE(is_valid_close_code(3000));
    EXPECT_TRUE(is_valid_close_code(3999));
    EXPECT_TRUE(is_valid_close_code(4000));
    EXPECT_TRUE(is_valid_close_code(4999));
}

TEST(IsValidCloseCode, OutOfRangeInvalid) {
    EXPECT_FALSE(is_valid_close_code(0));
    EXPECT_FALSE(is_valid_close_code(999));
    EXPECT_FALSE(is_valid_close_code(1012));  // not in standard set
    EXPECT_FALSE(is_valid_close_code(2999));
    EXPECT_FALSE(is_valid_close_code(5000));
}

// =======================================================================
// close_code_name
// =======================================================================

TEST(CloseCodeName, KnownCodesReturnNames) {
    EXPECT_EQ(close_code_name(1000), "NORMAL_CLOSURE");
    EXPECT_EQ(close_code_name(1001), "GOING_AWAY");
    EXPECT_EQ(close_code_name(1002), "PROTOCOL_ERROR");
    EXPECT_EQ(close_code_name(1006), "ABNORMAL_CLOSURE");
    EXPECT_EQ(close_code_name(1011), "INTERNAL_ERROR");
}

TEST(CloseCodeName, RegisteredRangePrefix) {
    auto name = close_code_name(3500);
    EXPECT_NE(name.find("REGISTERED"), std::string_view::npos);
}

TEST(CloseCodeName, PrivateRangePrefix) {
    auto name = close_code_name(4500);
    EXPECT_NE(name.find("PRIVATE"), std::string_view::npos);
}

TEST(CloseCodeName, UnknownCodePrefix) {
    auto name = close_code_name(999);
    EXPECT_NE(name.find("UNKNOWN"), std::string_view::npos);
}

// =======================================================================
// decode_error_name
// =======================================================================

TEST(DecodeErrorName, AllEnumValuesHaveNames) {
    EXPECT_EQ(decode_error_name(DecodeError::kIncomplete), "incomplete");
    EXPECT_EQ(decode_error_name(DecodeError::kReservedBits),
              "non-zero RSV bits without negotiated extension");
    EXPECT_EQ(decode_error_name(DecodeError::kFragmentedControl),
              "fragmented control frame");
    EXPECT_EQ(decode_error_name(DecodeError::kControlPayloadTooLarge),
              "control frame payload exceeds 125 bytes");
    EXPECT_EQ(decode_error_name(DecodeError::kInvalidOpcode),
              "reserved opcode (RFC 6455 \xC2\xA7""5.2)");
}

// =======================================================================
// apply_mask — XOR masking correctness
// =======================================================================

TEST(ApplyMask, RoundtripRecoverOriginal) {
    uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    uint8_t original[sizeof(data)];
    std::memcpy(original, data, sizeof(data));

    uint8_t mask[] = {0xAA, 0xBB, 0xCC, 0xDD};
    apply_mask(data, sizeof(data), mask);
    // Verify it changed
    EXPECT_NE(std::memcmp(data, original, sizeof(data)), 0);
    // Apply again to recover
    apply_mask(data, sizeof(data), mask);
    EXPECT_EQ(std::memcmp(data, original, sizeof(data)), 0);
}

TEST(ApplyMask, ZeroLengthNoOp) {
    uint8_t data[] = {0x42};
    uint8_t mask[] = {0xFF, 0xFF, 0xFF, 0xFF};
    apply_mask(data, 0, mask);
    EXPECT_EQ(data[0], 0x42);
}

// =======================================================================
// masked_copy — fused copy + XOR
// =======================================================================

TEST(MaskedCopy, MatchesApplyMaskResult) {
    uint8_t src[37];
    for (size_t i = 0; i < sizeof(src); ++i) src[i] = static_cast<uint8_t>(i);

    uint8_t mask[] = {0x12, 0x34, 0x56, 0x78};

    // masked_copy
    uint8_t dst1[sizeof(src)];
    masked_copy(dst1, src, sizeof(src), mask);

    // manual: copy then mask
    uint8_t dst2[sizeof(src)];
    std::memcpy(dst2, src, sizeof(src));
    apply_mask(dst2, sizeof(src), mask);

    EXPECT_EQ(std::memcmp(dst1, dst2, sizeof(src)), 0);
}

// =======================================================================
// frame_header_size / total_frame_size — constexpr utilities
// =======================================================================

TEST(FrameHeaderSize, SmallPayload) {
    EXPECT_EQ(frame_header_size(0), 6u);    // 2 + 4 (mask)
    EXPECT_EQ(frame_header_size(125), 6u);
}

TEST(FrameHeaderSize, MediumPayload) {
    EXPECT_EQ(frame_header_size(126), 8u);  // 2 + 2 + 4
    EXPECT_EQ(frame_header_size(65535), 8u);
}

TEST(FrameHeaderSize, LargePayload) {
    EXPECT_EQ(frame_header_size(65536), 14u);  // 2 + 8 + 4
}

TEST(TotalFrameSize, CalculatesCorrectly) {
    EXPECT_EQ(total_frame_size(100), 6u + 100u);
    EXPECT_EQ(total_frame_size(200), 8u + 200u);
    EXPECT_EQ(total_frame_size(70000), 14u + 70000u);
}

// =======================================================================
// is_valid_payload_len
// =======================================================================

TEST(IsValidPayloadLen, DataFrameAcceptsLargePayload) {
    EXPECT_TRUE(is_valid_payload_len(opcode::kBinary, 65535));
    EXPECT_TRUE(is_valid_payload_len(opcode::kText, 100000));
}

TEST(IsValidPayloadLen, ControlFrameRejectsOver125) {
    EXPECT_TRUE(is_valid_payload_len(opcode::kPing, 125));
    EXPECT_FALSE(is_valid_payload_len(opcode::kPing, 126));
    EXPECT_FALSE(is_valid_payload_len(opcode::kClose, 126));
    EXPECT_FALSE(is_valid_payload_len(opcode::kPong, 200));
}

// =======================================================================
// FrameTemplate
// =======================================================================

TEST(FrameTemplate, ForBinaryEncodesCorrectly) {
    auto tmpl = FrameTemplate::for_binary();
    EXPECT_EQ(tmpl.opcode_val, opcode::kBinary);
    EXPECT_TRUE(tmpl.fin);

    uint8_t payload[] = {1, 2, 3};
    uint8_t buf[32];
    size_t len = tmpl.encode(buf, payload, sizeof(payload));
    ASSERT_GT(len, 0u);

    auto result = decode_frame(buf, len);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->opcode, opcode::kBinary);
    EXPECT_TRUE(result->fin);
}

TEST(FrameTemplate, ForTextEncodesCorrectly) {
    auto tmpl = FrameTemplate::for_text();
    EXPECT_EQ(tmpl.opcode_val, opcode::kText);
    EXPECT_TRUE(tmpl.fin);
}

// =======================================================================
// DecodedFrame helper methods
// =======================================================================

TEST(DecodedFrame, IsControlVsData) {
    DecodedFrame frame;

    frame.opcode = opcode::kBinary;
    EXPECT_TRUE(frame.is_data());
    EXPECT_FALSE(frame.is_control());

    frame.opcode = opcode::kText;
    EXPECT_TRUE(frame.is_data());
    EXPECT_FALSE(frame.is_control());

    frame.opcode = opcode::kContinuation;
    EXPECT_TRUE(frame.is_data());
    EXPECT_FALSE(frame.is_control());

    frame.opcode = opcode::kPing;
    EXPECT_FALSE(frame.is_data());
    EXPECT_TRUE(frame.is_control());

    frame.opcode = opcode::kPong;
    EXPECT_FALSE(frame.is_data());
    EXPECT_TRUE(frame.is_control());

    frame.opcode = opcode::kClose;
    EXPECT_FALSE(frame.is_data());
    EXPECT_TRUE(frame.is_control());
}

// =======================================================================
// is_valid_utf8 — RFC 6455 §5.6 text frame payload validation
// =======================================================================

TEST(IsValidUtf8, EmptyInputIsValid) {
    EXPECT_TRUE(is_valid_utf8(nullptr, 0));
}

TEST(IsValidUtf8, PureAsciiIsValid) {
    std::string s = "Hello, World! 123 !@#$%";
    EXPECT_TRUE(is_valid_utf8(
        reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

TEST(IsValidUtf8, TwoByteSequence) {
    // U+00E9 (e-acute): 0xC3 0xA9
    uint8_t data[] = {0xC3, 0xA9};
    EXPECT_TRUE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, ThreeByteSequence) {
    // U+20AC (Euro sign): 0xE2 0x82 0xAC
    uint8_t data[] = {0xE2, 0x82, 0xAC};
    EXPECT_TRUE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, FourByteSequence) {
    // U+1F600 (grinning face): 0xF0 0x9F 0x98 0x80
    uint8_t data[] = {0xF0, 0x9F, 0x98, 0x80};
    EXPECT_TRUE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, InvalidContinuationByteAlone) {
    // 0x80-0xBF are continuation bytes; standalone is invalid
    uint8_t data[] = {0x80};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, TruncatedTwoByteSequence) {
    // 0xC3 expects a continuation byte, but ends prematurely
    uint8_t data[] = {0xC3};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, TruncatedThreeByteSequence) {
    // 0xE2 expects two continuation bytes, only one provided
    uint8_t data[] = {0xE2, 0x82};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, TruncatedFourByteSequence) {
    // 0xF0 expects three continuation bytes, only two provided
    uint8_t data[] = {0xF0, 0x9F, 0x98};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, InvalidByte0xFF) {
    uint8_t data[] = {0xFF};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, InvalidByte0xFE) {
    uint8_t data[] = {0xFE};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, OverlongTwoByteEncoding) {
    // U+002F (/) as overlong 2-byte: 0xC0 0xAF (should be just 0x2F)
    uint8_t data[] = {0xC0, 0xAF};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, OverlongThreeByteEncoding) {
    // U+002F as overlong 3-byte: 0xE0 0x80 0xAF
    uint8_t data[] = {0xE0, 0x80, 0xAF};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, SurrogateHalfRejected) {
    // U+D800 (high surrogate): 0xED 0xA0 0x80 — invalid in UTF-8
    uint8_t data[] = {0xED, 0xA0, 0x80};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, MaxValidCodePoint) {
    // U+10FFFF: 0xF4 0x8F 0xBF 0xBF
    uint8_t data[] = {0xF4, 0x8F, 0xBF, 0xBF};
    EXPECT_TRUE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, BeyondMaxCodePointRejected) {
    // U+110000: 0xF4 0x90 0x80 0x80 — beyond max Unicode scalar value
    uint8_t data[] = {0xF4, 0x90, 0x80, 0x80};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, MixedAsciiAndMultibyte) {
    // "Hello éàü"
    std::string s = "Hello \xC3\xA9\xC3\xA0\xC3\xBC";
    EXPECT_TRUE(is_valid_utf8(
        reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

TEST(IsValidUtf8, InvalidByteInMiddle) {
    // Valid ASCII, then invalid 0xFF, then valid ASCII
    uint8_t data[] = {'A', 'B', 0xFF, 'C', 'D'};
    EXPECT_FALSE(is_valid_utf8(data, sizeof(data)));
}

TEST(IsValidUtf8, StringViewOverload) {
    std::string_view sv = "valid UTF-8 string";
    EXPECT_TRUE(is_valid_utf8(sv));
    std::string_view bad("\xFF", 1);
    EXPECT_FALSE(is_valid_utf8(bad));
}

// =======================================================================
// MaskKeyCache — batch mask key generation
// =======================================================================

TEST(MaskKeyCache, ProducesNonZeroKeys) {
    MaskKeyCache cache;
    uint8_t key[4]{};
    cache.next_key(key);
    // Very unlikely that all 4 bytes are zero from CSPRNG
    bool all_zero = (key[0] == 0 && key[1] == 0 && key[2] == 0 && key[3] == 0);
    // Not a hard failure since it's probabilistic, but assert some activity
    EXPECT_FALSE(all_zero);
}

TEST(MaskKeyCache, ConsecutiveKeysAreDifferent) {
    MaskKeyCache cache;
    uint8_t key1[4]{}, key2[4]{};
    cache.next_key(key1);
    cache.next_key(key2);
    // Two consecutive keys from CSPRNG should differ with overwhelming probability
    EXPECT_NE(std::memcmp(key1, key2, 4), 0);
}

TEST(MaskKeyCache, ExhaustsPoolAndRefills) {
    MaskKeyCache cache;
    uint8_t key[4]{};
    // Exhaust the pool (1024 keys) + 1 to trigger refill
    for (size_t i = 0; i < MaskKeyCache::kPoolSize + 1; ++i) {
        cache.next_key(key);
    }
    // Should still produce a valid key after refill
    bool all_zero = (key[0] == 0 && key[1] == 0 && key[2] == 0 && key[3] == 0);
    EXPECT_FALSE(all_zero);
}
