/// @file test_packet_parse_adversarial.cpp
/// Adversarial / boundary-condition tests for packet_parse.hpp.
///
/// The pre-existing test_net_header.cpp covers happy-path parsing
/// (truncation, IHL=7, TCP options at data_off=8, ip_total > pkt_len).
/// This file targets the boundary cases NOT covered: IHL extremes,
/// TCP data_off extremes, ip_total internal-consistency violations,
/// and the UDP parser's analogous corner cases.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_tcp.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/packet_parse.hpp"

using namespace eph::dpdk::net;

namespace {

/// Build a minimal Ethernet/IPv4/TCP packet into `buf`.
/// Returns total bytes written.  Caller may corrupt fields after the call.
size_t build_tcp_packet(uint8_t* buf, uint16_t payload_len,
                         uint8_t ihl_words = 5, uint8_t tcp_doff_words = 5,
                         uint8_t proto = kIpProtoTcp) {
    size_t eth_len = kEtherHeaderLen;
    size_t ip_len  = ihl_words * 4u;
    size_t tcp_len = tcp_doff_words * 4u;
    size_t total   = eth_len + ip_len + tcp_len + payload_len;

    std::memset(buf, 0, total);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + eth_len);
    ip->version_ihl   = static_cast<uint8_t>((4 << 4) | ihl_words);
    ip->total_length  = hton16(static_cast<uint16_t>(ip_len + tcp_len + payload_len));
    ip->next_proto_id = proto;
    ip->src_addr      = hton32(0x0A000001);
    ip->dst_addr      = hton32(0x0A000002);

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + eth_len + ip_len);
    tcp->data_off  = static_cast<uint8_t>(tcp_doff_words << 4);
    tcp->src_port  = hton16(12345);
    tcp->dst_port  = hton16(443);
    tcp->tcp_flags = kTcpAck;

    return total;
}

/// Build a minimal Ethernet/IPv4/UDP packet into `buf`.
size_t build_udp_packet(uint8_t* buf, uint16_t payload_len,
                        uint8_t ihl_words = 5) {
    size_t eth_len = kEtherHeaderLen;
    size_t ip_len  = ihl_words * 4u;
    size_t udp_len = kUdpHeaderLen;
    size_t total   = eth_len + ip_len + udp_len + payload_len;

    std::memset(buf, 0, total);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + eth_len);
    ip->version_ihl   = static_cast<uint8_t>((4 << 4) | ihl_words);
    ip->total_length  = hton16(static_cast<uint16_t>(ip_len + udp_len + payload_len));
    ip->next_proto_id = kIpProtoUdp;
    ip->src_addr      = hton32(0x0A000001);
    ip->dst_addr      = hton32(0x0A000002);

    auto* udp = reinterpret_cast<UdpHeader*>(buf + eth_len + ip_len);
    udp->src_port = hton16(12345);
    udp->dst_port = hton16(53);
    udp->length   = hton16(static_cast<uint16_t>(udp_len + payload_len));
    udp->checksum = 0;

    return total;
}

/// Wrap a stack buffer in a fake rte_mbuf.
rte_mbuf make_mbuf(uint8_t* buf, size_t len) {
    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = static_cast<uint16_t>(len);
    mbuf.pkt_len  = static_cast<uint32_t>(len);
    return mbuf;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// IHL boundary tests
// ═══════════════════════════════════════════════════════════════════════

TEST(PacketParseAdv, IhlZeroRejected) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->version_ihl = (4 << 4) | 0;  // IHL=0
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

TEST(PacketParseAdv, IhlBelowMinimumRejected) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->version_ihl = (4 << 4) | 4;  // 16 bytes, below 20 minimum
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

TEST(PacketParseAdv, IhlMaximum15WithExtraOptions) {
    // IHL=15 → 60 bytes IP header.  Build a packet with the full
    // 60-byte IP header (40 bytes of options) + TCP + 4-byte payload.
    constexpr size_t pkt_size = kEtherHeaderLen + 60 + 20 + 4;
    uint8_t buf[pkt_size];
    std::memset(buf, 0, pkt_size);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->version_ihl   = (4 << 4) | 15;
    ip->total_length  = hton16(60 + 20 + 4);
    ip->next_proto_id = kIpProtoTcp;

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + kEtherHeaderLen + 60);
    tcp->data_off = 5 << 4;

    auto mbuf = make_mbuf(buf, pkt_size);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, 4);
    EXPECT_EQ(parsed.payload, buf + kEtherHeaderLen + 60 + 20);
}

