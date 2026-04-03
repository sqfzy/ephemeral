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

TEST(NetHeader, ParseIpv4NullPointerReturnsZero) {
    EXPECT_EQ(parse_ipv4(nullptr), 0u);
}

TEST(NetHeader, ParseIpv4TrailingCharsRejected) {
    EXPECT_EQ(parse_ipv4("10.0.0.1 "), 0u);
    EXPECT_EQ(parse_ipv4("10.0.0.1x"), 0u);
    EXPECT_EQ(parse_ipv4("10.0.0.1."), 0u);
}

TEST(NetHeader, ParseIpv4TooManyOctets) {
    EXPECT_EQ(parse_ipv4("10.0.0.1.5"), 0u);
    EXPECT_EQ(parse_ipv4("1.2.3.4.5.6"), 0u);
}

TEST(NetHeader, ParseIpv4EmptyOctets) {
    EXPECT_EQ(parse_ipv4(".0.0.1"), 0u);
    EXPECT_EQ(parse_ipv4("10..0.1"), 0u);
    EXPECT_EQ(parse_ipv4("10.0.0."), 0u);
}

TEST(NetHeader, ParseIpv4LeadingZerosAccepted) {
    // Leading zeros are accepted as decimal (not octal)
    EXPECT_EQ(parse_ipv4("010.001.001.001"), 0x0A010101u);
}

TEST(NetHeader, ParseIpv4FourDigitOctetRejected) {
    // Octets > 3 digits are rejected (parser reads max 3 digits)
    EXPECT_EQ(parse_ipv4("1000.0.0.1"), 0u);
}

TEST(NetHeader, ParseIpv4SingleOctet) {
    EXPECT_EQ(parse_ipv4("1"), 0u);  // Missing 3 octets
    EXPECT_EQ(parse_ipv4("1.2"), 0u); // Missing 2 octets
}

TEST(NetHeader, FormatIpv4Direct) {
    EXPECT_STREQ(format_ipv4(0x00000000u).data(), "0.0.0.0");
    EXPECT_STREQ(format_ipv4(0xFFFFFFFFu).data(), "255.255.255.255");
    EXPECT_STREQ(format_ipv4(0x80000000u).data(), "128.0.0.0");
    EXPECT_STREQ(format_ipv4(0x0A000001u).data(), "10.0.0.1");
    EXPECT_STREQ(format_ipv4(0x01020304u).data(), "1.2.3.4");
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

// ─────────────────────────────────────────────────────────────────────────────
// parse_packet boundary tests (simulated mbuf via raw buffer)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Build a minimal valid Eth+IP+TCP packet into a buffer.
/// Returns total bytes written. Payload is zero-filled.
size_t build_raw_packet(uint8_t* buf, uint16_t payload_len,
                         uint8_t ihl_words = 5, uint8_t tcp_doff_words = 5) {
    size_t eth_len = kEtherHeaderLen;
    size_t ip_len  = ihl_words * 4u;
    size_t tcp_len = tcp_doff_words * 4u;
    size_t total   = eth_len + ip_len + tcp_len + payload_len;

    std::memset(buf, 0, total);

    // Ethernet
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);

    // IP
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + eth_len);
    ip->version_ihl  = static_cast<uint8_t>((4 << 4) | ihl_words);
    ip->total_length  = hton16(static_cast<uint16_t>(ip_len + tcp_len + payload_len));
    ip->next_proto_id = kIpProtoTcp;
    ip->src_addr      = hton32(0x0A000001);
    ip->dst_addr      = hton32(0x0A000002);

    // TCP
    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + eth_len + ip_len);
    tcp->data_off  = static_cast<uint8_t>(tcp_doff_words << 4);
    tcp->src_port  = hton16(12345);
    tcp->dst_port  = hton16(443);
    tcp->tcp_flags = kTcpAck;

    return total;
}

} // anonymous namespace

TEST(ParsePacket, ZeroPayload) {
    uint8_t buf[128];
    size_t pkt_len = build_raw_packet(buf, 0);
    // Simulate rte_mbuf via simple struct
    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = static_cast<uint16_t>(pkt_len);
    mbuf.pkt_len  = static_cast<uint32_t>(pkt_len);

    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, 0);
    EXPECT_EQ(parsed.payload, nullptr);
}

