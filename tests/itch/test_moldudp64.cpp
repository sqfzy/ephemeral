#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "eph/itch/moldudp64.hpp"
#include "eph/itch/parser.hpp"
#include "eph/itch/messages.hpp"

using eph::itch::MoldUDP64Header;
using eph::itch::ParseError;
using eph::itch::moldudp64::kEndOfSession;
using eph::itch::moldudp64::kHeaderLen;
using eph::itch::moldudp64::kSessionLen;

// ---------------------------------------------------------------------------
// Helper: build a MoldUDP64 packet from parts
// ---------------------------------------------------------------------------

/// Write a big-endian uint16_t at dst.
static void write_be16(uint8_t* dst, uint16_t val) {
    dst[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
    dst[1] = static_cast<uint8_t>(val & 0xFF);
}

/// Write a big-endian uint64_t at dst.
static void write_be64(uint8_t* dst, uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        dst[i] = static_cast<uint8_t>(val & 0xFF);
        val >>= 8;
    }
}

/// Build a MoldUDP64 header (20 bytes) into buf.
static void build_header(std::vector<uint8_t>& buf,
                          const char* session, uint64_t seq, uint16_t count) {
    // Session ID: 10 bytes, right-padded with spaces
    size_t slen = std::strlen(session);
    for (size_t i = 0; i < kSessionLen; ++i) {
        buf.push_back(i < slen ? static_cast<uint8_t>(session[i]) : ' ');
    }
    // Sequence number: 8 bytes BE
    size_t seq_off = buf.size();
    buf.resize(buf.size() + 8);
    write_be64(buf.data() + seq_off, seq);
    // Message count: 2 bytes BE
    size_t cnt_off = buf.size();
    buf.resize(buf.size() + 2);
    write_be16(buf.data() + cnt_off, count);
}

/// Append one length-prefixed message to the packet.
static void append_message(std::vector<uint8_t>& buf,
                           const uint8_t* data, uint16_t len) {
    size_t off = buf.size();
    buf.resize(buf.size() + 2 + len);
    write_be16(buf.data() + off, len);
    if (len > 0) {
        std::memcpy(buf.data() + off + 2, data, len);
    }
}

// ---------------------------------------------------------------------------
// Header parsing tests
// ---------------------------------------------------------------------------

TEST(MoldUDP64, parse_header_valid) {
    std::vector<uint8_t> pkt;
    build_header(pkt, "SESS123", 42, 3);

    auto result = eph::itch::parse_moldudp64_header(pkt.data(), pkt.size());
    ASSERT_TRUE(result.has_value());
    // Session is 10 bytes, right-padded
    EXPECT_EQ(result->session, "SESS123   ");
    EXPECT_EQ(result->sequence_number, 42u);
    EXPECT_EQ(result->message_count, 3u);
}

TEST(MoldUDP64, parse_header_truncated) {
    // Only 15 bytes — not enough for the 20-byte header
    std::array<uint8_t, 15> buf{};
    auto result = eph::itch::parse_moldudp64_header(buf.data(), buf.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kTruncated);
}

TEST(MoldUDP64, parse_header_zero_length) {
    auto result = eph::itch::parse_moldudp64_header(nullptr, 0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ParseError::kTruncated);
}

TEST(MoldUDP64, parse_header_end_of_session) {
    std::vector<uint8_t> pkt;
    build_header(pkt, "ENDSESS", 1000, kEndOfSession);

    auto result = eph::itch::parse_moldudp64_header(pkt.data(), pkt.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->message_count, kEndOfSession);
}

// ---------------------------------------------------------------------------
// Message iteration tests
// ---------------------------------------------------------------------------

TEST(MoldUDP64, parse_zero_messages) {
    std::vector<uint8_t> pkt;
    build_header(pkt, "ZERO", 1, 0);

    size_t count = 0;
    auto delivered = eph::itch::parse_moldudp64(pkt.data(), pkt.size(),
        [&](const uint8_t*, uint16_t, uint64_t) { ++count; });

    EXPECT_EQ(delivered, 0u);
    EXPECT_EQ(count, 0u);
}

