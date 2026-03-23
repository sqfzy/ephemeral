/// @file test_arp.cpp
/// Unit tests for ARP protocol constants, packet structure, and parsing logic.
///
/// NOTE: Tests for resolve() require DPDK EAL and a real NIC — those are
/// integration tests run separately. These tests validate the protocol-level
/// correctness without DPDK runtime.

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "eph/dpdk/arp.hpp"

using namespace eph::dpdk;
using namespace eph::dpdk::arp;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time validation
// ─────────────────────────────────────────────────────────────────────────────

static_assert(sizeof(ArpPacket) == 28, "ARP packet must be 28 bytes");
static_assert(kEtherTypeArp == 0x0806);
static_assert(kArpHwTypeEthernet == 1);
static_assert(kArpProtoIpv4 == 0x0800);
static_assert(kArpOpRequest == 1);
static_assert(kArpOpReply == 2);
static_assert(kArpHwAddrLen == 6);
static_assert(kArpProtoAddrLen == 4);

// ─────────────────────────────────────────────────────────────────────────────
// ArpPacket field layout (verify packed layout matches wire format)
// ─────────────────────────────────────────────────────────────────────────────

TEST(ArpPacket, FieldOffsets) {
    // Verify packed layout: fields at expected byte offsets
    ArpPacket pkt{};
    auto base = reinterpret_cast<const uint8_t*>(&pkt);

    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&pkt.hw_type) - base, 0);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&pkt.proto_type) - base, 2);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&pkt.hw_addr_len) - base, 4);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&pkt.proto_addr_len) - base, 5);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&pkt.opcode) - base, 6);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&pkt.sender_mac) - base, 8);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&pkt.sender_ip) - base, 14);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&pkt.target_mac) - base, 18);
    EXPECT_EQ(reinterpret_cast<const uint8_t*>(&pkt.target_ip) - base, 24);
}

// ─────────────────────────────────────────────────────────────────────────────
// ARP request construction (test packet content without DPDK mbuf)
// ─────────────────────────────────────────────────────────────────────────────

