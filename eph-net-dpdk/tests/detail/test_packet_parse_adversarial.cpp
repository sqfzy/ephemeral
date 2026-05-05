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
    // IP header doesn't fit. A real router truncating ICMP MUST update
    // ip->total_length to match (RFC 791 §3.2); we mirror that here so
    // the parser's defense-in-depth ip_total cross-check (added in the
    // batch-2 round-2 fix) sees a self-consistent IP header.
    uint8_t buf[256];
    const size_t min_len = kEtherHeaderLen + kIpv4HeaderLen + 8;
    const size_t len = build_icmp_frag_needed(
        buf, /*mtu=*/900, /*embedded_proto=*/kIpProtoTcp,
        /*embedded_ihl_words=*/5, /*truncate_embedded_to=*/min_len);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    // Update ip->total_length to reflect the truncation (IP header + 8B
    // ICMP). Without this update the IP header would lie about how many
    // bytes follow it, which the parser correctly rejects as malformed.
    ip->total_length = hton16(static_cast<uint16_t>(kIpv4HeaderLen + 8));
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

TEST(PacketParseAdv, IcmpRejectsWhenIpTotalLengthSmallerThanIcmpFooter) {
    // Defense-in-depth (RFC 791 §3.2): IP `total_length` is the source of
    // truth for how many bytes follow the IP header. `parse_icmp` validates
    // the ICMP header / Type-3-Code-4 footer / embedded IP+L4 against
    // `pkt_len` (the mbuf data_len) but does NOT cross-check against
    // `ip->total_length`. A crafted frame whose pkt_len is large (e.g. 70B
    // — possibly arrived padded by NIC/router) but whose IP total_length
    // claims only the bare IP header (20B, no payload) is still walked by
    // the parser as if the trailing ICMP+embedded fields were legitimate.
    //
    // Impact: an attacker on the L2 segment who can deliver such a packet
    // can inject arbitrary `next_hop_mtu` and embedded 4-tuple values into
    // the ICMP dispatch path, potentially triggering MSS shrinks on
    // unrelated active flows that match the spoofed embedded 4-tuple.
    //
    // Expected behaviour: parse_icmp rejects the packet wholesale because
    // ip->total_length is incompatible with the ICMP footer the bytes
    // claim to encode.
    uint8_t buf[256];
    const size_t pkt_len = build_icmp_frag_needed(buf, /*mtu=*/1280);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    // Claim only the bare IP header (20B) — no ICMP payload at all per
    // ip->total_length, even though the buffer carries the full 56B
    // ICMP-T3C4 + embedded headers afterwards.
    ip->total_length = hton16(static_cast<uint16_t>(kIpv4HeaderLen));
    auto mbuf = make_mbuf(buf, pkt_len);
    auto parsed = parse_icmp(&mbuf);
    EXPECT_FALSE(static_cast<bool>(parsed))
        << "parse_icmp accepted a packet whose ip->total_length excludes "
           "the ICMP footer — defense-in-depth gap";
}

TEST(PacketParseAdv, IcmpRejectsWhenIpTotalLengthExceedsPktLen) {
    // Symmetric defense: ip->total_length must not claim MORE bytes than
    // the mbuf actually carries. `parse_ip_header` does not enforce this
    // (it only checks `ihl`); without a matching gate in parse_icmp, a
    // sender can lie in the upward direction and have the parser still
    // succeed because pkt_len happens to cover the ICMP+embedded reads.
    uint8_t buf[256];
    const size_t pkt_len = build_icmp_frag_needed(buf, /*mtu=*/1280);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    // Claim a total_length larger than pkt_len permits — even if the
    // ICMP footer "fits" in the buffer, the IP header is internally
    // inconsistent and must not be trusted.
    ip->total_length = hton16(static_cast<uint16_t>(pkt_len));  // exceeds payload
    // Trim mbuf to less than ip total_length claims.
    auto mbuf = make_mbuf(buf, pkt_len - 4);
    auto parsed = parse_icmp(&mbuf);
    EXPECT_FALSE(static_cast<bool>(parsed))
        << "parse_icmp accepted a packet whose ip->total_length exceeds "
           "pkt_len — defense-in-depth gap";
}