TEST(MoldUDP64, parse_one_message) {
    // Build a packet with one 5-byte message payload
    std::vector<uint8_t> pkt;
    build_header(pkt, "ONEMSG", 100, 1);

    const std::array<uint8_t, 5> msg_data = {'H', 'E', 'L', 'L', 'O'};
    append_message(pkt, msg_data.data(), 5);

    size_t count = 0;
    const uint8_t* received_data = nullptr;
    uint16_t received_len = 0;
    uint64_t received_seq = 0;

    auto delivered = eph::itch::parse_moldudp64(pkt.data(), pkt.size(),
        [&](const uint8_t* data, uint16_t len, uint64_t seq) {
            received_data = data;
            received_len = len;
            received_seq = seq;
            ++count;
        });

    EXPECT_EQ(delivered, 1u);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(received_len, 5u);
    EXPECT_EQ(received_seq, 100u);
    EXPECT_EQ(std::memcmp(received_data, "HELLO", 5), 0);
}

TEST(MoldUDP64, parse_three_messages) {
    std::vector<uint8_t> pkt;
    build_header(pkt, "MULTI", 500, 3);

    const std::array<uint8_t, 3> m1 = {'A', 'B', 'C'};
    const std::array<uint8_t, 2> m2 = {'D', 'E'};
    const std::array<uint8_t, 4> m3 = {'F', 'G', 'H', 'I'};

    append_message(pkt, m1.data(), 3);
    append_message(pkt, m2.data(), 2);
    append_message(pkt, m3.data(), 4);

    std::vector<uint16_t> lengths;
    std::vector<uint64_t> seqs;

    auto delivered = eph::itch::parse_moldudp64(pkt.data(), pkt.size(),
        [&](const uint8_t*, uint16_t len, uint64_t seq) {
            lengths.push_back(len);
            seqs.push_back(seq);
        });

    EXPECT_EQ(delivered, 3u);
    ASSERT_EQ(lengths.size(), 3u);
    EXPECT_EQ(lengths[0], 3u);
    EXPECT_EQ(lengths[1], 2u);
    EXPECT_EQ(lengths[2], 4u);
    ASSERT_EQ(seqs.size(), 3u);
    EXPECT_EQ(seqs[0], 500u);
    EXPECT_EQ(seqs[1], 501u);
    EXPECT_EQ(seqs[2], 502u);
}

TEST(MoldUDP64, end_of_session_delivers_no_messages) {
    std::vector<uint8_t> pkt;
    build_header(pkt, "ENDSESS", 9999, kEndOfSession);

    size_t count = 0;
    auto delivered = eph::itch::parse_moldudp64(pkt.data(), pkt.size(),
        [&](const uint8_t*, uint16_t, uint64_t) { ++count; });

    EXPECT_EQ(delivered, 0u);
    EXPECT_EQ(count, 0u);
}

TEST(MoldUDP64, truncated_header_returns_zero) {
    std::array<uint8_t, 10> buf{};
    size_t count = 0;
    auto delivered = eph::itch::parse_moldudp64(buf.data(), buf.size(),
        [&](const uint8_t*, uint16_t, uint64_t) { ++count; });

    EXPECT_EQ(delivered, 0u);
    EXPECT_EQ(count, 0u);
}

TEST(MoldUDP64, truncated_message_length_prefix) {
    // Header says 2 messages, but only 1 message + partial length prefix
    std::vector<uint8_t> pkt;
    build_header(pkt, "TRUNC", 1, 2);

    const std::array<uint8_t, 3> m1 = {'X', 'Y', 'Z'};
    append_message(pkt, m1.data(), 3);
    // Only add 1 byte of the second message's length prefix
    pkt.push_back(0x00);

    size_t count = 0;
    auto delivered = eph::itch::parse_moldudp64(pkt.data(), pkt.size(),
        [&](const uint8_t*, uint16_t, uint64_t) { ++count; });

    // Should deliver the first message but stop at the truncated second
    EXPECT_EQ(delivered, 1u);
    EXPECT_EQ(count, 1u);
}

TEST(MoldUDP64, truncated_message_body) {
    // Header says 1 message of length 10, but only 5 bytes follow the prefix
    std::vector<uint8_t> pkt;
    build_header(pkt, "TRUNCBODY", 1, 1);

    // Write length prefix manually (10 bytes)
    size_t off = pkt.size();
    pkt.resize(pkt.size() + 2 + 5);  // Only 5 bytes of body, not 10
    write_be16(pkt.data() + off, 10);
    std::memset(pkt.data() + off + 2, 'A', 5);

    size_t count = 0;
    auto delivered = eph::itch::parse_moldudp64(pkt.data(), pkt.size(),
        [&](const uint8_t*, uint16_t, uint64_t) { ++count; });

    EXPECT_EQ(delivered, 0u);
    EXPECT_EQ(count, 0u);
}

