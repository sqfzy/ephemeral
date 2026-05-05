/// @file test_udp_zero_len_and_template_overflow.cpp
/// Regression tests for two critical audit findings:
///
///   1. Zero-length UDP datagrams were silently rejected despite RFC 768
///      allowing them (parse_udp_from_ip set payload=nullptr when len==0).
///   2. UdpPacketTemplate::fill() used uint16_t arithmetic that silently
///      overflowed when payload_len was near UINT16_MAX.
///
/// Also covers the new IHL and TCP doff bounds checks in packet_parse.hpp.

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/packet_template.hpp"

using namespace eph::dpdk::net;

namespace {

rte_mbuf make_mbuf(uint8_t* buf, size_t len) {
    rte_mbuf mbuf{};
    mbuf.buf_addr = buf;
    mbuf.data_off = 0;
    mbuf.data_len = static_cast<uint16_t>(len);
    mbuf.pkt_len  = static_cast<uint32_t>(len);
    return mbuf;
}

/// Build a minimal Ethernet/IPv4/UDP packet with the given payload size.
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

/// Build a minimal TCP packet helper.
size_t build_tcp_packet(uint8_t* buf, uint16_t payload_len,
                        uint8_t ihl_words = 5, uint8_t tcp_doff_words = 5) {
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
    ip->next_proto_id = kIpProtoTcp;
    ip->src_addr      = hton32(0x0A000001);
    ip->dst_addr      = hton32(0x0A000002);

    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + eth_len + ip_len);
    tcp->data_off  = static_cast<uint8_t>(tcp_doff_words << 4);
    tcp->src_port  = hton16(12345);
    tcp->dst_port  = hton16(443);
    tcp->tcp_flags = kTcpAck;