TEST(PacketParseAdv, IcmpAcceptsConsistentIpTotalLength) {
    // Regression guard: the well-formed case must continue to parse so
    // the new bounds checks don't reject legitimate router-originated
    // Type 3 Code 4 messages.
    uint8_t buf[256];
    const size_t pkt_len = build_icmp_frag_needed(buf, /*mtu=*/1400);
    // build_icmp_frag_needed already sets ip->total_length consistently;
    // verify the round-trip parses fully.
    auto mbuf = make_mbuf(buf, pkt_len);
    auto parsed = parse_icmp(&mbuf);
    ASSERT_TRUE(static_cast<bool>(parsed));
    EXPECT_TRUE(parsed.is_frag_needed());
    EXPECT_EQ(parsed.next_hop_mtu, 1400);
    EXPECT_TRUE(parsed.embedded_valid);
}

TEST(PacketParseAdv, IcmpRefusesEmbeddedPastIpTotalEvenWhenPktLenAllows) {
    // Regression for r2 commit 804a2a9e (fix(dpdk/packet_parse): bound ICMP
    // embedded reads by ip_total). The bug shape: NIC delivers Ethernet
    // padding past the IP-declared end (802.3 minimum frame is 64 bytes).
    // pre-fix, parse_icmp's bound was `pkt_len` only — an attacker who
    // controlled the trailer bytes could craft them to look like a valid
    // embedded IP+L4 and have parse_icmp surface the forged 4-tuple.
    //
    // Distinct from IcmpRejectsWhenIpTotalLengthSmallerThanIcmpFooter
    // (the d8cb1b0d/fd6f68ce regression test) which sets ip_total < ICMP
    // footer and expects whole-packet rejection. Here ip_total is large
    // enough to cover the legitimate ICMP T3C4 body (header + unused + MTU)
    // but stops BEFORE the embedded IP+L4 — those bytes are now padding,
    // and parse_icmp must NOT surface them as a 4-tuple.
    uint8_t buf[256];
    const size_t pkt_len = build_icmp_frag_needed(buf, /*mtu=*/1400);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);

    // Trim ip->total_length so it covers OUTER ip header + ICMP-T3C4 8-byte
    // header but EXCLUDES the embedded IP+L4 (which is now NIC-padding).
    // Embedded fields in the packet still carry build_icmp_frag_needed's
    // crafted ports/IPs (12345 → 443 over 10.0.0.1 → 10.0.0.2) — exactly
    // the kind of forged 4-tuple a hostile L2 peer could plant.
    constexpr uint16_t kIcmpHeaderLen = 8;
    ip->total_length = hton16(static_cast<uint16_t>(kIpv4HeaderLen + kIcmpHeaderLen));

    auto mbuf = make_mbuf(buf, pkt_len);
    auto parsed = parse_icmp(&mbuf);

    // Whole-packet acceptance is fine here — the IP+ICMP header are
    // legitimate. The crucial guarantee is that the *embedded* fields
    // are NOT surfaced because they sit past ip_total.
    ASSERT_TRUE(static_cast<bool>(parsed))
        << "parse_icmp rejected an otherwise-well-formed ICMP T3C4 whose "
           "ip_total only covers the ICMP header (NIC-padding shape)";
    EXPECT_TRUE(parsed.is_frag_needed());
    EXPECT_EQ(parsed.next_hop_mtu, 1400);
    EXPECT_FALSE(parsed.embedded_valid)
        << "parse_icmp surfaced an embedded 4-tuple that lay PAST "
           "ip_total — regression of r2 commit 804a2a9e (forged-padding "
           "4-tuple injection)";
}

