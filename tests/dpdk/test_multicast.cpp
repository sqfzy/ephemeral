#include <gtest/gtest.h>
#include "eph/dpdk/multicast.hpp"

using namespace eph::dpdk;
using namespace eph::dpdk::net;

// ─────────────────────────────────────────────────────────────────────────────
// Test helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Fake rte_mbuf with inline data buffer for building test packets.
struct FakeUdpMbuf {
    uint8_t  buf[512]{};
    rte_mbuf mbuf{};

    FakeUdpMbuf() {
        mbuf.buf_addr = buf;
        mbuf.data_off = 0;
        mbuf.data_len = 0;
        mbuf.pkt_len  = 0;
    }

    uint8_t* data() { return buf; }
    void set_len(uint16_t len) { mbuf.data_len = len; mbuf.pkt_len = len; }
};

/// Build a fake Ethernet/IPv4/UDP packet in the given buffer.
/// Returns total packet length, or 0 on buffer overflow.
size_t build_fake_udp_packet(uint8_t* buf, size_t buf_size,
                              uint32_t src_ip_host, uint16_t src_port,
                              uint32_t dst_ip_host, uint16_t dst_port,
                              const uint8_t* payload, uint16_t payload_len) {
    constexpr uint16_t eth_len = kEtherHeaderLen;
    constexpr uint16_t ip_len  = kIpv4HeaderLen;
    constexpr uint16_t udp_hdr = kUdpHeaderLen;
    uint16_t total = eth_len + ip_len + udp_hdr + payload_len;
    if (buf_size < total) return 0;
    std::memset(buf, 0, total);

    // Ethernet header
    auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
    auto mcast_mac = multicast_mac_from_ip(dst_ip_host);
    rte_ether_addr_copy(&mcast_mac, &eth->dst_addr);
    eth->ether_type = hton16(kEtherTypeIpv4);

    // IPv4 header
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + eth_len);
    ip->version_ihl    = kIpv4VersionIhl5;
    ip->total_length   = hton16(ip_len + udp_hdr + payload_len);
    ip->time_to_live   = kDefaultTtl;
    ip->next_proto_id  = kIpProtoUdp;
    ip->src_addr       = hton32(src_ip_host);
    ip->dst_addr       = hton32(dst_ip_host);
    ip->hdr_checksum   = internet_checksum(ip, ip_len);

    // UDP header
    auto* udp = reinterpret_cast<UdpHeader*>(buf + eth_len + ip_len);
    udp->src_port = hton16(src_port);
    udp->dst_port = hton16(dst_port);
    udp->length   = hton16(udp_hdr + payload_len);
    udp->checksum = 0;

    // Payload
    if (payload && payload_len > 0) {
        std::memcpy(buf + eth_len + ip_len + udp_hdr, payload, payload_len);
    }
    return total;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Multicast MAC address computation (RFC 1112)
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastMac, Rfc1112Mapping) {
    // 224.0.0.1 -> 01:00:5e:00:00:01
    auto mac = multicast_mac_from_ip(parse_ipv4("224.0.0.1"));
    EXPECT_EQ(mac.addr_bytes[0], 0x01);
    EXPECT_EQ(mac.addr_bytes[1], 0x00);
    EXPECT_EQ(mac.addr_bytes[2], 0x5e);
    EXPECT_EQ(mac.addr_bytes[3], 0x00);
    EXPECT_EQ(mac.addr_bytes[4], 0x00);
    EXPECT_EQ(mac.addr_bytes[5], 0x01);
}

TEST(MulticastMac, HighBitMasked) {
    // 239.255.255.255 -> IP = 0xEFFFFFFF
    // Low 23 bits = 0x7FFFFF -> 01:00:5e:7f:ff:ff
    auto mac = multicast_mac_from_ip(parse_ipv4("239.255.255.255"));
    EXPECT_EQ(mac.addr_bytes[0], 0x01);
    EXPECT_EQ(mac.addr_bytes[1], 0x00);
    EXPECT_EQ(mac.addr_bytes[2], 0x5e);
    EXPECT_EQ(mac.addr_bytes[3], 0x7f);
    EXPECT_EQ(mac.addr_bytes[4], 0xff);
    EXPECT_EQ(mac.addr_bytes[5], 0xff);
}

TEST(MulticastMac, NasdaqGroup) {
    // 233.54.12.111 = 0xE9360C6F
    // Low 23 bits = 0x360C6F -> 01:00:5e:36:0c:6f
    auto mac = multicast_mac_from_ip(parse_ipv4("233.54.12.111"));
    EXPECT_EQ(mac.addr_bytes[0], 0x01);
    EXPECT_EQ(mac.addr_bytes[1], 0x00);
    EXPECT_EQ(mac.addr_bytes[2], 0x5e);
    EXPECT_EQ(mac.addr_bytes[3], 0x36);
    EXPECT_EQ(mac.addr_bytes[4], 0x0c);
    EXPECT_EQ(mac.addr_bytes[5], 0x6f);
}

