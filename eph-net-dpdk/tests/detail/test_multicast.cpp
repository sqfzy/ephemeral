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

TEST(IsMulticast, BoundaryJustBelowMulticast) {
    // 223.255.255.255 is the last Class C address, just below multicast range
    EXPECT_FALSE(is_multicast_ip(parse_ipv4("223.255.255.255")));
}

TEST(IsMulticast, BoundaryJustAboveMulticast) {
    // 240.0.0.0 is the first Class E address, just above multicast range
    EXPECT_FALSE(is_multicast_ip(parse_ipv4("240.0.0.0")));
}

TEST(IsMulticast, ConstexprEvaluation) {
    // Verify is_multicast_ip works at compile time
    static_assert(is_multicast_ip(0xE0000001));   // 224.0.0.1
    static_assert(!is_multicast_ip(0xC0A80101));  // 192.168.1.1
    static_assert(!is_multicast_ip(0));
    static_assert(!is_multicast_ip(0xF0000000));  // 240.0.0.0 (class E)
}

TEST(MulticastMac, ConstexprEvaluation) {
    // Verify multicast_mac_from_ip works at compile time
    constexpr auto mac = multicast_mac_from_ip(0xE9FF6F6F); // 233.255.111.111
    static_assert(mac.addr_bytes[0] == 0x01);
    static_assert(mac.addr_bytes[1] == 0x00);
    static_assert(mac.addr_bytes[2] == 0x5e);
    // Low 23 bits of 233.255.111.111: bits 22..16 = 0x7F & 0x7F = 0x7F,
    // bits 15..8 = 0x6F, bits 7..0 = 0x6F
    static_assert(mac.addr_bytes[3] == 0x7F);
    static_assert(mac.addr_bytes[4] == 0x6F);
    static_assert(mac.addr_bytes[5] == 0x6F);
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

// SSM filter must be a unicast source — multicast / broadcast values can
// never appear as a real datagram's source IP, so the receiver would
// silently drop every packet. validate() catches the misconfig early.
TEST(MulticastGroup, SourceIpMulticastInvalid) {
    MulticastGroup grp{
        .group_ip   = parse_ipv4("233.54.12.111"),
        .group_port = 26477,
        .source_ip  = parse_ipv4("239.1.2.3"), // also multicast
    };
    auto err = grp.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("unicast"), std::string_view::npos);
}

TEST(MulticastGroup, SourceIpBroadcastInvalid) {
    MulticastGroup grp{
        .group_ip   = parse_ipv4("233.54.12.111"),
        .group_port = 26477,
        .source_ip  = 0xFFFFFFFFu, // 255.255.255.255
    };
    auto err = grp.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("unicast"), std::string_view::npos);
}