TEST(ParsePacket, ExactOneBytePayload) {
    uint8_t buf[128];
    size_t pkt_len = build_raw_packet(buf, 1);
    buf[kAllHeadersLen] = 0xAB; // payload byte

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = static_cast<uint16_t>(pkt_len);
    mbuf.pkt_len  = static_cast<uint32_t>(pkt_len);

    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, 1);
    ASSERT_NE(parsed.payload, nullptr);
    EXPECT_EQ(parsed.payload[0], 0xAB);
}

TEST(ParsePacket, EthernetPaddingIgnored) {
    // Build a packet with 6 bytes payload, but pad to 64-byte Ethernet minimum
    uint8_t buf[128];
    size_t real_len = build_raw_packet(buf, 6);
    // Pad to 64 bytes (Ethernet minimum)
    std::memset(buf + real_len, 0xCC, 64 - real_len);
    uint16_t padded_len = 64;

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = padded_len;
    mbuf.pkt_len  = padded_len;

    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    // Must use IP total_length (6), NOT pkt_len - headers (10)
    EXPECT_EQ(parsed.payload_len, 6)
        << "Ethernet padding bytes should not be included in TCP payload";
}

TEST(ParsePacket, IpTotalExceedsPktLen_Rejected) {
    uint8_t buf[128];
    size_t pkt_len = build_raw_packet(buf, 10);

    // Corrupt: set ip_total_length to something > pkt_len
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->total_length = hton16(2000);

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = static_cast<uint16_t>(pkt_len);
    mbuf.pkt_len  = static_cast<uint32_t>(pkt_len);

    auto parsed = parse_packet(&mbuf);
    // Should reject: ip_total > pkt_len guard
    EXPECT_EQ(parsed.tcp, nullptr)
        << "Packet with ip_total > pkt_len should be rejected";
}

TEST(ParsePacket, TruncatedPacket_TooShort) {
    uint8_t buf[32] = {};
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = 30; // Less than kAllHeadersLen=54
    mbuf.pkt_len  = 30;

    auto parsed = parse_packet(&mbuf);
    EXPECT_EQ(parsed.tcp, nullptr);
}

TEST(ParsePacket, NonIpv4Rejected) {
    uint8_t buf[128];
    build_raw_packet(buf, 0);
    // Change ether_type to IPv6
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(0x86DD);

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = 54;
    mbuf.pkt_len  = 54;

    auto parsed = parse_packet(&mbuf);
    EXPECT_EQ(parsed.tcp, nullptr);
}

TEST(ParsePacket, IpWithOptions_IHL7) {
    // IPv4 with options: IHL=7 → 28-byte IP header
    uint8_t buf[128];
    size_t pkt_len = build_raw_packet(buf, 4, /*ihl_words=*/7);

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = static_cast<uint16_t>(pkt_len);
    mbuf.pkt_len  = static_cast<uint32_t>(pkt_len);

    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, 4);
    // TCP header starts at Eth(14) + IP(28) = 42, not 34
    auto* expected_tcp = reinterpret_cast<const rte_tcp_hdr*>(buf + 14 + 28);
    EXPECT_EQ(parsed.tcp, expected_tcp);
}

TEST(ParsePacket, TcpWithOptions_DataOff8) {
    // TCP with options: data_off=8 → 32-byte TCP header
    uint8_t buf[128];
    size_t pkt_len = build_raw_packet(buf, 2, /*ihl_words=*/5, /*tcp_doff=*/8);

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = static_cast<uint16_t>(pkt_len);
    mbuf.pkt_len  = static_cast<uint32_t>(pkt_len);

    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, 2);
    // Payload starts at Eth(14) + IP(20) + TCP(32) = 66
    EXPECT_EQ(parsed.payload, buf + 66);
}

TEST(ParsePacket, NonTcpProtocol_Rejected) {
    uint8_t buf[128];
    build_raw_packet(buf, 0);
    // Change protocol to UDP
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->next_proto_id = 17; // UDP

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = 54;
    mbuf.pkt_len  = 54;

    auto parsed = parse_packet(&mbuf);
    EXPECT_EQ(parsed.tcp, nullptr);
}

TEST(ParsePacket, MaxPayload_FullMSS) {
    uint8_t buf[2048];
    size_t pkt_len = build_raw_packet(buf, 1460); // Full MSS

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = static_cast<uint16_t>(pkt_len);
    mbuf.pkt_len  = static_cast<uint32_t>(pkt_len);

    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, 1460);
}

// ─────────────────────────────────────────────────────────────────────────────
// TCP SYN options
// ─────────────────────────────────────────────────────────────────────────────