TEST(MulticastMac, Bit24Collision) {
    // Two IPs that differ only in bit 23 (masked out) map to the same MAC
    auto mac1 = multicast_mac_from_ip(parse_ipv4("224.1.0.1"));
    auto mac2 = multicast_mac_from_ip(parse_ipv4("224.129.0.1"));
    EXPECT_EQ(std::memcmp(mac1.addr_bytes, mac2.addr_bytes, 6), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// is_multicast_ip
// ─────────────────────────────────────────────────────────────────────────────

TEST(IsMulticast, ClassDRange) {
    EXPECT_TRUE(is_multicast_ip(parse_ipv4("224.0.0.0")));
    EXPECT_TRUE(is_multicast_ip(parse_ipv4("239.255.255.255")));
    EXPECT_TRUE(is_multicast_ip(parse_ipv4("233.54.12.111")));
}

TEST(IsMulticast, NonMulticast) {
    EXPECT_FALSE(is_multicast_ip(parse_ipv4("10.0.0.1")));
    EXPECT_FALSE(is_multicast_ip(parse_ipv4("192.168.1.1")));
    EXPECT_FALSE(is_multicast_ip(parse_ipv4("255.255.255.255")));
    EXPECT_FALSE(is_multicast_ip(0));
}

// ─────────────────────────────────────────────────────────────────────────────
// MulticastConfig validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastConfig, DefaultIsValid) {
    MulticastConfig cfg{};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(MulticastConfig, InvalidBurstSizeZero) {
    MulticastConfig cfg{};
    cfg.rx_burst = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
}

TEST(MulticastConfig, InvalidBurstSizeTooLarge) {
    MulticastConfig cfg{};
    cfg.rx_burst = 1024;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// MulticastGroup validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastGroup, ValidGroupIp) {
    MulticastGroup grp{
        .group_ip = parse_ipv4("233.54.12.111"),
        .group_port = 26477
    };
    auto err = grp.validate();
    EXPECT_TRUE(err.empty());
}

TEST(MulticastGroup, ZeroGroupIpInvalid) {
    MulticastGroup grp{.group_ip = 0, .group_port = 5000};
    auto err = grp.validate();
    EXPECT_FALSE(err.empty());
}

TEST(MulticastGroup, UnicastIpInvalid) {
    MulticastGroup grp{
        .group_ip = parse_ipv4("10.0.0.1"),
        .group_port = 5000
    };
    auto err = grp.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("multicast"), std::string_view::npos);
}

TEST(MulticastGroup, ZeroPortInvalid) {
    MulticastGroup grp{
        .group_ip = parse_ipv4("224.0.0.1"),
        .group_port = 0
    };
    auto err = grp.validate();
    EXPECT_FALSE(err.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// UDP packet parsing
// ─────────────────────────────────────────────────────────────────────────────

TEST(ParseUdpPacket, ValidPacket) {
    FakeUdpMbuf fake;
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    size_t len = build_fake_udp_packet(
        fake.data(), sizeof(fake.buf),
        parse_ipv4("10.1.2.3"), 5000,
        parse_ipv4("233.54.12.111"), 26477,
        payload, sizeof(payload));
    ASSERT_GT(len, 0u);
    fake.set_len(static_cast<uint16_t>(len));

    auto parsed = parse_udp_packet(&fake.mbuf);
    ASSERT_TRUE(static_cast<bool>(parsed));
    EXPECT_EQ(parsed.src_ip(), parse_ipv4("10.1.2.3"));
    EXPECT_EQ(parsed.dst_ip(), parse_ipv4("233.54.12.111"));
    EXPECT_EQ(parsed.src_port(), 5000);
    EXPECT_EQ(parsed.dst_port(), 26477);
    EXPECT_EQ(parsed.payload_len, sizeof(payload));
    EXPECT_EQ(std::memcmp(parsed.payload, payload, sizeof(payload)), 0);
}

TEST(ParseUdpPacket, TruncatedEthernet) {
    FakeUdpMbuf fake;
    fake.set_len(10); // Less than Ethernet header
    auto parsed = parse_udp_packet(&fake.mbuf);
    EXPECT_FALSE(static_cast<bool>(parsed));
}

TEST(ParseUdpPacket, TruncatedIpHeader) {
    FakeUdpMbuf fake;
    auto* eth = reinterpret_cast<rte_ether_hdr*>(fake.data());
    eth->ether_type = hton16(kEtherTypeIpv4);
    fake.set_len(kEtherHeaderLen + 5);
    auto parsed = parse_udp_packet(&fake.mbuf);
    EXPECT_FALSE(static_cast<bool>(parsed));
}

TEST(ParseUdpPacket, NonIpv4Rejected) {
    FakeUdpMbuf fake;
    auto* eth = reinterpret_cast<rte_ether_hdr*>(fake.data());
    eth->ether_type = hton16(0x86DD); // IPv6
    fake.set_len(60);
    auto parsed = parse_udp_packet(&fake.mbuf);
    EXPECT_FALSE(static_cast<bool>(parsed));
}

TEST(ParseUdpPacket, NonUdpProtocolRejected) {
    FakeUdpMbuf fake;
    auto* eth = reinterpret_cast<rte_ether_hdr*>(fake.data());
    eth->ether_type = hton16(kEtherTypeIpv4);
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(fake.data() + kEtherHeaderLen);
    ip->version_ihl    = kIpv4VersionIhl5;
    ip->next_proto_id  = kIpProtoTcp; // TCP, not UDP
    ip->total_length   = hton16(40);
    fake.set_len(kEtherHeaderLen + 40);
    auto parsed = parse_udp_packet(&fake.mbuf);
    EXPECT_FALSE(static_cast<bool>(parsed));
}

TEST(ParseUdpPacket, EmptyPayload) {
    FakeUdpMbuf fake;
    size_t len = build_fake_udp_packet(
        fake.data(), sizeof(fake.buf),
        parse_ipv4("10.0.0.1"), 1234,
        parse_ipv4("224.0.0.1"), 5678,
        nullptr, 0);
    ASSERT_GT(len, 0u);
    fake.set_len(static_cast<uint16_t>(len));

    auto parsed = parse_udp_packet(&fake.mbuf);
    ASSERT_TRUE(static_cast<bool>(parsed));
    EXPECT_EQ(parsed.payload_len, 0);
}

TEST(ParseUdpPacket, NullMbufRejected) {
    auto parsed = parse_udp_packet(nullptr);
    EXPECT_FALSE(static_cast<bool>(parsed));
}

// ─────────────────────────────────────────────────────────────────────────────
// MoldUDP64 adapter
// ─────────────────────────────────────────────────────────────────────────────

TEST(MoldUDP64Adapter, CallbackInvoked) {
    // Build a minimal MoldUDP64 packet: 20-byte header + one 2-byte message
    // Header: session(10) + seq(8) + count(2)
    // Message: length(2) + data(N)
    uint8_t mold_packet[24];
    std::memset(mold_packet, ' ', 10);    // session = "          "
    std::memset(mold_packet + 10, 0, 8);  // sequence = 0...01
    mold_packet[17] = 1;
    mold_packet[18] = 0;                  // count = 1
    mold_packet[19] = 1;
    mold_packet[20] = 0;                  // msg length = 2
    mold_packet[21] = 2;
    mold_packet[22] = 0xAB;              // msg data
    mold_packet[23] = 0xCD;

    int callback_count = 0;
    auto adapter = make_moldudp64_adapter(
        [](const uint8_t* data, size_t len, auto& msg_cb) {
            // Minimal MoldUDP64 parse: skip 20-byte header, iterate messages
            if (len < 20) return;
            uint16_t count = (static_cast<uint16_t>(data[18]) << 8) | data[19];
            size_t offset = 20;
            for (uint16_t i = 0; i < count && offset + 2 <= len; ++i) {
                uint16_t msg_len = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
                offset += 2;
                if (offset + msg_len > len) break;
                msg_cb(data + offset, msg_len, i + 1);
                offset += msg_len;
            }
        },
        [&callback_count](const uint8_t* msg, uint16_t msg_len, uint64_t /*seq*/) {
            EXPECT_EQ(msg_len, 2);
            EXPECT_EQ(msg[0], 0xAB);
            EXPECT_EQ(msg[1], 0xCD);
            ++callback_count;
        });

    adapter(mold_packet, sizeof(mold_packet));
    EXPECT_EQ(callback_count, 1);
}

TEST(MoldUDP64Adapter, EmptyPacketNoCallback) {
    int callback_count = 0;
    auto adapter = make_moldudp64_adapter(
        [](const uint8_t* /*data*/, size_t len, auto& /*msg_cb*/) {
            // Parse nothing for packets too short
            if (len < 20) return;
        },
        [&callback_count](const uint8_t*, uint16_t, uint64_t) {
            ++callback_count;
        });

    // Packet too short for MoldUDP64 header
    uint8_t data[5] = {};
    adapter(data, sizeof(data));
    EXPECT_EQ(callback_count, 0);
}