TEST(PacketParseAdv, IpVersionNotV4Rejected) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->version_ihl = (6 << 4) | 5;  // IPv6 in version field
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
// TCP data_off boundary tests
// ═══════════════════════════════════════════════════════════════════════

TEST(PacketParseAdv, TcpDataOffZeroRejected) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + kEtherHeaderLen + 20);
    tcp->data_off = 0;
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

TEST(PacketParseAdv, TcpDataOffBelowMinimumRejected) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + kEtherHeaderLen + 20);
    tcp->data_off = 4 << 4;  // 16 bytes, below 20 minimum
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

TEST(PacketParseAdv, TcpDataOffMaximum15WithFullOptions) {
    // TCP data_off=15 → 60 bytes TCP header.  IP=20, TCP=60, payload=4.
    constexpr size_t pkt_size = kEtherHeaderLen + 20 + 60 + 4;
    uint8_t buf[pkt_size];
    std::memset(buf, 0, pkt_size);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->version_ihl   = (4 << 4) | 5;
    ip->total_length  = hton16(20 + 60 + 4);
    ip->next_proto_id = kIpProtoTcp;

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + kEtherHeaderLen + 20);
    tcp->data_off = 15 << 4;

    auto mbuf = make_mbuf(buf, pkt_size);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, 4);
    EXPECT_EQ(parsed.payload, buf + kEtherHeaderLen + 20 + 60);
}

// ═══════════════════════════════════════════════════════════════════════
// ip_total_length internal-consistency
// ═══════════════════════════════════════════════════════════════════════

TEST(PacketParseAdv, IpTotalLessThanIhlRejected) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->total_length = hton16(15);  // less than IHL=20 itself
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

TEST(PacketParseAdv, IpTotalEqualsIhlNoTcpRejected) {
    // ip_total = IHL → no room for any L4 header
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->total_length = hton16(20);  // exactly IHL
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

TEST(PacketParseAdv, IpTotalLessThanIhlPlusTcpRejected) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->total_length = hton16(35);  // IHL(20) + half of TCP header
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

TEST(PacketParseAdv, IpTotalExactlyIhlPlusTcpZeroPayload) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 0);  // already 0 payload
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, 0);
}

// ═══════════════════════════════════════════════════════════════════════
// Mbuf-level edge cases
// ═══════════════════════════════════════════════════════════════════════

TEST(PacketParseAdv, NullMbufReturnsEmpty) {
    auto parsed = parse_packet(nullptr);
    EXPECT_EQ(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.ip, nullptr);
}

TEST(PacketParseAdv, ZeroLengthMbufRejected) {
    uint8_t buf[128] = {};
    auto mbuf = make_mbuf(buf, 0);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

TEST(PacketParseAdv, EthernetOnlyMbufRejected) {
    uint8_t buf[128];
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);
    auto mbuf = make_mbuf(buf, kEtherHeaderLen);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
// UDP parser boundary tests (parallel to TCP cases above)
// ═══════════════════════════════════════════════════════════════════════

TEST(PacketParseAdv, UdpHappyPath) {
    uint8_t buf[128];
    size_t len = build_udp_packet(buf, 16);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_udp_packet(&mbuf);
    ASSERT_NE(parsed.udp, nullptr);
    EXPECT_EQ(parsed.payload_len, 16);
}

TEST(PacketParseAdv, UdpLengthBelowHeaderRejected) {
    uint8_t buf[128];
    size_t len = build_udp_packet(buf, 16);
    auto* udp = reinterpret_cast<UdpHeader*>(buf + kEtherHeaderLen + 20);
    udp->length = hton16(7);  // less than 8-byte UDP header
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_udp_packet(&mbuf).udp, nullptr);
}

