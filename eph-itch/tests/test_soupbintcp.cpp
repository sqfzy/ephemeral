#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "eph/itch/soupbintcp.hpp"

using eph::itch::SoupBinTcpFramer;
using eph::itch::soupbin::kClientHeartbeat;
using eph::itch::soupbin::kLoginAccepted;
using eph::itch::soupbin::kSequencedData;
using eph::itch::soupbin::kServerHeartbeat;
using eph::net::FrameError;

// --- Concept satisfaction -------------------------------------------------

static_assert(eph::net::MessageFramer<SoupBinTcpFramer>,
    "SoupBinTcpFramer must satisfy MessageFramer concept");

// --- Decode tests ---------------------------------------------------------

TEST(SoupBinTcpFramer, decode_sequenced_data_packet) {
    // Sequenced Data 'S' with 4-byte payload "ABCD"
    //   length = 5 (1 type + 4 payload), big-endian: 0x00 0x05
    const std::array<uint8_t, 7> buf = {0x00, 0x05, 'S', 'A', 'B', 'C', 'D'};

    SoupBinTcpFramer framer;
    auto result = framer.decode(buf.data(), buf.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, kSequencedData);
    EXPECT_EQ(result->payload_len, 4u);
    EXPECT_EQ(result->total_len, 7u);
    EXPECT_FALSE(result->is_control);
    EXPECT_EQ(std::memcmp(result->payload, "ABCD", 4), 0);
}

TEST(SoupBinTcpFramer, decode_server_heartbeat_empty_payload) {
    // Server heartbeat 'H' — length = 1 (type byte only, no payload)
    const std::array<uint8_t, 3> buf = {0x00, 0x01, 'H'};

    SoupBinTcpFramer framer;
    auto result = framer.decode(buf.data(), buf.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, kServerHeartbeat);
    EXPECT_EQ(result->payload_len, 0u);
    EXPECT_EQ(result->total_len, 3u);
    EXPECT_TRUE(result->is_control);
    EXPECT_EQ(result->payload, nullptr);
}

TEST(SoupBinTcpFramer, decode_login_accepted) {
    // Login Accepted 'A' with 30-byte payload (session + seq number fields)
    std::vector<uint8_t> buf(2 + 1 + 30);
    const uint16_t wire_len = 31;  // 1 type + 30 payload
    buf[0] = static_cast<uint8_t>((wire_len >> 8) & 0xFF);
    buf[1] = static_cast<uint8_t>(wire_len & 0xFF);
    buf[2] = 'A';
    // Fill payload with dummy session data
    std::memset(buf.data() + 3, ' ', 30);

    SoupBinTcpFramer framer;
    auto result = framer.decode(buf.data(), buf.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, kLoginAccepted);
    EXPECT_EQ(result->payload_len, 30u);
    EXPECT_EQ(result->total_len, 33u);
    EXPECT_FALSE(result->is_control);
}

TEST(SoupBinTcpFramer, decode_incomplete_header) {
    // Only 1 byte available — not enough for the 2-byte length header
    const uint8_t buf = 0x00;

    SoupBinTcpFramer framer;
    auto result = framer.decode(&buf, 1);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

TEST(SoupBinTcpFramer, decode_incomplete_truncated_payload) {
    // Length says 5 bytes (1 type + 4 payload) but only 4 bytes after header
    const std::array<uint8_t, 6> buf = {0x00, 0x05, 'S', 'A', 'B', 'C'};

    SoupBinTcpFramer framer;
    auto result = framer.decode(buf.data(), buf.size());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kIncomplete);
}

TEST(SoupBinTcpFramer, decode_zero_length_returns_invalid_format) {
    // Length field = 0 — invalid, must be at least 1 for type byte
    const std::array<uint8_t, 2> buf = {0x00, 0x00};

    SoupBinTcpFramer framer;
    auto result = framer.decode(buf.data(), buf.size());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), FrameError::kInvalidFormat);
}

TEST(SoupBinTcpFramer, decode_zero_length_payload_after_type) {
    // Client heartbeat 'R' — length = 1 (type only, zero payload bytes)
    const std::array<uint8_t, 3> buf = {0x00, 0x01, 'R'};

    SoupBinTcpFramer framer;
    auto result = framer.decode(buf.data(), buf.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, kClientHeartbeat);
    EXPECT_EQ(result->payload_len, 0u);
    EXPECT_EQ(result->total_len, 3u);
    EXPECT_TRUE(result->is_control);
}

// --- Encode round-trip ----------------------------------------------------

TEST(SoupBinTcpFramer, encode_round_trip) {
    SoupBinTcpFramer framer;

    // Encode a Sequenced Data packet with payload "HELLO"
    const uint8_t payload[] = {'H', 'E', 'L', 'L', 'O'};
    std::array<uint8_t, 64> wire{};

    const size_t written = framer.encode(wire.data(), payload, 5, kSequencedData);
    ASSERT_EQ(written, 8u);  // 2 header + 1 type + 5 payload

    // Verify wire bytes manually
    EXPECT_EQ(wire[0], 0x00);  // length high byte
    EXPECT_EQ(wire[1], 0x06);  // length low byte (5 payload + 1 type = 6)
    EXPECT_EQ(wire[2], 'S');   // type byte

    // Now decode what we encoded
    auto result = framer.decode(wire.data(), written);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, kSequencedData);
    EXPECT_EQ(result->payload_len, 5u);
    EXPECT_EQ(result->total_len, 8u);
    EXPECT_FALSE(result->is_control);
    EXPECT_EQ(std::memcmp(result->payload, payload, 5), 0);
}