TEST(MulticastGroup, SourceIpUnicastValid) {
    MulticastGroup grp{
        .group_ip   = parse_ipv4("233.54.12.111"),
        .group_port = 26477,
        .source_ip  = parse_ipv4("10.0.0.1"),
    };
    auto err = grp.validate();
    EXPECT_TRUE(err.empty());
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
// ParsedUdpPacket::dump
// ─────────────────────────────────────────────────────────────────────────────

TEST(ParsedUdpPacket, DumpInvalidPacket) {
    ParsedUdpPacket empty{};
    EXPECT_EQ(empty.dump(), "(invalid)");
}

TEST(ParsedUdpPacket, DumpShowsAddressesAndPayloadLen) {
    uint8_t payload[] = {0xDE, 0xAD};
    FakeUdpMbuf fake;
    auto len = build_fake_udp_packet(
        fake.data(), sizeof(fake.buf),
        parse_ipv4("10.1.2.3"), 5000,
        parse_ipv4("233.54.12.111"), 26477,
        payload, sizeof(payload));
    ASSERT_GT(len, 0u);
    fake.set_len(static_cast<uint16_t>(len));

    auto parsed = parse_udp_packet(&fake.mbuf);
    ASSERT_TRUE(static_cast<bool>(parsed));
    auto d = parsed.dump();
    EXPECT_NE(d.find("UDP"), std::string::npos);
    EXPECT_NE(d.find("10.1.2.3"), std::string::npos);
    EXPECT_NE(d.find("5000"), std::string::npos);
    EXPECT_NE(d.find("26477"), std::string::npos);
    EXPECT_NE(d.find("payload=2B"), std::string::npos);
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

// ─────────────────────────────────────────────────────────────────────────────
// MulticastGroup dump/to_json
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastGroup, DumpContainsIpAndPort) {
    MulticastGroup grp{
        .group_ip = parse_ipv4("233.54.12.111"),
        .group_port = 26477};
    auto d = grp.dump();
    EXPECT_NE(d.find("233.54.12.111"), std::string::npos);
    EXPECT_NE(d.find("26477"), std::string::npos);
    EXPECT_NE(d.find("any"), std::string::npos);  // source_ip == 0
}

TEST(MulticastGroup, DumpWithSourceFilter) {
    MulticastGroup grp{
        .group_ip = parse_ipv4("233.54.12.111"),
        .group_port = 26477,
        .source_ip = parse_ipv4("10.0.0.1")};
    auto d = grp.dump();
    EXPECT_NE(d.find("10.0.0.1"), std::string::npos);
}

TEST(MulticastGroup, ToJsonFormat) {
    MulticastGroup grp{
        .group_ip = parse_ipv4("233.54.12.111"),
        .group_port = 26477};
    auto j = grp.to_json();
    EXPECT_NE(j.find("\"group_ip\":\"233.54.12.111\""), std::string::npos);
    EXPECT_NE(j.find("\"group_port\":26477"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// MulticastConfig dump/to_json/validate
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastConfigObservability, DumpContainsFields) {
    MulticastConfig cfg{.port_id = 1, .rx_queue_id = 2, .rx_cpu = 3, .rx_burst = 64};
    auto d = cfg.dump();
    EXPECT_NE(d.find("port_id: 1"), std::string::npos);
    EXPECT_NE(d.find("rx_queue: 2"), std::string::npos);
    EXPECT_NE(d.find("rx_cpu: 3"), std::string::npos);
    EXPECT_NE(d.find("rx_burst: 64"), std::string::npos);
    // The RSS-multi-queue safety flag must be visible to operators —
    // otherwise an `rss_active_multi_queue=true` config that fails the
    // start() gate looks identical to a sane config in logs.
    EXPECT_NE(d.find("rss_active_multi_queue"), std::string::npos);
}

TEST(MulticastConfigObservability, ToJsonFormat) {
    MulticastConfig cfg{.port_id = 0, .rx_queue_id = 0, .rx_cpu = -1, .rx_burst = 32};
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"port_id\":0"), std::string::npos);
    EXPECT_NE(j.find("\"rx_burst\":32"), std::string::npos);
    // JSON dump goes to monitoring systems — the safety flag must be
    // queryable there too. Default is `false`; we assert the literal so
    // a future field-rename (e.g. snake_case → camelCase) breaks loud.
    EXPECT_NE(j.find("\"rss_active_multi_queue\":false"), std::string::npos);
}

TEST(MulticastConfigObservability, ToJsonReflectsRssFlagWhenTrue) {
    MulticastConfig cfg{};
    cfg.rss_active_multi_queue = true;
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"rss_active_multi_queue\":true"), std::string::npos)
        << "to_json() must surface the RSS safety flag both ways: " << j;
}

TEST(MulticastConfigValidation, NegativeCpuAffinity) {
    MulticastConfig cfg{.rx_cpu = -2};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("rx_cpu"), std::string_view::npos);
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

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastGroupFormatter, ContainsKeyFields) {
    MulticastGroup g{
        .group_ip   = 0xE9360C6F, // 233.54.12.111
        .group_port = 26477,
        .source_ip  = 0,
    };
    auto s = std::format("{}", g);
    EXPECT_NE(s.find("MulticastGroup"), std::string::npos);
    EXPECT_NE(s.find("26477"), std::string::npos);
    EXPECT_NE(s.find("any"), std::string::npos);
}