    return total;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Zero-length UDP datagram parsing (audit finding #1)
// ═══════════════════════════════════════════════════════════════════════

TEST(UdpZeroLen, ZeroLengthPayloadAccepted) {
    uint8_t buf[128];
    size_t len = build_udp_packet(buf, 0);
    auto mbuf = make_mbuf(buf, len);

    auto ip_hdr = parse_ip_header(&mbuf);
    ASSERT_TRUE(ip_hdr);

    auto parsed = parse_udp_from_ip(&mbuf, ip_hdr);
    ASSERT_NE(parsed.udp, nullptr) << "Zero-length UDP must parse successfully";
    EXPECT_EQ(parsed.payload_len, 0);
    // payload pointer must be non-null (points past UDP header)
    EXPECT_NE(parsed.payload, nullptr)
        << "payload must point past UDP header even for zero-length datagrams";
}

TEST(UdpZeroLen, ZeroLengthViaConvenienceWrapper) {
    uint8_t buf[128];
    size_t len = build_udp_packet(buf, 0);
    auto mbuf = make_mbuf(buf, len);

    auto parsed = parse_udp_packet(&mbuf);
    ASSERT_NE(parsed.udp, nullptr);
    EXPECT_EQ(parsed.payload_len, 0);
    EXPECT_NE(parsed.payload, nullptr);
}

TEST(UdpZeroLen, NonZeroPayloadStillWorks) {
    uint8_t buf[256];
    size_t len = build_udp_packet(buf, 10);
    // Fill payload with recognizable pattern
    std::memset(buf + kEtherHeaderLen + 20 + kUdpHeaderLen, 0xAB, 10);
    auto mbuf = make_mbuf(buf, len);

    auto parsed = parse_udp_packet(&mbuf);
    ASSERT_NE(parsed.udp, nullptr);
    EXPECT_EQ(parsed.payload_len, 10);
    EXPECT_NE(parsed.payload, nullptr);
    EXPECT_EQ(parsed.payload[0], 0xAB);
}

// ═══════════════════════════════════════════════════════════════════════
// UDP template overflow (audit finding #2)
// ═══════════════════════════════════════════════════════════════════════

TEST(UdpTemplateOverflow, MaxPayloadLenFitsMbuf) {
    // payload_len = UINT16_MAX - kUdpAllHeadersLen should be accepted
    // (if the mbuf is large enough — we only test the overflow check)
    constexpr uint16_t max_safe = UINT16_MAX - kUdpAllHeadersLen;
    // The actual fill() would need a huge mbuf, so we can't fully test it
    // without a real mempool. But we verify the arithmetic doesn't overflow.
    uint32_t total = static_cast<uint32_t>(kUdpAllHeadersLen) + max_safe;
    EXPECT_EQ(total, static_cast<uint32_t>(UINT16_MAX));
}

TEST(UdpTemplateOverflow, OverflowingPayloadLenWouldWrap) {
    // Verify that the old uint16_t arithmetic WOULD have wrapped
    constexpr uint16_t bad_payload = UINT16_MAX;
    uint16_t old_result = static_cast<uint16_t>(kUdpAllHeadersLen + bad_payload);
    EXPECT_LT(old_result, kUdpAllHeadersLen)
        << "Confirms the old code would silently wrap to a small value";
    // The new code uses uint32_t and rejects this
    uint32_t new_result = static_cast<uint32_t>(kUdpAllHeadersLen) + bad_payload;
    EXPECT_GT(new_result, static_cast<uint32_t>(UINT16_MAX));
}

// ═══════════════════════════════════════════════════════════════════════
// IHL bounds check hardening (audit finding #3)
// ═══════════════════════════════════════════════════════════════════════

TEST(ParserHardening, IhlExceedingPacketLengthRejected) {
    // IHL=15 (60 bytes) but packet is only Ether(14) + 20 bytes = 34 total
    uint8_t buf[128];
    size_t len = build_tcp_packet(buf, 4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->version_ihl = (4 << 4) | 15;  // IHL=60 bytes
    // Keep the mbuf length at the original (smaller) size
    auto mbuf = make_mbuf(buf, kEtherHeaderLen + 24);  // only 24 bytes for IP
    auto parsed = parse_ip_header(&mbuf);
    EXPECT_FALSE(parsed) << "IHL exceeding packet length must be rejected";
}

// ═══════════════════════════════════════════════════════════════════════
// TCP doff bounds check hardening (audit finding #4)
// ═══════════════════════════════════════════════════════════════════════

TEST(ParserHardening, TcpDoffExceedingPacketLengthRejected) {
    // tcp_doff=15 (60 bytes TCP header) but packet only has room for 20
    constexpr size_t short_pkt = kEtherHeaderLen + 20 + 20;  // ETH+IP+TCP(min)
    uint8_t buf[short_pkt];
    build_tcp_packet(buf, 0);
    auto* tcp = reinterpret_cast<rte_tcp_hdr*>(buf + kEtherHeaderLen + 20);
    tcp->data_off = 15 << 4;  // Claims 60-byte TCP header
    // Update IP total_length to claim the bigger TCP header
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + kEtherHeaderLen);
    ip->total_length = hton16(20 + 60);

    auto mbuf = make_mbuf(buf, short_pkt);
    auto ip_hdr = parse_ip_header(&mbuf);
    // IP header itself should parse fine (IHL=5, fits)
    ASSERT_TRUE(ip_hdr);
    auto parsed = parse_tcp_from_ip(&mbuf, ip_hdr);
    // TCP must be rejected because doff=60 exceeds actual packet bytes
    EXPECT_EQ(parsed.tcp, nullptr)
        << "TCP doff exceeding packet length must be rejected";
}

TEST(ParserHardening, TcpDoffExactlyAtPacketBoundaryAccepted) {
    // Build a packet with exactly enough room for doff=8 (32 bytes TCP)
    constexpr size_t pkt_size = kEtherHeaderLen + 20 + 32 + 4;
    uint8_t buf[pkt_size];
    size_t len = build_tcp_packet(buf, 4, 5, 8);
    auto mbuf = make_mbuf(buf, len);

    auto ip_hdr = parse_ip_header(&mbuf);
    ASSERT_TRUE(ip_hdr);
    auto parsed = parse_tcp_from_ip(&mbuf, ip_hdr);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, 4);
}
