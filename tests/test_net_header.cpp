/// @file test_net_header.cpp
/// Unit tests for network header utilities: byte order, checksum, IPv4 parse/format.
/// Does NOT require DPDK EAL — tests only pure-logic functions.

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "eph/dpdk/net_header.hpp"

using namespace eph::dpdk::net;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time validation
// ─────────────────────────────────────────────────────────────────────────────

static_assert(kEtherHeaderLen == 14);
static_assert(kIpv4HeaderLen == 20);
static_assert(kTcpHeaderLen == 20);
static_assert(kAllHeadersLen == 54);
static_assert(kDefaultMss == 1460);

// ─────────────────────────────────────────────────────────────────────────────
// Byte order helpers
// ─────────────────────────────────────────────────────────────────────────────

TEST(NetHeader, Hton16Roundtrip) {
    for (uint16_t v : {uint16_t{0}, uint16_t{1}, uint16_t{0x0100},
                       uint16_t{0x1234}, uint16_t{0xFFFF}}) {
        EXPECT_EQ(ntoh16(hton16(v)), v) << "v=" << v;
    }
}

TEST(NetHeader, Hton32Roundtrip) {
    for (uint32_t v : {0u, 1u, 0x12345678u, 0xDEADBEEFu, 0xFFFFFFFFu}) {
        EXPECT_EQ(ntoh32(hton32(v)), v) << "v=" << v;
    }
}

TEST(NetHeader, Hton16Constexpr) {
    constexpr uint16_t net = hton16(0x0800);
    constexpr uint16_t host = ntoh16(net);
    static_assert(host == 0x0800);
    EXPECT_EQ(host, 0x0800);
}

TEST(NetHeader, Hton32Constexpr) {
    constexpr uint32_t net = hton32(0xC0A80001);
    constexpr uint32_t host = ntoh32(net);
    static_assert(host == 0xC0A80001);
    EXPECT_EQ(host, 0xC0A80001u);
}

// ─────────────────────────────────────────────────────────────────────────────
// IPv4 parse / format
// ─────────────────────────────────────────────────────────────────────────────

TEST(NetHeader, ParseIpv4Valid) {
    EXPECT_EQ(parse_ipv4("10.0.0.1"),       0x0A000001u);
    EXPECT_EQ(parse_ipv4("192.168.1.100"),   0xC0A80164u);
    EXPECT_EQ(parse_ipv4("0.0.0.0"),         0x00000000u);
    EXPECT_EQ(parse_ipv4("255.255.255.255"), 0xFFFFFFFFu);
    EXPECT_EQ(parse_ipv4("127.0.0.1"),       0x7F000001u);
}

TEST(NetHeader, ParseIpv4Invalid) {
    EXPECT_EQ(parse_ipv4(""), 0u);
    EXPECT_EQ(parse_ipv4("not_an_ip"), 0u);
    EXPECT_EQ(parse_ipv4("256.0.0.1"), 0u);
    EXPECT_EQ(parse_ipv4("1.2.3"), 0u);
}

TEST(NetHeader, FormatIpv4Roundtrip) {
    auto test = [](const char* ip_str) {
        uint32_t ip = parse_ipv4(ip_str);
        auto formatted = format_ipv4(ip);
        EXPECT_STREQ(formatted.data(), ip_str);
    };

    test("10.0.0.1");
    test("192.168.1.100");
    test("0.0.0.0");
    test("255.255.255.255");
    test("127.0.0.1");
}

// ─────────────────────────────────────────────────────────────────────────────
// Internet checksum (RFC 1071)
// ─────────────────────────────────────────────────────────────────────────────

TEST(NetHeader, ChecksumRfc1071Example) {
    // RFC 1071 example: 16-bit words 0x0001 0xF203 0xF4F5 0xF6F7
    // Expected checksum: 0x220D
    uint8_t data[] = {
        0x00, 0x01,
        0xF2, 0x03,
        0xF4, 0xF5,
        0xF6, 0xF7,
    };
    uint16_t cksum = internet_checksum(data, sizeof(data));
    EXPECT_EQ(cksum, hton16(0x220D));
}