/// Manually construct what build_arp_request should produce, then verify.
TEST(ArpRequest, PacketContent) {
    // Simulate the packet buffer (Ethernet header + ARP payload)
    uint8_t buf[sizeof(rte_ether_hdr) + sizeof(ArpPacket)]{};

    rte_ether_addr src_mac = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
    uint32_t src_ip   = net::parse_ipv4("10.0.0.1");
    uint32_t target_ip = net::parse_ipv4("10.0.0.2");

    // Build Ethernet header
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    rte_ether_addr_copy(&kBroadcastMac, &eth->dst_addr);
    rte_ether_addr_copy(&src_mac, &eth->src_addr);
    eth->ether_type = net::hton16(kEtherTypeArp);

    // Build ARP payload
    auto* arp = reinterpret_cast<ArpPacket*>(buf + sizeof(rte_ether_hdr));
    arp->hw_type        = net::hton16(kArpHwTypeEthernet);
    arp->proto_type     = net::hton16(kArpProtoIpv4);
    arp->hw_addr_len    = kArpHwAddrLen;
    arp->proto_addr_len = kArpProtoAddrLen;
    arp->opcode         = net::hton16(kArpOpRequest);
    std::memcpy(arp->sender_mac, src_mac.addr_bytes, 6);
    arp->sender_ip = net::hton32(src_ip);
    std::memset(arp->target_mac, 0, 6);
    arp->target_ip = net::hton32(target_ip);

    // Verify Ethernet header
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(eth->dst_addr.addr_bytes[i], 0xFF)
            << "Broadcast MAC byte " << i;
    }
    EXPECT_EQ(eth->src_addr.addr_bytes[0], 0x00);
    EXPECT_EQ(eth->src_addr.addr_bytes[5], 0x55);
    EXPECT_EQ(net::ntoh16(eth->ether_type), kEtherTypeArp);

    // Verify ARP fields
    EXPECT_EQ(net::ntoh16(arp->hw_type), kArpHwTypeEthernet);
    EXPECT_EQ(net::ntoh16(arp->proto_type), kArpProtoIpv4);
    EXPECT_EQ(arp->hw_addr_len, 6);
    EXPECT_EQ(arp->proto_addr_len, 4);
    EXPECT_EQ(net::ntoh16(arp->opcode), kArpOpRequest);
    EXPECT_EQ(net::ntoh32(arp->sender_ip), src_ip);
    EXPECT_EQ(net::ntoh32(arp->target_ip), target_ip);

    // Target MAC should be zero (unknown)
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(arp->target_mac[i], 0) << "Target MAC byte " << i;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ARP reply parsing (simulate reply packet without DPDK mbuf)
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Helper: build a simulated ARP reply in a raw buffer.
/// Returns the total frame size.
size_t build_fake_arp_reply(uint8_t* buf, size_t buf_size,
                             const rte_ether_addr& sender_mac,
                             uint32_t sender_ip,
                             const rte_ether_addr& target_mac,
                             uint32_t target_ip_host) {
    constexpr size_t frame_len = sizeof(rte_ether_hdr) + sizeof(ArpPacket);
    if (buf_size < frame_len) return 0;
    std::memset(buf, 0, frame_len);

    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    rte_ether_addr_copy(&target_mac, &eth->dst_addr);
    rte_ether_addr_copy(&sender_mac, &eth->src_addr);
    eth->ether_type = net::hton16(kEtherTypeArp);

    auto* arp = reinterpret_cast<ArpPacket*>(buf + sizeof(rte_ether_hdr));
    arp->hw_type        = net::hton16(kArpHwTypeEthernet);
    arp->proto_type     = net::hton16(kArpProtoIpv4);
    arp->hw_addr_len    = kArpHwAddrLen;
    arp->proto_addr_len = kArpProtoAddrLen;
    arp->opcode         = net::hton16(kArpOpReply);
    std::memcpy(arp->sender_mac, sender_mac.addr_bytes, 6);
    arp->sender_ip = net::hton32(sender_ip);
    std::memcpy(arp->target_mac, target_mac.addr_bytes, 6);
    arp->target_ip = net::hton32(target_ip_host);

    return frame_len;
}

/// Minimal fake mbuf for testing parse_arp_reply without DPDK EAL.
/// Only data_len and the data pointer are used by parse_arp_reply.
struct FakeMbuf {
    uint8_t  buf[128]{};
    rte_mbuf mbuf{};

    FakeMbuf() {
        // Point mbuf data to our buffer
        // rte_pktmbuf_mtod uses mbuf->buf_addr + mbuf->data_off
        mbuf.buf_addr = buf;
        mbuf.data_off = 0;
        mbuf.data_len = 0;
        mbuf.pkt_len  = 0;
    }

    uint8_t* data() { return buf; }
    void set_len(uint16_t len) {
        mbuf.data_len = len;
        mbuf.pkt_len  = len;
    }
};

} // anonymous namespace

TEST(ArpReply, ValidReplyParsesCorrectly) {
    rte_ether_addr gateway_mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    rte_ether_addr our_mac     = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
    uint32_t gateway_ip = net::parse_ipv4("10.0.0.1");
    uint32_t our_ip     = net::parse_ipv4("10.0.0.100");

    FakeMbuf fake;
    size_t len = build_fake_arp_reply(fake.data(), sizeof(fake.buf),
                                       gateway_mac, gateway_ip,
                                       our_mac, our_ip);
    fake.set_len(static_cast<uint16_t>(len));

    auto result = parse_arp_reply(&fake.mbuf, gateway_ip);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::memcmp(result->addr_bytes, gateway_mac.addr_bytes, 6), 0);
}

