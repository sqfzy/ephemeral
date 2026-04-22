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

// ─────────────────────────────────────────────────────────────────────────────
// ICMP parse — Type 3 Code 4 (Fragmentation Needed and DF Set) path + common
// boundary cases. The full path is already exercised in test_tcp_state_machine
// via a real TcpSession fixture; these tests isolate parse_icmp() itself and
// target edge cases the state-machine tests don't cover.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Build a minimal ICMP Type 3 Code 4 packet with an embedded IP/TCP header.
/// Parameters:
///   - `next_hop_mtu`: MTU value to encode (host byte order; written BE).
///   - `embedded_proto`: protocol in the embedded IP header (kIpProtoTcp / Udp).
///   - `embedded_ihl_words`: IHL for the embedded IP header.
///   - `truncate_embedded_to`: if non-zero, shrink the total to exactly this
///     many bytes (simulates a wire-truncated ICMP).
size_t build_icmp_frag_needed(uint8_t* buf,
                               uint16_t next_hop_mtu = 1280,
                               uint8_t  embedded_proto = kIpProtoTcp,
                               uint8_t  embedded_ihl_words = 5,
                               size_t   truncate_embedded_to = 0) {
    const size_t eth_len = kEtherHeaderLen;
    const size_t outer_ip_len = kIpv4HeaderLen;
    const size_t icmp_hdr_len = 8;  // type/code/cksum/unused/mtu
    const size_t emb_ip_len = embedded_ihl_words * 4u;
    const size_t emb_l4_len = 8;    // ports + 4 bytes of TCP seq / UDP len+cksum
    const size_t total = eth_len + outer_ip_len + icmp_hdr_len
                        + emb_ip_len + emb_l4_len;

    std::memset(buf, 0, total);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    eth->ether_type = hton16(kEtherTypeIpv4);

    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + eth_len);
    ip->version_ihl   = static_cast<uint8_t>((4 << 4) | 5);
    ip->total_length  = hton16(static_cast<uint16_t>(
        outer_ip_len + icmp_hdr_len + emb_ip_len + emb_l4_len));
    ip->next_proto_id = kIpProtoIcmp;
    ip->src_addr      = hton32(0x0A00000A); // router
    ip->dst_addr      = hton32(0x0A000001); // us

    uint8_t* icmp = buf + eth_len + outer_ip_len;
    icmp[0] = 3; // Type 3
    icmp[1] = 4; // Code 4
    // icmp[2..3] checksum (skip)
    // icmp[4..5] unused (zero)
    uint16_t mtu_net = hton16(next_hop_mtu);
    std::memcpy(icmp + 6, &mtu_net, 2);

    auto* emb_ip = reinterpret_cast<rte_ipv4_hdr*>(icmp + icmp_hdr_len);
    emb_ip->version_ihl   = static_cast<uint8_t>((4 << 4) | embedded_ihl_words);
    emb_ip->next_proto_id = embedded_proto;
    emb_ip->src_addr      = hton32(0x0A000001);
    emb_ip->dst_addr      = hton32(0x0A000002);

    // Embedded L4 ports (first 4 bytes of L4)
    uint8_t* emb_l4 = icmp + icmp_hdr_len + emb_ip_len;
    uint16_t sp = hton16(12345);
    uint16_t dp = hton16(443);
    std::memcpy(emb_l4, &sp, 2);
    std::memcpy(emb_l4 + 2, &dp, 2);

    return truncate_embedded_to != 0 ? truncate_embedded_to : total;
}

} // namespace

TEST(PacketParseAdv, IcmpFragNeededFullyParsed) {
    uint8_t buf[256];
    const size_t len = build_icmp_frag_needed(buf, /*mtu=*/1400);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_icmp(&mbuf);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed.is_frag_needed());
    EXPECT_EQ(parsed.type, 3);
    EXPECT_EQ(parsed.code, 4);
    EXPECT_EQ(parsed.next_hop_mtu, 1400);
    EXPECT_TRUE(parsed.embedded_valid);
    EXPECT_EQ(parsed.embedded_src_ip, 0x0A000001u);
    EXPECT_EQ(parsed.embedded_dst_ip, 0x0A000002u);
    EXPECT_EQ(parsed.embedded_proto, kIpProtoTcp);
    EXPECT_EQ(parsed.embedded_src_port, 12345);
    EXPECT_EQ(parsed.embedded_dst_port, 443);
}