TEST(NetHeader, ChecksumZeroLength) {
    // Checksum of empty data = ~0 = 0xFFFF
    uint16_t cksum = internet_checksum(nullptr, 0);
    EXPECT_EQ(cksum, 0xFFFFu);
}

TEST(NetHeader, ChecksumOddLength) {
    // Single byte 0x01 → sum = 0x0001 (byte padded with zero)
    // checksum = ~0x0001 = 0xFFFE (in network order)
    uint8_t data[] = {0x01};
    uint16_t cksum = internet_checksum(data, 1);
    EXPECT_EQ(cksum, static_cast<uint16_t>(~uint16_t{0x0001}));
}

TEST(NetHeader, ChecksumSelfVerifies) {
    // Computing checksum over data + its checksum should yield 0
    uint8_t data[] = {0x45, 0x00, 0x00, 0x3C, 0x1C, 0x46};
    uint16_t cksum = internet_checksum(data, sizeof(data));

    // Append checksum to data and recompute
    uint8_t with_cksum[sizeof(data) + 2];
    std::memcpy(with_cksum, data, sizeof(data));
    std::memcpy(with_cksum + sizeof(data), &cksum, 2);

    uint16_t verify = internet_checksum(with_cksum, sizeof(with_cksum));
    EXPECT_EQ(verify, 0u) << "Checksum over data+checksum should be 0";
}

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionTuple
// ─────────────────────────────────────────────────────────────────────────────

TEST(NetHeader, ConnectionTupleEquality) {
    ConnectionTuple a{.src_ip = 1, .dst_ip = 2, .src_port = 80, .dst_port = 443};
    ConnectionTuple b = a;
    EXPECT_EQ(a, b);

    ConnectionTuple c = a;
    c.dst_port = 8080;
    EXPECT_NE(a, c);
}

// ─────────────────────────────────────────────────────────────────────────────
// TCP checksum (with pseudo-header)
// ─────────────────────────────────────────────────────────────────────────────

TEST(NetHeader, TcpChecksumSelfVerifies) {
    // Build a minimal TCP header (20 bytes) with known data
    uint8_t tcp_hdr[20] = {};
    // src_port = 12345 (net order)
    uint16_t sp = hton16(12345);
    std::memcpy(tcp_hdr, &sp, 2);
    // dst_port = 80 (net order)
    uint16_t dp = hton16(80);
    std::memcpy(tcp_hdr + 2, &dp, 2);
    // data_off = 5 words = 20 bytes
    tcp_hdr[12] = (5 << 4);
    // flags = SYN
    tcp_hdr[13] = kTcpSyn;
    // window = 65535
    uint16_t win = hton16(65535);
    std::memcpy(tcp_hdr + 14, &win, 2);
    // checksum = 0 (will be computed)
    tcp_hdr[16] = 0;
    tcp_hdr[17] = 0;

    uint32_t src_ip_net = hton32(parse_ipv4("10.0.0.1"));
    uint32_t dst_ip_net = hton32(parse_ipv4("10.0.0.2"));

    uint16_t cksum = tcp_checksum(src_ip_net, dst_ip_net, tcp_hdr, 20);
    // Store checksum in header
    std::memcpy(tcp_hdr + 16, &cksum, 2);

    // Recompute — should be 0 (self-verification)
    // Need to re-add pseudo-header contribution
    uint32_t sum = pseudo_header_sum(src_ip_net, dst_ip_net, kIpProtoTcp, 20);
    auto ptr = tcp_hdr;
    for (int i = 0; i < 10; ++i) {
        uint16_t word;
        std::memcpy(&word, ptr + i * 2, 2);
        sum += word;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    EXPECT_EQ(static_cast<uint16_t>(~sum), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// PacketTemplate constants check
// ─────────────────────────────────────────────────────────────────────────────

TEST(NetHeader, PacketTemplateDefaults) {
    PacketTemplate tmpl{};
    EXPECT_EQ(tmpl.ip_id, 0);
    EXPECT_FALSE(tmpl.hw_cksum);
}