TEST(ArpReply, WrongSenderIpReturnsNullopt) {
    rte_ether_addr gateway_mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    rte_ether_addr our_mac     = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
    uint32_t gateway_ip   = net::parse_ipv4("10.0.0.1");
    uint32_t different_ip  = net::parse_ipv4("10.0.0.99");
    uint32_t our_ip        = net::parse_ipv4("10.0.0.100");

    FakeMbuf fake;
    // Reply comes from gateway_ip, but we're looking for different_ip
    size_t len = build_fake_arp_reply(fake.data(), sizeof(fake.buf),
                                       gateway_mac, gateway_ip,
                                       our_mac, our_ip);
    fake.set_len(static_cast<uint16_t>(len));

    auto result = parse_arp_reply(&fake.mbuf, different_ip);
    EXPECT_FALSE(result.has_value());
}

TEST(ArpReply, ArpRequestIgnored) {
    rte_ether_addr mac1 = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    rte_ether_addr mac2 = {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}};
    uint32_t ip1 = net::parse_ipv4("10.0.0.1");
    uint32_t ip2 = net::parse_ipv4("10.0.0.100");

    FakeMbuf fake;
    size_t len = build_fake_arp_reply(fake.data(), sizeof(fake.buf),
                                       mac1, ip1, mac2, ip2);
    fake.set_len(static_cast<uint16_t>(len));

    // Change opcode to REQUEST
    auto* arp = reinterpret_cast<ArpPacket*>(
        fake.data() + sizeof(rte_ether_hdr));
    arp->opcode = net::hton16(kArpOpRequest);

    auto result = parse_arp_reply(&fake.mbuf, ip1);
    EXPECT_FALSE(result.has_value())
        << "ARP request should not be parsed as reply";
}

TEST(ArpReply, NonArpEtherTypeIgnored) {
    FakeMbuf fake;
    // Build valid ARP reply
    rte_ether_addr mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    uint32_t ip = net::parse_ipv4("10.0.0.1");
    size_t len = build_fake_arp_reply(fake.data(), sizeof(fake.buf),
                                       mac, ip, mac, ip);
    fake.set_len(static_cast<uint16_t>(len));

    // Change EtherType to IPv4 instead of ARP
    auto* eth = reinterpret_cast<rte_ether_hdr*>(fake.data());
    eth->ether_type = net::hton16(net::kEtherTypeIpv4);

    auto result = parse_arp_reply(&fake.mbuf, ip);
    EXPECT_FALSE(result.has_value())
        << "Non-ARP EtherType should be ignored";
}

TEST(ArpReply, TruncatedPacketReturnsNullopt) {
    FakeMbuf fake;
    // Set data_len too short for a valid ARP frame
    fake.set_len(sizeof(rte_ether_hdr) + 10); // Need 28 for ARP, only 10

    auto result = parse_arp_reply(&fake.mbuf, net::parse_ipv4("10.0.0.1"));
    EXPECT_FALSE(result.has_value())
        << "Truncated packet should return nullopt";
}

TEST(ArpReply, WrongHwTypeIgnored) {
    rte_ether_addr mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    uint32_t ip = net::parse_ipv4("10.0.0.1");

    FakeMbuf fake;
    size_t len = build_fake_arp_reply(fake.data(), sizeof(fake.buf),
                                       mac, ip, mac, ip);
    fake.set_len(static_cast<uint16_t>(len));

    // Change hardware type to non-Ethernet
    auto* arp = reinterpret_cast<ArpPacket*>(
        fake.data() + sizeof(rte_ether_hdr));
    arp->hw_type = net::hton16(99);

    auto result = parse_arp_reply(&fake.mbuf, ip);
    EXPECT_FALSE(result.has_value())
        << "Non-Ethernet hardware type should be ignored";
}