TEST(PacketParseAdv, IcmpFragNeededNonTcpNonUdpProtoInvalidatesEmbedded) {
    // RFC 792 requires the embedded first-8-bytes-of-original-L4, but
    // our parser only populates src/dst port for TCP/UDP. An embedded
    // ICMP-in-ICMP (proto 1) must leave `embedded_valid` = false even
    // though the rest of the parse succeeds.
    uint8_t buf[256];
    const size_t len = build_icmp_frag_needed(
        buf, /*mtu=*/1280, /*embedded_proto=*/kIpProtoIcmp);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_icmp(&mbuf);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed.is_frag_needed());
    EXPECT_EQ(parsed.embedded_proto, kIpProtoIcmp);
    EXPECT_FALSE(parsed.embedded_valid);
}

TEST(PacketParseAdv, IcmpFragNeededTruncatedAfterIcmpHeaderReturnsCoreFields) {
    // Truncated so only the 8-byte ICMP header fits. parse_icmp still
    // returns type/code/next_hop_mtu (those are in the 8-byte header),
    // but the embedded_* fields stay default-zeroed because the nested
    // IP header doesn't fit.
    uint8_t buf[256];
    const size_t min_len = kEtherHeaderLen + kIpv4HeaderLen + 8;
    const size_t len = build_icmp_frag_needed(
        buf, /*mtu=*/900, /*embedded_proto=*/kIpProtoTcp,
        /*embedded_ihl_words=*/5, /*truncate_embedded_to=*/min_len);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_icmp(&mbuf);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed.type, 3);
    EXPECT_EQ(parsed.code, 4);
    EXPECT_EQ(parsed.next_hop_mtu, 900);
    EXPECT_FALSE(parsed.embedded_valid);
    EXPECT_EQ(parsed.embedded_src_port, 0);
    EXPECT_EQ(parsed.embedded_dst_port, 0);
}

TEST(PacketParseAdv, IcmpFragNeededTruncatedBeforeIcmpHeaderRejected) {
    // Truncated below the 8-byte ICMP header — parse_icmp must return
    // a default-constructed ParsedIcmp (operator bool() false).
    uint8_t buf[256];
    (void)build_icmp_frag_needed(buf);
    // Cut to just ethernet+ip, no ICMP payload at all.
    const size_t truncated = kEtherHeaderLen + kIpv4HeaderLen + 4; // 4 < 8
    auto mbuf = make_mbuf(buf, truncated);
    auto parsed = parse_icmp(&mbuf);
    EXPECT_FALSE(static_cast<bool>(parsed));
}

TEST(PacketParseAdv, IcmpNonFragNeededTypeLeavesEmbeddedFieldsZero) {
    // Build a Type 3 Code 4, then overwrite type+code to a different
    // diagnostic message (e.g. echo request). parse_icmp exposes
    // type/code but leaves next_hop_mtu and embedded_* at their
    // default zero values — confirming the embedded parse is strictly
    // gated on is_frag_needed().
    uint8_t buf[256];
    const size_t len = build_icmp_frag_needed(buf, /*mtu=*/1100);
    buf[kEtherHeaderLen + kIpv4HeaderLen + 0] = 8; // echo request
    buf[kEtherHeaderLen + kIpv4HeaderLen + 1] = 0;
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_icmp(&mbuf);
    ASSERT_TRUE(parsed);
    EXPECT_FALSE(parsed.is_frag_needed());
    EXPECT_EQ(parsed.type, 8);
    EXPECT_EQ(parsed.code, 0);
    EXPECT_EQ(parsed.next_hop_mtu, 0);
    EXPECT_FALSE(parsed.embedded_valid);
}

TEST(PacketParseAdv, IcmpNullMbufReturnsDefault) {
    auto parsed = parse_icmp(nullptr);
    EXPECT_FALSE(static_cast<bool>(parsed));
}

TEST(PacketParseAdv, IcmpProtocolMismatchRejected) {
    // IP says proto=17 (UDP) but we call parse_icmp — must reject at
    // the proto gate rather than misinterpret UDP bytes as ICMP.
    uint8_t buf[128];
    const size_t len = build_udp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_icmp(&mbuf);
    EXPECT_FALSE(static_cast<bool>(parsed));
}