// ═══════════════════════════════════════════════════════════════════════
// IP fragmentation — our L4 parsers assume the TCP/UDP/ICMP header sits
// immediately after the IP header, which is only true for the *first*
// fragment (MF=1, offset=0) or an unfragmented packet. Accepting a
// non-first fragment would let an attacker place arbitrary payload bytes
// where the parser reads src/dst/seq/ack. We don't do L3 reassembly, so
// drop fragmented packets at parse_ip_header. DF=1 alone is fine.
// ═══════════════════════════════════════════════════════════════════════

TEST(PacketParseAdv, IpMoreFragmentsBitRejected) {
    uint8_t buf[128];
    const size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    // MF=1, offset=0 — first fragment of a larger datagram.
    ip->fragment_offset = hton16(kIpMoreFragments);
    auto mbuf = make_mbuf(buf, len);
    auto hdr = parse_ip_header(&mbuf);
    EXPECT_FALSE(static_cast<bool>(hdr));
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

TEST(PacketParseAdv, IpNonZeroFragmentOffsetRejected) {
    uint8_t buf[128];
    const size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    // MF=0, offset=185 (in 8-byte units, so byte 1480) — a middle/last fragment.
    ip->fragment_offset = hton16(185);
    auto mbuf = make_mbuf(buf, len);
    auto hdr = parse_ip_header(&mbuf);
    EXPECT_FALSE(static_cast<bool>(hdr));
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
}

TEST(PacketParseAdv, IpMoreFragmentsAndNonZeroOffsetRejected) {
    // A middle fragment typically has MF=1 AND offset > 0. Both flags
    // set simultaneously must be rejected — this is the attacker-
    // friendly overlapping-fragment case.
    uint8_t buf[128];
    const size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->fragment_offset = hton16(kIpMoreFragments | 1);
    auto mbuf = make_mbuf(buf, len);
    EXPECT_FALSE(static_cast<bool>(parse_ip_header(&mbuf)));
}

TEST(PacketParseAdv, IpDontFragmentAloneAccepted) {
    // DF=1 (no MF, no offset) is the common HFT send pattern — must
    // pass through unchanged. This is a regression guard for the
    // fragment-rejection patch.
    uint8_t buf[128];
    const size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->fragment_offset = hton16(kIpDontFragment);
    auto mbuf = make_mbuf(buf, len);
    auto hdr = parse_ip_header(&mbuf);
    EXPECT_TRUE(static_cast<bool>(hdr));
    auto parsed = parse_packet(&mbuf);
    EXPECT_NE(parsed.tcp, nullptr);
}

TEST(PacketParseAdv, UdpFragmentedRejectedBySharedParseIpHeader) {
    // UDP's parse path shares parse_ip_header with TCP; confirm the
    // fragment rejection covers UDP too (defensive — a fragmented UDP
    // message attack would otherwise misinterpret payload bytes as
    // UDP length / checksum).
    uint8_t buf[128];
    const size_t len = build_udp_packet(buf, 16);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->fragment_offset = hton16(kIpMoreFragments);
    auto mbuf = make_mbuf(buf, len);
    EXPECT_EQ(parse_udp_packet(&mbuf).udp, nullptr);
}

TEST(PacketParseAdv, IcmpFragmentedRejectedBySharedParseIpHeader) {
    // Same gate applies to ICMP — a fragmented Type 3 Code 4 could
    // otherwise feed bogus next_hop_mtu / embedded 4-tuple into
    // TcpSession::on_icmp_frag_needed.
    uint8_t buf[256];
    const size_t len = build_icmp_frag_needed(buf, /*mtu=*/1280);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->fragment_offset = hton16(kIpMoreFragments | 2);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_icmp(&mbuf);
    EXPECT_FALSE(static_cast<bool>(parsed));
}

// ═══════════════════════════════════════════════════════════════════════
// Multi-segment mbuf — all downstream parsers use first-segment data_len.
// Chained mbufs would let bounds-check logic operate on the wrong byte
// count; parse_ip_header rejects outright as defense-in-depth.
// ═══════════════════════════════════════════════════════════════════════

TEST(PacketParseAdv, MultiSegmentMbufRejected) {
    uint8_t buf[128];
    const size_t len = build_tcp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    // Simulate a NIC delivering the packet as a chain by setting
    // nb_segs=2. Our parsers assume nb_segs==1 (single contiguous
    // buffer) and must bail rather than silently parse the first
    // segment alone.
    mbuf.nb_segs = 2;
    EXPECT_FALSE(static_cast<bool>(parse_ip_header(&mbuf)));
    EXPECT_EQ(parse_packet(&mbuf).tcp, nullptr);
    EXPECT_EQ(parse_udp_packet(&mbuf).udp, nullptr);
}

TEST(PacketParseAdv, SingleSegmentMbufAccepted) {
    // Regression guard for the multi-segment rejection above: the
    // normal case (nb_segs==1, what PMDs deliver for standard-MTU
    // traffic) must continue to parse without incident.
    uint8_t buf[128];
    const size_t len = build_tcp_packet(buf, 4);
    auto mbuf = make_mbuf(buf, len);
    mbuf.nb_segs = 1;
    EXPECT_TRUE(static_cast<bool>(parse_ip_header(&mbuf)));
    EXPECT_NE(parse_packet(&mbuf).tcp, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
// parse_tcp_options — RFC 793 / RFC 1323 options walker.
//
// Until now this function has had zero direct tests; coverage was
// implicitly via DpdkTcpStream MSS negotiation. The parser has subtle
// boundary rules (NOP single-byte, EOL terminates, len<2 malformed,
// tail-overrun stops cleanly) that warrant explicit pinning.
// ═══════════════════════════════════════════════════════════════════════

namespace {
// Build a SYN packet whose TCP options area carries `opts_bytes` after the
// 20-byte fixed TCP header. Pads the options area to a 4-byte multiple
// with NOPs so data_off lands on a word boundary, matching real wire
// frames.
size_t build_tcp_with_options(uint8_t* buf,
                               const std::vector<uint8_t>& opts_bytes) {
    // Round up to multiple of 4 — TCP options area must end on a 4-byte
    // boundary (data_off is in 32-bit words).
    std::vector<uint8_t> padded = opts_bytes;
    while (padded.size() % 4u != 0) padded.push_back(1);  // NOP
    const uint8_t doff_words = static_cast<uint8_t>(5 + padded.size() / 4u);
    const size_t total = build_tcp_packet(buf, /*payload_len=*/0,
                                           /*ihl_words=*/5,
                                           /*tcp_doff_words=*/doff_words);
    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + kEtherHeaderLen + 20);
    tcp->tcp_flags = kTcpSyn;
    auto* opts_dst = reinterpret_cast<uint8_t*>(tcp) + kTcpHeaderLen;
    std::memcpy(opts_dst, padded.data(), padded.size());
    return total;
}
} // namespace

TEST(PacketParseAdv, TcpOptionsAbsentReturnsAllFalse) {
    // doff == 5 → no options area. parse_tcp_options must short-circuit.
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, /*payload_len=*/4,
                                   /*ihl_words=*/5, /*tcp_doff_words=*/5);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    auto opts = parse_tcp_options(parsed);
    EXPECT_FALSE(opts.has_mss);
    EXPECT_FALSE(opts.has_wscale);
    EXPECT_FALSE(opts.has_sack_perm);
    EXPECT_EQ(opts.mss, 0u);
    EXPECT_EQ(opts.wscale, 0u);
}

TEST(PacketParseAdv, TcpOptionsNullTcpReturnsAllDefault) {
    // parse_tcp_options on a synthetically-empty ParsedPacket must not
    // dereference the null tcp pointer.
    eph::dpdk::net::ParsedPacket empty{};
    auto opts = parse_tcp_options(empty);
    EXPECT_FALSE(opts.has_mss);
    EXPECT_FALSE(opts.has_wscale);
    EXPECT_FALSE(opts.has_sack_perm);
}

TEST(PacketParseAdv, TcpOptionsMssWscaleSackPermAllPresent) {
    // {kind=2, len=4, MSS=1460} {kind=3, len=3, wscale=7} {kind=4, len=2}
    std::vector<uint8_t> opts = {
        2, 4, 0x05, 0xB4,   // MSS = 1460 (0x05B4)
        3, 3, 7,
        4, 2,
    };
    uint8_t buf[256];
    size_t len = build_tcp_with_options(buf, opts);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    auto out = parse_tcp_options(parsed);
    EXPECT_TRUE(out.has_mss);
    EXPECT_EQ(out.mss, 1460u);
    EXPECT_TRUE(out.has_wscale);
    EXPECT_EQ(out.wscale, 7u);
    EXPECT_TRUE(out.has_sack_perm);
}

TEST(PacketParseAdv, TcpOptionsEolTerminatesParse) {
    // NOP, EOL, then a fake MSS that must NOT be parsed because EOL
    // already terminated the walk.
    std::vector<uint8_t> opts = {
        1,                       // NOP
        0,                       // EOL — terminator
        2, 4, 0x05, 0xB4,        // would-be MSS=1460, must be ignored
    };
    uint8_t buf[256];
    size_t len = build_tcp_with_options(buf, opts);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    auto out = parse_tcp_options(parsed);
    EXPECT_FALSE(out.has_mss);
}

TEST(PacketParseAdv, TcpOptionsLenBelow2StopsParse) {
    // {kind=99, len=1} is malformed (len must be >= 2). The parser must
    // stop at this offending byte; subsequent valid options must NOT be
    // surfaced — bailing on first malformed kind is the documented contract.
    std::vector<uint8_t> opts = {
        99, 1,                   // malformed — len < 2
        2, 4, 0x05, 0xB4,        // a real MSS that must be ignored
    };
    uint8_t buf[256];
    size_t len = build_tcp_with_options(buf, opts);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    auto out = parse_tcp_options(parsed);
    EXPECT_FALSE(out.has_mss);
}

TEST(PacketParseAdv, TcpOptionsLenOverrunStopsParse) {
    // {kind=2, len=10} declares more bytes than the options area
    // contains — must bail before reading past the buffer.
    std::vector<uint8_t> opts = {
        2, 10, 0x05, 0xB4,      // declared len=10, only 2 payload bytes follow
    };
    uint8_t buf[256];
    size_t len = build_tcp_with_options(buf, opts);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    auto out = parse_tcp_options(parsed);
    EXPECT_FALSE(out.has_mss);
}

TEST(PacketParseAdv, TcpOptionsUnknownKindSkipped) {
    // Unknown option kind=99 with proper len-delimited 4 bytes — parser
    // must skip past it (i += len) and still surface following options.
    std::vector<uint8_t> opts = {
        99, 4, 0xAA, 0xBB,       // unknown kind — len=4, must be skipped
        2, 4, 0x05, 0xB4,        // MSS=1460 — must still be parsed after skip
    };
    uint8_t buf[256];
    size_t len = build_tcp_with_options(buf, opts);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    auto out = parse_tcp_options(parsed);
    EXPECT_TRUE(out.has_mss);
    EXPECT_EQ(out.mss, 1460u);
}

TEST(PacketParseAdv, TcpOptionsMssWrongLengthIgnored) {
    // {kind=2, len=3} is malformed for MSS — RFC 793 requires len=4.
    // The parser must skip the malformed option (no MSS surfaced) but
    // continue to subsequent valid options. Behaviour: the option is
    // walked over (i += 3) and the next option is parsed normally.
    std::vector<uint8_t> opts = {
        2, 3, 0x05,              // MSS with bogus len=3 — silently ignored
        3, 3, 7,                 // valid wscale=7 follows
    };
    uint8_t buf[256];
    size_t len = build_tcp_with_options(buf, opts);
    auto mbuf = make_mbuf(buf, len);
    auto parsed = parse_packet(&mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    auto out = parse_tcp_options(parsed);
    EXPECT_FALSE(out.has_mss);
    EXPECT_TRUE(out.has_wscale);
    EXPECT_EQ(out.wscale, 7u);
}