TEST(ArpReply, WrongHwAddrLenIgnored) {
    rte_ether_addr mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    uint32_t ip = net::parse_ipv4("10.0.0.1");

    FakeMbuf fake;
    size_t len = build_fake_arp_reply(fake.data(), sizeof(fake.buf),
                                       mac, ip, mac, ip);
    fake.set_len(static_cast<uint16_t>(len));

    // Set hw_addr_len to 7 instead of 6 (RFC 826: must be 6 for Ethernet)
    auto* arp = reinterpret_cast<ArpPacket*>(
        fake.data() + sizeof(rte_ether_hdr));
    arp->hw_addr_len = 7;

    auto result = parse_arp_reply(&fake.mbuf, ip);
    EXPECT_FALSE(result.has_value())
        << "Invalid hw_addr_len should be rejected";
}

TEST(ArpReply, WrongProtoAddrLenIgnored) {
    rte_ether_addr mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    uint32_t ip = net::parse_ipv4("10.0.0.1");

    FakeMbuf fake;
    size_t len = build_fake_arp_reply(fake.data(), sizeof(fake.buf),
                                       mac, ip, mac, ip);
    fake.set_len(static_cast<uint16_t>(len));

    // Set proto_addr_len to 16 instead of 4 (RFC 826: must be 4 for IPv4)
    auto* arp = reinterpret_cast<ArpPacket*>(
        fake.data() + sizeof(rte_ether_hdr));
    arp->proto_addr_len = 16;

    auto result = parse_arp_reply(&fake.mbuf, ip);
    EXPECT_FALSE(result.has_value())
        << "Invalid proto_addr_len should be rejected";
}

TEST(ArpReply, WrongProtoTypeIgnored) {
    rte_ether_addr mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    uint32_t ip = net::parse_ipv4("10.0.0.1");

    FakeMbuf fake;
    size_t len = build_fake_arp_reply(fake.data(), sizeof(fake.buf),
                                       mac, ip, mac, ip);
    fake.set_len(static_cast<uint16_t>(len));

    // Change protocol type to IPv6 instead of IPv4
    auto* arp = reinterpret_cast<ArpPacket*>(
        fake.data() + sizeof(rte_ether_hdr));
    arp->proto_type = net::hton16(0x86DD);  // IPv6

    auto result = parse_arp_reply(&fake.mbuf, ip);
    EXPECT_FALSE(result.has_value())
        << "Non-IPv4 protocol type should be ignored";
}

TEST(ArpReply, ZeroAddrLensIgnored) {
    rte_ether_addr mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    uint32_t ip = net::parse_ipv4("10.0.0.1");

    FakeMbuf fake;
    size_t len = build_fake_arp_reply(fake.data(), sizeof(fake.buf),
                                       mac, ip, mac, ip);
    fake.set_len(static_cast<uint16_t>(len));

    auto* arp = reinterpret_cast<ArpPacket*>(
        fake.data() + sizeof(rte_ether_hdr));
    arp->hw_addr_len = 0;
    arp->proto_addr_len = 0;

    auto result = parse_arp_reply(&fake.mbuf, ip);
    EXPECT_FALSE(result.has_value())
        << "Zero address lengths should be rejected";
}

// ─────────────────────────────────────────────────────────────────────────────
// MAC formatting
// ─────────────────────────────────────────────────────────────────────────────

TEST(ArpUtil, FormatMac) {
    rte_ether_addr mac = {{0x00, 0x11, 0x22, 0xAA, 0xBB, 0xCC}};
    auto str = detail::format_mac(mac);
    EXPECT_STREQ(str.data(), "00:11:22:aa:bb:cc");
}

TEST(ArpUtil, FormatMacBroadcast) {
    auto str = detail::format_mac(kBroadcastMac);
    EXPECT_STREQ(str.data(), "ff:ff:ff:ff:ff:ff");
}

TEST(ArpUtil, FormatMacZero) {
    rte_ether_addr zero = {{0, 0, 0, 0, 0, 0}};
    auto str = detail::format_mac(zero);
    EXPECT_STREQ(str.data(), "00:00:00:00:00:00");
}
