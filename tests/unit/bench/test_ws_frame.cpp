/// @file tests/unit/bench/test_ws_frame.cpp
/// Server-side WS frame build/parse tests.
///
/// Covers all three RFC 6455 length encodings (≤125, 126→2-byte ext,
/// 127→8-byte ext) plus client mask round-trip.

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "mock/lib/ws_frame.hpp"

using namespace bench::mock;

// ── Server frame build ───────────────────────────────────────────────────

TEST(BenchWsFrameBuild, ShortPayloadSingleByteLength) {
    std::array<uint8_t, 5> payload{1, 2, 3, 4, 5};
    std::array<uint8_t, 32> out{};
    size_t total = build_server_frame(out.data(), kOpText,
                                      payload.data(), payload.size());
    ASSERT_EQ(total, 2u + 5u);
    EXPECT_EQ(out[0], 0x80 | kOpText);
    EXPECT_EQ(out[1], 5u);
    EXPECT_EQ(std::memcmp(out.data() + 2, payload.data(), 5), 0);
}

TEST(BenchWsFrameBuild, BoundaryPayload125UsesSingleByteLength) {
    std::vector<uint8_t> payload(125, 0xAA);
    std::vector<uint8_t> out(256);
    size_t total = build_server_frame(out.data(), kOpBinary,
                                      payload.data(), payload.size());
    EXPECT_EQ(total, 2u + 125u);
    EXPECT_EQ(out[1], 125u);
}

TEST(BenchWsFrameBuild, Payload126UsesExtended16) {
    std::vector<uint8_t> payload(126, 0x55);
    std::vector<uint8_t> out(256);
    size_t total = build_server_frame(out.data(), kOpBinary,
                                      payload.data(), payload.size());
    EXPECT_EQ(total, 4u + 126u);
    EXPECT_EQ(out[1], 126u);
    uint16_t ext = (uint16_t(out[2]) << 8) | out[3];
    EXPECT_EQ(ext, 126u);
}

TEST(BenchWsFrameBuild, Payload65536UsesExtended64) {
    std::vector<uint8_t> payload(65536, 0xCC);
    std::vector<uint8_t> out(70000);
    size_t total = build_server_frame(out.data(), kOpBinary,
                                      payload.data(), payload.size());
    EXPECT_EQ(total, 10u + 65536u);
    EXPECT_EQ(out[1], 127u);
    uint64_t ext = 0;
    for (int i = 0; i < 8; ++i) ext = (ext << 8) | out[2 + i];
    EXPECT_EQ(ext, 65536u);
}

// ── Client frame parse + unmask ──────────────────────────────────────────

namespace {
// Build a client→server frame with the given payload, masked.
std::vector<uint8_t> build_masked_client(uint8_t opcode,
                                         const uint8_t* payload, size_t len,
                                         uint8_t mask[4]) {
    std::vector<uint8_t> buf;
    buf.push_back(static_cast<uint8_t>(0x80 | opcode));
    if (len < 126) {
        buf.push_back(static_cast<uint8_t>(0x80 | len));
    } else if (len < 65536) {
        buf.push_back(static_cast<uint8_t>(0x80 | 126));
        buf.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        buf.push_back(static_cast<uint8_t>(0x80 | 127));
        for (int i = 7; i >= 0; --i) {
            buf.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }
    for (int i = 0; i < 4; ++i) buf.push_back(mask[i]);
    for (size_t i = 0; i < len; ++i) {
        buf.push_back(payload[i] ^ mask[i & 3]);
    }
    return buf;
}
} // namespace

TEST(BenchWsFrameParse, ShortMaskedPayloadRoundTrip) {
    uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    uint8_t mask[4]   = {0xDE, 0xAD, 0xBE, 0xEF};
    auto buf = build_masked_client(kOpText, payload, 5, mask);

    auto f = parse_client_frame_inplace(buf.data(), buf.size(), 1024);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->opcode, kOpText);
    EXPECT_EQ(f->payload_len, 5u);
    EXPECT_EQ(f->total_consumed, buf.size());

    // Unmasked bytes are at the tail of the buffer.
    const uint8_t* p = buf.data() + (buf.size() - 5);
    EXPECT_EQ(std::memcmp(p, payload, 5), 0);
}

TEST(BenchWsFrameParse, ExtendedLength126RoundTrip) {
    std::vector<uint8_t> payload(200, 0x77);
    uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    auto buf = build_masked_client(kOpBinary, payload.data(), payload.size(), mask);
    auto f = parse_client_frame_inplace(buf.data(), buf.size(), 4096);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->payload_len, 200u);
    const uint8_t* p = buf.data() + (buf.size() - 200);
    for (size_t i = 0; i < 200; ++i) EXPECT_EQ(p[i], 0x77);
}

TEST(BenchWsFrameParse, RejectsOverlimit) {
    std::vector<uint8_t> payload(2000, 0x33);
    uint8_t mask[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    auto buf = build_masked_client(kOpBinary, payload.data(), payload.size(), mask);
    auto f = parse_client_frame_inplace(buf.data(), buf.size(), /*max*/ 1024);
    EXPECT_FALSE(f.has_value());
}

TEST(BenchWsFrameParse, RejectsTruncated) {
    uint8_t buf[2] = {0x81, 0x85};  // claims 5 byte masked payload
    auto f = parse_client_frame_inplace(buf, 2, 1024);
    EXPECT_FALSE(f.has_value());
}