TEST(SynOptions, WriteSynOptionsContainsMSS) {
    uint8_t buf[kSynOptionsLen]{};
    uint16_t len = write_syn_options(buf, 1460);

    EXPECT_EQ(len, kSynOptionsLen);

    // MSS option: Kind=2, Length=4, Value=1460 (network order)
    EXPECT_EQ(buf[0], 2);  // Kind: MSS
    EXPECT_EQ(buf[1], 4);  // Length
    uint16_t mss_net;
    std::memcpy(&mss_net, &buf[2], 2);
    EXPECT_EQ(ntoh16(mss_net), 1460);

    // SACK Permitted: Kind=4, Length=2
    EXPECT_EQ(buf[4], 4);
    EXPECT_EQ(buf[5], 2);

    // Window Scale: Kind=3, Length=3
    EXPECT_EQ(buf[7], 3);
    EXPECT_EQ(buf[8], 3);
    EXPECT_EQ(buf[9], 0);  // Shift=0
}

TEST(SynOptions, BuildPacketSynIncludesOptions) {
    // Create a fake mempool-like mbuf for testing.
    // build_packet needs a real rte_mempool; we test via write_syn_options
    // and verify the option layout is correct with a known MSS value.
    uint8_t buf[kSynOptionsLen]{};
    write_syn_options(buf, 536);  // Minimum MSS

    uint16_t mss_net;
    std::memcpy(&mss_net, &buf[2], 2);
    EXPECT_EQ(ntoh16(mss_net), 536);
}

TEST(SynOptions, ConstantsAreConsistent) {
    EXPECT_EQ(kSynOptionsLen, 12u);
    EXPECT_EQ(kSynTcpHeaderLen, 32u);
    // SYN header is 8 32-bit words (must be 4-byte aligned)
    EXPECT_EQ(kSynTcpHeaderLen % 4, 0u);
}

TEST(ParsePacket, MatchesSwapsAddresses) {
    uint8_t buf[128];
    build_raw_packet(buf, 0);

    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = 54;
    mbuf.pkt_len  = 54;

    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);

    // Packet src=10.0.0.1:12345, dst=10.0.0.2:443
    // Our tuple: src=10.0.0.2:443, dst=10.0.0.1:12345 (swapped)
    ConnectionTuple our_tuple{
        .src_ip = 0x0A000002, .dst_ip = 0x0A000001,
        .src_port = 443, .dst_port = 12345};
    EXPECT_TRUE(parsed.matches(our_tuple));

    // Non-matching tuple
    ConnectionTuple wrong{
        .src_ip = 0x0A000001, .dst_ip = 0x0A000002,
        .src_port = 12345, .dst_port = 443};
    EXPECT_FALSE(parsed.matches(wrong));
}

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionTuple dump/to_json
// ─────────────────────────────────────────────────────────────────────────────

TEST(ConnectionTuple, DumpShowsIpsAndPorts) {
    ConnectionTuple t{
        .src_ip = 0x0A000002, .dst_ip = 0x0A000001,
        .src_port = 443, .dst_port = 12345};
    auto d = t.dump();
    EXPECT_NE(d.find("10.0.0.2"), std::string::npos);
    EXPECT_NE(d.find("443"), std::string::npos);
    EXPECT_NE(d.find("10.0.0.1"), std::string::npos);
    EXPECT_NE(d.find("12345"), std::string::npos);
    EXPECT_NE(d.find("->"), std::string::npos);
}

TEST(ConnectionTuple, ToJsonFormat) {
    ConnectionTuple t{
        .src_ip = 0xC0A80101, .dst_ip = 0x08080808,
        .src_port = 5000, .dst_port = 443};
    auto j = t.to_json();
    EXPECT_NE(j.find("\"src_ip\":\"192.168.1.1\""), std::string::npos);
    EXPECT_NE(j.find("\"dst_ip\":\"8.8.8.8\""), std::string::npos);
    EXPECT_NE(j.find("\"src_port\":5000"), std::string::npos);
    EXPECT_NE(j.find("\"dst_port\":443"), std::string::npos);
}

TEST(ConnectionTuple, DumpZeroTuple) {
    ConnectionTuple t{};
    auto d = t.dump();
    EXPECT_NE(d.find("0.0.0.0"), std::string::npos);
    EXPECT_NE(d.find(":0"), std::string::npos);
}