TEST(PacketParseAdv, UdpLengthExceedsBufferRejected) {
    uint8_t buf[128];
    size_t len = build_udp_packet(buf, 16);
    auto* udp = reinterpret_cast<UdpHeader*>(buf + kEtherHeaderLen + 20);
    udp->length = hton16(2000);  // way beyond buffer
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_udp_packet(&mbuf).udp, nullptr);
}

TEST(PacketParseAdv, UdpZeroPayloadAccepted) {
    uint8_t buf[128];
    size_t len = build_udp_packet(buf, 0);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_udp_packet(&mbuf);
    ASSERT_NE(parsed.udp, nullptr);
    EXPECT_EQ(parsed.payload_len, 0);
    // payload pointer must be non-null even for zero-length datagrams
    // (points past the UDP header), so callers can distinguish "zero-len
    // payload" from "parse failed" via the udp field alone.
    EXPECT_NE(parsed.payload, nullptr);
}

TEST(PacketParseAdv, UdpWithIpOptionsAccepted) {
    // IHL=7 (28-byte IP header) + UDP + 4-byte payload
    uint8_t buf[128];
    size_t len = build_udp_packet(buf, 4, /*ihl_words=*/7);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_udp_packet(&mbuf);
    ASSERT_NE(parsed.udp, nullptr);
    EXPECT_EQ(parsed.payload_len, 4);
}

TEST(PacketParseAdv, UdpProtocolMismatchRejected) {
    // Build a TCP packet but call parse_udp_packet on it.
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_udp_packet(&mbuf).udp, nullptr);
}

TEST(PacketParseAdv, TcpProtocolMismatchRejected) {
    // Build a UDP packet but call parse_packet (TCP) on it.
    uint8_t buf[128];
    size_t len = build_udp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
// parse_ip_header — layered API
// ═══════════════════════════════════════════════════════════════════════

TEST(PacketParseAdv, ParseIpHeaderPicksUpProtocolForUdp) {
    uint8_t buf[128];
    size_t len = build_udp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    auto ip = parse_ip_header(&mbuf);
    ASSERT_TRUE(ip);
    EXPECT_EQ(ip.proto, kIpProtoUdp);
    EXPECT_EQ(ip.ihl, 20);
}

TEST(PacketParseAdv, ParseIpHeaderPicksUpProtocolForTcp) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    auto ip = parse_ip_header(&mbuf);
    ASSERT_TRUE(ip);
    EXPECT_EQ(ip.proto, kIpProtoTcp);
}

TEST(PacketParseAdv, ParseTcpFromIpRejectsUdpHeader) {
    uint8_t buf[128];
    size_t len = build_udp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    auto ip = parse_ip_header(&mbuf);
    ASSERT_TRUE(ip);
    auto tcp = parse_tcp_from_ip(&mbuf, ip);
    EXPECT_EQ(tcp.tcp, nullptr);
}

TEST(PacketParseAdv, ParseUdpFromIpRejectsTcpHeader) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    auto ip = parse_ip_header(&mbuf);
    ASSERT_TRUE(ip);
    auto udp = parse_udp_from_ip(&mbuf, ip);
    EXPECT_EQ(udp.udp, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
// 4-tuple matching
// ═══════════════════════════════════════════════════════════════════════

TEST(PacketParseAdv, MatchesExactTuple) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);

    // The packet was built with src=0x0A000001:12345, dst=0x0A000002:443.
    // matches() takes a tuple where src/dst are LOCAL view, so for an
    // RX packet from "remote 0x0A000001:12345 → local 0x0A000002:443",
    // the local tuple is dst=0x0A000001:12345, src=0x0A000002:443.
    ConnectionTuple local{
        /*src_ip=*/   0x0A000002,
        /*dst_ip=*/   0x0A000001,
        /*src_port=*/ 443,
        /*dst_port=*/ 12345,
    };
    EXPECT_TRUE(parsed.matches(local));
}

TEST(PacketParseAdv, MatchesRejectsWrongPort) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);

    ConnectionTuple wrong{0x0A000002, 0x0A000001, 444, 12345};
    EXPECT_FALSE(parsed.matches(wrong));
}

TEST(PacketParseAdv, MatchesRejectsWrongIp) {
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);

    ConnectionTuple wrong{0x0A000099, 0x0A000001, 443, 12345};
    EXPECT_FALSE(parsed.matches(wrong));
}