TEST(MulticastGroupFormatter, SourceSpecific) {
    MulticastGroup g{
        .group_ip   = 0xE9360C6F,
        .group_port = 26477,
        .source_ip  = 0x0A000001, // 10.0.0.1
    };
    auto s = std::format("{}", g);
    EXPECT_NE(s.find("10.0.0.1"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// MulticastConfig::warnings
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastConfigWarnings, PinnedCpuNoWarnings) {
    MulticastConfig cfg{.rx_cpu = 3, .rx_burst = 32};
    auto w = cfg.warnings();
    EXPECT_TRUE(w.empty()) << "Unexpected warning: " << (w.empty() ? "" : w[0]);
}

TEST(MulticastConfigWarnings, UnpinnedCpuWarns) {
    MulticastConfig cfg{.rx_cpu = -1, .rx_burst = 32};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("no pinning") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected no-pinning warning";
}

TEST(MulticastConfigWarnings, SmallBurstWarns) {
    MulticastConfig cfg{.rx_cpu = 1, .rx_burst = 4};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("rx_burst=4") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected small burst warning";
}

TEST(MulticastConfigWarnings, LargeBurstWarns) {
    MulticastConfig cfg{.rx_cpu = 1, .rx_burst = 256};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("rx_burst=256") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected large burst warning";
}

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastConfigFormatter, ContainsKeyFields) {
    MulticastConfig cfg{
        .port_id = 1,
        .rx_queue_id = 3,
        .rx_cpu = 5,
        .rx_burst = 64,
    };
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("MulticastConfig"), std::string::npos);
    EXPECT_NE(s.find("port=1"), std::string::npos);
    EXPECT_NE(s.find("q=3"), std::string::npos);
    EXPECT_NE(s.find("cpu=5"), std::string::npos);
    EXPECT_NE(s.find("burst=64"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// MulticastConfig equality
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastConfigEquality, DefaultsAreEqual) {
    MulticastConfig a{};
    MulticastConfig b{};
    EXPECT_EQ(a, b);
}

TEST(MulticastConfigEquality, DifferentPortIdNotEqual) {
    MulticastConfig a{.port_id = 0};
    MulticastConfig b{.port_id = 1};
    EXPECT_NE(a, b);
}

TEST(MulticastConfigEquality, DifferentRxQueueNotEqual) {
    MulticastConfig a{.rx_queue_id = 0};
    MulticastConfig b{.rx_queue_id = 1};
    EXPECT_NE(a, b);
}

TEST(MulticastConfigEquality, DifferentCpuNotEqual) {
    MulticastConfig a{.rx_cpu = -1};
    MulticastConfig b{.rx_cpu = 2};
    EXPECT_NE(a, b);
}

TEST(MulticastConfigEquality, DifferentBurstNotEqual) {
    MulticastConfig a{.rx_burst = 32};
    MulticastConfig b{.rx_burst = 64};
    EXPECT_NE(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// MulticastGroup::warnings
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastGroupWarnings, NormalGroupNoWarnings) {
    MulticastGroup grp{
        .group_ip = parse_ipv4("233.54.12.111"),
        .group_port = 26477};
    auto w = grp.warnings();
    EXPECT_TRUE(w.empty()) << "Unexpected warning: " << (w.empty() ? "" : w[0]);
}

TEST(MulticastGroupWarnings, LocalControlBlockWarns) {
    // 224.0.0.1 is in the local network control block (RFC 5771)
    MulticastGroup grp{
        .group_ip = parse_ipv4("224.0.0.1"),
        .group_port = 5000};
    auto w = grp.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("local network control block") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected local control block warning for 224.0.0.1";
}

TEST(MulticastGroupWarnings, LoopbackSourceWarns) {
    MulticastGroup grp{
        .group_ip = parse_ipv4("233.54.12.111"),
        .group_port = 26477,
        .source_ip = parse_ipv4("127.0.0.1")};
    auto w = grp.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("loopback") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected loopback source warning";
}

TEST(MulticastGroupWarnings, WellKnownPortWarns) {
    MulticastGroup grp{
        .group_ip = parse_ipv4("233.54.12.111"),
        .group_port = 80};
    auto w = grp.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("well-known") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected well-known port warning for port 80";
}

// ─────────────────────────────────────────────────────────────────────────────
// MulticastReceiver — type safety and lifecycle
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastReceiver, NotCopyable) {
    static_assert(!std::is_copy_constructible_v<MulticastReceiver>);
    static_assert(!std::is_copy_assignable_v<MulticastReceiver>);
}

TEST(MulticastReceiver, NotMovable) {
    static_assert(!std::is_move_constructible_v<MulticastReceiver>);
    static_assert(!std::is_move_assignable_v<MulticastReceiver>);
}

TEST(MulticastReceiver, InitialState) {
    MulticastReceiver receiver(MulticastConfig{});
    EXPECT_EQ(receiver.group_count(), 0u);
    EXPECT_EQ(receiver.active_group_count(), 0u);
    EXPECT_FALSE(receiver.is_running());
}

TEST(MulticastReceiver, JoinInvalidGroupFails) {
    MulticastReceiver receiver(MulticastConfig{});
    // Zero group IP is invalid
    MulticastGroup bad{.group_ip = 0, .group_port = 12345};
    auto result = receiver.join_group(bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Invalid"), std::string::npos);
}

TEST(MulticastReceiver, JoinUnicastGroupFails) {
    MulticastReceiver receiver(MulticastConfig{});
    // 10.0.0.1 is not a multicast address
    MulticastGroup bad{.group_ip = parse_ipv4("10.0.0.1"), .group_port = 12345};
    auto result = receiver.join_group(bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("multicast"), std::string::npos);
}

TEST(MulticastReceiver, JoinZeroPortFails) {
    MulticastReceiver receiver(MulticastConfig{});
    MulticastGroup bad{.group_ip = parse_ipv4("233.54.12.111"), .group_port = 0};
    auto result = receiver.join_group(bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("port"), std::string::npos);
}

TEST(MulticastReceiver, StopIsIdempotent) {
    MulticastReceiver receiver(MulticastConfig{});
    receiver.stop();
    receiver.stop();  // Double stop should not crash
    EXPECT_FALSE(receiver.is_running());
}

// ─────────────────────────────────────────────────────────────────────────────
// ParsedUdpPacket::to_json
// ─────────────────────────────────────────────────────────────────────────────

TEST(ParsedUdpPacket, ToJsonInvalid) {
    ParsedUdpPacket p{};
    auto json = p.to_json();
    EXPECT_NE(json.find("\"valid\":false"), std::string::npos);
}

TEST(ParsedUdpPacket, ToJsonValidPacket) {
    FakeUdpMbuf fake;
    uint8_t payload[] = {0xDE, 0xAD};
    size_t pkt_len = build_fake_udp_packet(
        fake.data(), sizeof(fake.buf),
        parse_ipv4("10.1.2.3"), 5000,
        parse_ipv4("233.54.12.111"), 26477,
        payload, 2);
    ASSERT_GT(pkt_len, 0u);
    fake.set_len(static_cast<uint16_t>(pkt_len));

    auto parsed = parse_udp_packet(&fake.mbuf);
    ASSERT_TRUE(static_cast<bool>(parsed));

    auto json = parsed.to_json();
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"src_port\":5000"), std::string::npos);
    EXPECT_NE(json.find("\"dst_port\":26477"), std::string::npos);
    EXPECT_NE(json.find("\"payload_len\":2"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// MulticastConfig::to_json
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastConfig, ToJsonContainsAllFields) {
    MulticastConfig cfg{.port_id = 2, .rx_queue_id = 3, .rx_cpu = 5, .rx_burst = 64};
    auto json = cfg.to_json();
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"port_id\":2"), std::string::npos);
    EXPECT_NE(json.find("\"rx_queue_id\":3"), std::string::npos);
    EXPECT_NE(json.find("\"rx_cpu\":5"), std::string::npos);
    EXPECT_NE(json.find("\"rx_burst\":64"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// MulticastGroup::to_json
// ─────────────────────────────────────────────────────────────────────────────

TEST(MulticastGroup, ToJsonWithSourceFilter) {
    MulticastGroup g{
        .group_ip = parse_ipv4("233.54.12.111"),
        .group_port = 26477,
        .source_ip = parse_ipv4("10.1.2.3"),
    };
    auto json = g.to_json();
    EXPECT_NE(json.find("\"group_port\":26477"), std::string::npos);
    EXPECT_NE(json.find("\"source_ip\":\"10.1.2.3\""), std::string::npos);
}

TEST(MulticastGroup, ToJsonNoSourceFilter) {
    MulticastGroup g{
        .group_ip = parse_ipv4("233.54.12.111"),
        .group_port = 26477,
    };
    auto json = g.to_json();
    EXPECT_NE(json.find("\"source_ip\":\"0.0.0.0\""), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// ParsedUdpPacket::formatter
// ─────────────────────────────────────────────────────────────────────────────

TEST(ParsedUdpPacket, FormatterInvalidProducesOutput) {
    ParsedUdpPacket p{};
    auto s = std::format("{}", p);
    EXPECT_EQ(s, "(invalid)");
}

TEST(ParsedUdpPacket, FormatterValidProducesOutput) {
    FakeUdpMbuf fake;
    uint8_t payload[] = {0xAB};
    size_t pkt_len = build_fake_udp_packet(
        fake.data(), sizeof(fake.buf),
        parse_ipv4("10.1.2.3"), 5000,
        parse_ipv4("233.54.12.111"), 26477,
        payload, 1);
    ASSERT_GT(pkt_len, 0u);
    fake.set_len(static_cast<uint16_t>(pkt_len));
    auto parsed = parse_udp_packet(&fake.mbuf);
    auto s = std::format("{}", parsed);
    EXPECT_NE(s.find("UDP"), std::string::npos);
    EXPECT_NE(s.find("5000"), std::string::npos);
}