TEST(MoldUDP64, sequence_number_incrementing) {
    std::vector<uint8_t> pkt;
    const uint64_t base_seq = 1'000'000;
    const uint16_t num_msgs = 5;
    build_header(pkt, "SEQTEST", base_seq, num_msgs);

    for (uint16_t i = 0; i < num_msgs; ++i) {
        uint8_t payload = static_cast<uint8_t>('A' + i);
        append_message(pkt, &payload, 1);
    }

    std::vector<uint64_t> seqs;
    auto delivered = eph::itch::parse_moldudp64(pkt.data(), pkt.size(),
        [&](const uint8_t*, uint16_t, uint64_t seq) {
            seqs.push_back(seq);
        });

    EXPECT_EQ(delivered, 5u);
    ASSERT_EQ(seqs.size(), 5u);
    for (uint16_t i = 0; i < num_msgs; ++i) {
        EXPECT_EQ(seqs[i], base_seq + i);
    }
}

// ---------------------------------------------------------------------------
// Integration: MoldUDP64 -> ITCH parse -> verify fields
// ---------------------------------------------------------------------------

TEST(MoldUDP64, integration_itch_system_event) {
    // Build a SystemEvent ITCH message (type 'S', 12 bytes total):
    //   byte 0: 'S' (type tag)
    //   bytes 1-2: stock_locate (uint16 BE)
    //   bytes 3-4: tracking_number (uint16 BE)
    //   bytes 5-10: timestamp (6-byte BE nanoseconds)
    //   byte 11: event_code
    std::array<uint8_t, 12> itch_msg{};
    itch_msg[0] = 'S';  // SystemEvent type tag
    // stock_locate = 0
    write_be16(itch_msg.data() + 1, 0);
    // tracking_number = 0
    write_be16(itch_msg.data() + 3, 0);
    // timestamp: 6 bytes at offset 5 — set to a known value
    // Use a small value that fits in 6 bytes
    const uint64_t ts_ns = 34'200'000'000'000ULL;  // 09:30:00 in nanoseconds
    // Write 6-byte big-endian timestamp
    for (int i = 5; i >= 0; --i) {
        itch_msg[5 + (5 - i)] = static_cast<uint8_t>((ts_ns >> (i * 8)) & 0xFF);
    }
    // event_code = 'O' (start of messages)
    itch_msg[11] = 'O';

    // Wrap in a MoldUDP64 packet
    std::vector<uint8_t> pkt;
    build_header(pkt, "ITCH50", 1, 1);
    append_message(pkt, itch_msg.data(), static_cast<uint16_t>(itch_msg.size()));

    // Parse MoldUDP64 and then parse ITCH
    bool itch_parsed = false;
    auto delivered = eph::itch::parse_moldudp64(pkt.data(), pkt.size(),
        [&](const uint8_t* msg_data, uint16_t msg_len, uint64_t seq) {
            EXPECT_EQ(seq, 1u);
            EXPECT_EQ(msg_len, 12u);

            // Parse the contained ITCH message
            auto result = eph::itch::parse(msg_data, msg_len);
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(result->msg_type, eph::itch::kSystemEvent);
            EXPECT_EQ(result->length, 12u);

            // Verify the event code via the raw message pointer
            // SystemEvent body: type(1) + stock_locate(2) + tracking(2) + ts(6) + event(1)
            EXPECT_EQ(msg_data[11], 'O');

            itch_parsed = true;
        });

    EXPECT_EQ(delivered, 1u);
    EXPECT_TRUE(itch_parsed);
}

TEST(MoldUDP64, integration_multiple_itch_messages) {
    // Two SystemEvent messages in a single MoldUDP64 packet
    std::array<uint8_t, 12> msg1{};
    msg1[0] = 'S';
    msg1[11] = 'O';  // Start of Messages

    std::array<uint8_t, 12> msg2{};
    msg2[0] = 'S';
    msg2[11] = 'C';  // End of Messages

    std::vector<uint8_t> pkt;
    build_header(pkt, "MULTI", 100, 2);
    append_message(pkt, msg1.data(), 12);
    append_message(pkt, msg2.data(), 12);

    std::vector<uint8_t> event_codes;
    auto delivered = eph::itch::parse_moldudp64(pkt.data(), pkt.size(),
        [&](const uint8_t* msg_data, uint16_t msg_len, uint64_t) {
            auto result = eph::itch::parse(msg_data, msg_len);
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(result->msg_type, eph::itch::kSystemEvent);
            event_codes.push_back(msg_data[11]);
        });

    EXPECT_EQ(delivered, 2u);
    ASSERT_EQ(event_codes.size(), 2u);
    EXPECT_EQ(event_codes[0], 'O');
    EXPECT_EQ(event_codes[1], 'C');
}