TEST(SoupBinTcpFramer, encode_heartbeat_round_trip) {
    SoupBinTcpFramer framer;

    // Encode a server heartbeat (zero payload)
    std::array<uint8_t, 8> wire{};
    const size_t written = framer.encode(wire.data(), nullptr, 0, kServerHeartbeat);
    ASSERT_EQ(written, 3u);  // 2 header + 1 type, no payload

    // Verify wire bytes
    EXPECT_EQ(wire[0], 0x00);
    EXPECT_EQ(wire[1], 0x01);  // length = 1 (type byte only)
    EXPECT_EQ(wire[2], 'H');

    // Decode
    auto result = framer.decode(wire.data(), written);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->msg_type, kServerHeartbeat);
    EXPECT_EQ(result->payload_len, 0u);
    EXPECT_TRUE(result->is_control);
}

// --- Decode chained packets -----------------------------------------------

TEST(SoupBinTcpFramer, decode_chained_two_packets_in_one_buffer) {
    // Two packets back-to-back:
    //   1) Server heartbeat 'H' (3 bytes total)
    //   2) Sequenced Data 'S' with payload "XY" (5 bytes total)
    const std::array<uint8_t, 8> buf = {
        0x00, 0x01, 'H',                    // packet 1: heartbeat
        0x00, 0x03, 'S', 'X', 'Y',          // packet 2: sequenced data
    };

    SoupBinTcpFramer framer;

    // Decode first packet
    auto r1 = framer.decode(buf.data(), buf.size());
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->msg_type, kServerHeartbeat);
    EXPECT_EQ(r1->total_len, 3u);
    EXPECT_TRUE(r1->is_control);

    // Advance buffer past first packet and decode second
    const uint8_t* next = buf.data() + r1->total_len;
    const size_t remaining = buf.size() - r1->total_len;

    auto r2 = framer.decode(next, remaining);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->msg_type, kSequencedData);
    EXPECT_EQ(r2->payload_len, 2u);
    EXPECT_EQ(r2->total_len, 5u);
    EXPECT_FALSE(r2->is_control);
    EXPECT_EQ(std::memcmp(r2->payload, "XY", 2), 0);
}

// ─── encode error paths (round-54) ──────────────────────────────────────
//
// The existing encode tests cover only happy paths (round_trip,
// heartbeat). The three precondition checks at the top of `encode`
// (null out, payload too large, null data with non-zero len) all
// silently return 0 — and none had a test pinning that 0. A refactor
// that swapped any guard for a noisier behavior (e.g. UB on null) or
// that changed the return convention would slip through.

TEST(SoupBinTcpFramer, encode_null_output_returns_zero) {
    SoupBinTcpFramer framer;
    const uint8_t payload[] = {'X'};
    EXPECT_EQ(framer.encode(/*out=*/nullptr, payload, 1, kSequencedData),
              0u);
}

TEST(SoupBinTcpFramer, encode_payload_exceeding_max_returns_zero) {
    // kMaxPayloadLen is 65534. A request for 65535 must reject — the
    // wire-length field is `len + 1`, so 65535 + 1 would overflow u16.
    SoupBinTcpFramer framer;
    std::array<uint8_t, 8> wire{};
    // We never deref a 65535-byte payload; the size check must short-
    // circuit before any access. Pass a small buffer.
    const uint8_t dummy[] = {'X'};
    EXPECT_EQ(framer.encode(wire.data(), dummy, 65535, kSequencedData),
              0u);
}

TEST(SoupBinTcpFramer, encode_null_data_with_nonzero_len_returns_zero) {
    SoupBinTcpFramer framer;
    std::array<uint8_t, 8> wire{};
    EXPECT_EQ(framer.encode(wire.data(), nullptr, 5, kSequencedData),
              0u);
    // wire must NOT have been touched.
    EXPECT_EQ(wire[0], 0u);
    EXPECT_EQ(wire[1], 0u);
    EXPECT_EQ(wire[2], 0u);
}

TEST(SoupBinTcpFramer, encode_null_data_with_zero_len_succeeds) {
    // Pinned by the heartbeat round-trip test indirectly, but make
    // the contract explicit: null data + zero len is the canonical
    // zero-payload form (heartbeat / login-request without body).
    SoupBinTcpFramer framer;
    std::array<uint8_t, 8> wire{};
    EXPECT_EQ(framer.encode(wire.data(), nullptr, 0, kServerHeartbeat),
              3u);
    EXPECT_EQ(wire[0], 0x00);
    EXPECT_EQ(wire[1], 0x01);
    EXPECT_EQ(wire[2], 'H');
}

TEST(SoupBinTcpFramer, encode_at_max_payload_len_succeeds) {
    // Pin the strict `>` boundary at line 92: len == kMaxPayloadLen
    // (65534) must succeed.
    SoupBinTcpFramer framer;
    std::vector<uint8_t> payload(65534, 0x5A);
    std::vector<uint8_t> wire(65534 + 3);
    EXPECT_EQ(framer.encode(wire.data(), payload.data(),
                             payload.size(), kSequencedData),
              65534u + 3u);
    // Wire-length field must be 65535 (payload + 1 type byte).
    EXPECT_EQ(wire[0], 0xFF);
    EXPECT_EQ(wire[1], 0xFF);
    EXPECT_EQ(wire[2], 'S');
}
