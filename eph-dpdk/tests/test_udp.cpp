/// @file test_udp.cpp
/// Unit tests for UDP support: UdpPacketTemplate, udp_checksum, UdpConfig,
/// and build_udp_packet. Does NOT require DPDK EAL — uses fake mbufs.

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "eph/dpdk/udp.hpp"

using namespace eph::dpdk;
using namespace eph::dpdk::net;

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time validation
// ─────────────────────────────────────────────────────────────────────────────

static_assert(kUdpAllHeadersLen == 42);
static_assert(kUdpAllHeadersLen == kEtherHeaderLen + kIpv4HeaderLen + kUdpHeaderLen);

// ─────────────────────────────────────────────────────────────────────────────
// Test helpers — fake mbuf/mempool for unit testing without EAL
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Fake mbuf with inline data buffer, no EAL required.
/// Compatible with rte_pktmbuf_reset/append (respects data_off headroom).
struct FakeMbuf {
    alignas(64) uint8_t buf[2048]{};
    rte_mbuf mbuf{};

    FakeMbuf() {
        std::memset(&mbuf, 0, sizeof(mbuf));
        mbuf.buf_addr = buf;
        mbuf.buf_len = sizeof(buf);
        mbuf.data_off = 0;
        mbuf.data_len = 0;
        mbuf.pkt_len = 0;
    }

    /// Get pointer to packet data (accounts for data_off set by rte_pktmbuf_reset).
    uint8_t* data() { return rte_pktmbuf_mtod(&mbuf, uint8_t*); }
    const uint8_t* data() const { return rte_pktmbuf_mtod(&mbuf, const uint8_t*); }
};

constexpr uint32_t kTestSrcIp = 0x0A000001;  // 10.0.0.1
constexpr uint32_t kTestDstIp = 0x0A000002;  // 10.0.0.2
constexpr uint16_t kTestSrcPort = 50000;
constexpr uint16_t kTestDstPort = 8080;

rte_ether_addr make_mac(uint8_t last) {
    rte_ether_addr mac{};
    mac.addr_bytes[5] = last;
    return mac;
}

UdpPacketTemplate make_template(bool hw_cksum = false) {
    UdpPacketTemplate tmpl;
    tmpl.init(make_mac(0x01), make_mac(0x02),
              kTestSrcIp, kTestDstIp,
              kTestSrcPort, kTestDstPort,
              hw_cksum);
    return tmpl;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// udp_checksum
// ─────────────────────────────────────────────────────────────────────────────

TEST(UdpChecksum, ZeroPayload) {
    // UDP header only, no payload
    uint8_t udp_seg[8]{};
    auto* udp = reinterpret_cast<UdpHeader*>(udp_seg);
    udp->src_port = hton16(kTestSrcPort);
    udp->dst_port = hton16(kTestDstPort);
    udp->length = hton16(8);
    udp->checksum = 0;

    uint32_t src_net = hton32(kTestSrcIp);
    uint32_t dst_net = hton32(kTestDstIp);
    uint16_t cksum = udp_checksum(src_net, dst_net, udp_seg, 8);
    EXPECT_NE(cksum, 0u);  // RFC 768: if computed checksum is 0, transmit 0xFFFF
}

TEST(UdpChecksum, SelfConsistency) {
    // Build a UDP segment, compute checksum, embed it, verify full checksum == 0
    uint8_t udp_seg[16]{};
    auto* udp = reinterpret_cast<UdpHeader*>(udp_seg);
    udp->src_port = hton16(kTestSrcPort);
    udp->dst_port = hton16(kTestDstPort);
    udp->length = hton16(16);
    udp->checksum = 0;
    // 8 bytes of payload
    std::memset(udp_seg + 8, 0xAB, 8);

    uint32_t src_net = hton32(kTestSrcIp);
    uint32_t dst_net = hton32(kTestDstIp);
    uint16_t cksum = udp_checksum(src_net, dst_net, udp_seg, 16);

    // Embed the checksum and re-verify
    udp->checksum = cksum;
    uint32_t sum = pseudo_header_sum(src_net, dst_net, kIpProtoUdp, 16);
    const uint8_t* ptr = udp_seg;
    size_t len = 16;
    while (len > 1) {
        uint16_t word;
        std::memcpy(&word, ptr, 2);
        sum += word;
        ptr += 2;
        len -= 2;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    EXPECT_EQ(static_cast<uint16_t>(~sum), 0u);
}

TEST(UdpChecksum, OddLengthPayload) {
    // 8-byte header + 3-byte payload = 11 bytes (odd)
    uint8_t udp_seg[11]{};
    auto* udp = reinterpret_cast<UdpHeader*>(udp_seg);
    udp->src_port = hton16(1234);
    udp->dst_port = hton16(5678);
    udp->length = hton16(11);
    udp->checksum = 0;
    udp_seg[8] = 0xDE;
    udp_seg[9] = 0xAD;
    udp_seg[10] = 0xBE;

    uint32_t src_net = hton32(0xC0A80001);
    uint32_t dst_net = hton32(0xC0A80002);
    uint16_t cksum = udp_checksum(src_net, dst_net, udp_seg, 11);
    EXPECT_NE(cksum, 0u);

    // Verify self-consistency after embedding
    udp->checksum = cksum;
    uint32_t sum = pseudo_header_sum(src_net, dst_net, kIpProtoUdp, 11);
    const uint8_t* p = udp_seg;
    size_t l = 11;
    while (l > 1) {
        uint16_t word;
        std::memcpy(&word, p, 2);
        sum += word;
        p += 2;
        l -= 2;
    }
    if (l == 1) {
        uint16_t word = 0;
        std::memcpy(&word, p, 1);
        sum += word;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    EXPECT_EQ(static_cast<uint16_t>(~sum), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// UdpPacketTemplate::init
// ─────────────────────────────────────────────────────────────────────────────

TEST(UdpPacketTemplate, InitSetsCorrectHeaders) {
    auto tmpl = make_template();

    // Verify Ethernet header
    auto* eth = reinterpret_cast<const rte_ether_hdr*>(tmpl.header_);
    EXPECT_EQ(eth->ether_type, hton16(kEtherTypeIpv4));
    EXPECT_EQ(eth->src_addr.addr_bytes[5], 0x01);
    EXPECT_EQ(eth->dst_addr.addr_bytes[5], 0x02);

    // Verify IPv4 header
    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(tmpl.header_ + kEtherHeaderLen);
    EXPECT_EQ(ip->version_ihl, kIpv4VersionIhl5);
    EXPECT_EQ(ip->time_to_live, kDefaultTtl);
    EXPECT_EQ(ip->next_proto_id, kIpProtoUdp);
    EXPECT_EQ(ntoh32(ip->src_addr), kTestSrcIp);
    EXPECT_EQ(ntoh32(ip->dst_addr), kTestDstIp);
    EXPECT_EQ(ip->fragment_offset, hton16(kIpDontFragment));

    // Verify UDP header
    auto* udp = reinterpret_cast<const UdpHeader*>(
        tmpl.header_ + kEtherHeaderLen + kIpv4HeaderLen);
    EXPECT_EQ(ntoh16(udp->src_port), kTestSrcPort);
    EXPECT_EQ(ntoh16(udp->dst_port), kTestDstPort);
}

TEST(UdpPacketTemplate, InitDynamicFieldsAreZero) {
    auto tmpl = make_template();
    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(tmpl.header_ + kEtherHeaderLen);
    auto* udp = reinterpret_cast<const UdpHeader*>(
        tmpl.header_ + kEtherHeaderLen + kIpv4HeaderLen);

    // Dynamic fields should be zero (placeholder), updated per-packet in fill()
    EXPECT_EQ(ip->total_length, 0);
    EXPECT_EQ(ip->packet_id, 0);
    EXPECT_EQ(udp->length, 0);
}

TEST(UdpPacketTemplate, HwCksumFlagStored) {
    auto tmpl_sw = make_template(false);
    EXPECT_FALSE(tmpl_sw.hw_cksum_);

    auto tmpl_hw = make_template(true);
    EXPECT_TRUE(tmpl_hw.hw_cksum_);
}

// ─────────────────────────────────────────────────────────────────────────────
// UdpPacketTemplate::fill
// ─────────────────────────────────────────────────────────────────────────────

TEST(UdpPacketTemplate, FillCorrectHeaders) {
    auto tmpl = make_template();
    FakeMbuf fake;

    const char payload[] = "Hello UDP";
    uint16_t plen = sizeof(payload) - 1;  // exclude null terminator

    uint16_t written = tmpl.fill(&fake.mbuf, payload, plen);
    EXPECT_EQ(written, kUdpAllHeadersLen + plen);
    EXPECT_EQ(fake.mbuf.data_len, written);
    EXPECT_EQ(fake.mbuf.pkt_len, written);

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(fake.data() + kEtherHeaderLen);
    EXPECT_EQ(ntoh16(ip->total_length), kIpv4HeaderLen + kUdpHeaderLen + plen);
    EXPECT_EQ(ip->next_proto_id, kIpProtoUdp);

    auto* udp = reinterpret_cast<const UdpHeader*>(
        fake.data() + kEtherHeaderLen + kIpv4HeaderLen);
    EXPECT_EQ(ntoh16(udp->length), kUdpHeaderLen + plen);
    EXPECT_EQ(ntoh16(udp->src_port), kTestSrcPort);
    EXPECT_EQ(ntoh16(udp->dst_port), kTestDstPort);
}

TEST(UdpPacketTemplate, FillPayloadCopied) {
    auto tmpl = make_template();
    FakeMbuf fake;

    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    tmpl.fill(&fake.mbuf, payload, sizeof(payload));

    const uint8_t* pkt_payload = fake.data() + kUdpAllHeadersLen;
    EXPECT_EQ(std::memcmp(pkt_payload, payload, sizeof(payload)), 0);
}

TEST(UdpPacketTemplate, FillIpIdIncrements) {
    auto tmpl = make_template();

    for (uint16_t i = 0; i < 5; ++i) {
        FakeMbuf fake;
        tmpl.fill(&fake.mbuf, nullptr, 0);

        auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(fake.data() + kEtherHeaderLen);
        EXPECT_EQ(ntoh16(ip->packet_id), i) << "Iteration " << i;
    }
}

TEST(UdpPacketTemplate, FillNullMbufReturnsZero) {
    auto tmpl = make_template();
    EXPECT_EQ(tmpl.fill(nullptr, nullptr, 0), 0u);
}

TEST(UdpPacketTemplate, FillZeroPayload) {
    auto tmpl = make_template();
    FakeMbuf fake;

    uint16_t written = tmpl.fill(&fake.mbuf, nullptr, 0);
    EXPECT_EQ(written, kUdpAllHeadersLen);

    auto* udp = reinterpret_cast<const UdpHeader*>(
        fake.data() + kEtherHeaderLen + kIpv4HeaderLen);
    EXPECT_EQ(ntoh16(udp->length), kUdpHeaderLen);
}

TEST(UdpPacketTemplate, FillSwCksumUdpChecksumZero) {
    auto tmpl = make_template(false);
    FakeMbuf fake;

    const uint8_t payload[] = {0x01, 0x02, 0x03};
    tmpl.fill(&fake.mbuf, payload, sizeof(payload));

    auto* udp = reinterpret_cast<const UdpHeader*>(
        fake.data() + kEtherHeaderLen + kIpv4HeaderLen);
    // Software mode: UDP checksum = 0 (optional for IPv4)
    EXPECT_EQ(udp->checksum, 0u);
    // ol_flags should be clear
    EXPECT_EQ(fake.mbuf.ol_flags, 0u);
}

TEST(UdpPacketTemplate, FillSwCksumIpChecksumValid) {
    auto tmpl = make_template(false);
    FakeMbuf fake;

    const uint8_t payload[] = {0x01, 0x02, 0x03};
    tmpl.fill(&fake.mbuf, payload, sizeof(payload));

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(fake.data() + kEtherHeaderLen);
    // Verify IP checksum by recomputing — should be zero when including stored checksum
    uint16_t verify = internet_checksum(ip, kIpv4HeaderLen);
    EXPECT_EQ(verify, 0u);
}

TEST(UdpPacketTemplate, FillHwCksumOffloadFlags) {
    auto tmpl = make_template(true);
    FakeMbuf fake;

    const uint8_t payload[] = {0x01, 0x02};
    tmpl.fill(&fake.mbuf, payload, sizeof(payload));

    // HW offload flags set
    EXPECT_TRUE(fake.mbuf.ol_flags & RTE_MBUF_F_TX_IP_CKSUM);
    EXPECT_TRUE(fake.mbuf.ol_flags & RTE_MBUF_F_TX_UDP_CKSUM);
    EXPECT_EQ(fake.mbuf.l2_len, kEtherHeaderLen);
    EXPECT_EQ(fake.mbuf.l3_len, kIpv4HeaderLen);
    EXPECT_EQ(fake.mbuf.l4_len, kUdpHeaderLen);

    // IP checksum field = 0 (NIC fills)
    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(fake.data() + kEtherHeaderLen);
    EXPECT_EQ(ip->hdr_checksum, 0u);

    // UDP checksum = pseudo-header checksum (NIC completes)
    auto* udp = reinterpret_cast<const UdpHeader*>(
        fake.data() + kEtherHeaderLen + kIpv4HeaderLen);
    EXPECT_NE(udp->checksum, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// UdpPacketTemplate::build
// ─────────────────────────────────────────────────────────────────────────────

TEST(UdpPacketTemplate, BuildNullPoolReturnsNull) {
    auto tmpl = make_template();
    EXPECT_EQ(tmpl.build(nullptr, nullptr, 0), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// UdpPacketTemplate::dump
// ─────────────────────────────────────────────────────────────────────────────

TEST(UdpPacketTemplate, DumpContainsAddresses) {
    auto tmpl = make_template();
    auto s = tmpl.dump();
    EXPECT_NE(s.find("10.0.0.1"), std::string::npos);
    EXPECT_NE(s.find("10.0.0.2"), std::string::npos);
    EXPECT_NE(s.find("50000"), std::string::npos);
    EXPECT_NE(s.find("8080"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// UdpPacketTemplate — ip_id wraparound
// ─────────────────────────────────────────────────────────────────────────────

TEST(UdpPacketTemplate, IpIdWrapsAroundAt65535) {
    auto tmpl = make_template();
    tmpl.ip_id_ = 65534;  // Start near wraparound

    FakeMbuf fake1;
    tmpl.fill(&fake1.mbuf, nullptr, 0);
    auto* ip1 = reinterpret_cast<const rte_ipv4_hdr*>(fake1.data() + kEtherHeaderLen);
    EXPECT_EQ(ntoh16(ip1->packet_id), 65534u);

    FakeMbuf fake2;
    tmpl.fill(&fake2.mbuf, nullptr, 0);
    auto* ip2 = reinterpret_cast<const rte_ipv4_hdr*>(fake2.data() + kEtherHeaderLen);
    EXPECT_EQ(ntoh16(ip2->packet_id), 65535u);

    // Next packet wraps to 0
    FakeMbuf fake3;
    tmpl.fill(&fake3.mbuf, nullptr, 0);
    auto* ip3 = reinterpret_cast<const rte_ipv4_hdr*>(fake3.data() + kEtherHeaderLen);
    EXPECT_EQ(ntoh16(ip3->packet_id), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// UdpConfig::validate
// ─────────────────────────────────────────────────────────────────────────────

TEST(UdpConfig, ValidateSuccess) {
    // pool is a non-null pointer for validation (not dereferenced)
    rte_mempool fake_pool{};
    UdpConfig cfg{
        .src_ip = kTestSrcIp, .dst_ip = kTestDstIp,
        .src_port = kTestSrcPort, .dst_port = kTestDstPort,
        .pool = &fake_pool,
    };
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(UdpConfig, ValidateZeroSrcIpFails) {
    rte_mempool fake_pool{};
    UdpConfig cfg{
        .src_ip = 0, .dst_ip = kTestDstIp,
        .src_port = kTestSrcPort, .dst_port = kTestDstPort,
        .pool = &fake_pool,
    };
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("src_ip"), std::string_view::npos);
}

TEST(UdpConfig, ValidateZeroDstIpFails) {
    rte_mempool fake_pool{};
    UdpConfig cfg{
        .src_ip = kTestSrcIp, .dst_ip = 0,
        .src_port = kTestSrcPort, .dst_port = kTestDstPort,
        .pool = &fake_pool,
    };
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(UdpConfig, ValidateZeroSrcPortFails) {
    rte_mempool fake_pool{};
    UdpConfig cfg{
        .src_ip = kTestSrcIp, .dst_ip = kTestDstIp,
        .src_port = 0, .dst_port = kTestDstPort,
        .pool = &fake_pool,
    };
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(UdpConfig, ValidateZeroDstPortFails) {
    rte_mempool fake_pool{};
    UdpConfig cfg{
        .src_ip = kTestSrcIp, .dst_ip = kTestDstIp,
        .src_port = kTestSrcPort, .dst_port = 0,
        .pool = &fake_pool,
    };
    EXPECT_FALSE(cfg.validate().empty());
}

TEST(UdpConfig, ValidateNullPoolFails) {
    UdpConfig cfg{
        .src_ip = kTestSrcIp, .dst_ip = kTestDstIp,
        .src_port = kTestSrcPort, .dst_port = kTestDstPort,
        .pool = nullptr,
    };
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("pool"), std::string_view::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// UdpConfig::dump / to_json
// ─────────────────────────────────────────────────────────────────────────────

TEST(UdpConfig, DumpContainsFields) {
    rte_mempool fake_pool{};
    UdpConfig cfg{
        .src_ip = kTestSrcIp, .dst_ip = kTestDstIp,
        .src_port = kTestSrcPort, .dst_port = kTestDstPort,
        .port_id = 1, .tx_queue_id = 3,
        .pool = &fake_pool, .hw_cksum = true,
    };
    auto s = cfg.dump();
    EXPECT_NE(s.find("10.0.0.1"), std::string::npos) << s;
    EXPECT_NE(s.find("10.0.0.2"), std::string::npos) << s;
    EXPECT_NE(s.find("50000"), std::string::npos) << s;
    EXPECT_NE(s.find("8080"), std::string::npos) << s;
    EXPECT_NE(s.find("hw_cksum=true"), std::string::npos) << s;
}

TEST(UdpConfig, ToJsonFormat) {
    rte_mempool fake_pool{};
    UdpConfig cfg{
        .src_ip = kTestSrcIp, .dst_ip = kTestDstIp,
        .src_port = kTestSrcPort, .dst_port = kTestDstPort,
        .port_id = 0, .tx_queue_id = 0,
        .pool = &fake_pool, .hw_cksum = false,
    };
    auto json = cfg.to_json();
    EXPECT_NE(json.find("\"src_port\":50000"), std::string::npos) << json;
    EXPECT_NE(json.find("\"dst_port\":8080"), std::string::npos) << json;
    EXPECT_NE(json.find("\"hw_cksum\":false"), std::string::npos) << json;
}

// ─────────────────────────────────────────────────────────────────────────────
// build_udp_packet convenience function
// ─────────────────────────────────────────────────────────────────────────────

TEST(BuildUdpPacket, NullPoolReturnsNull) {
    auto src = make_mac(0x01);
    auto dst = make_mac(0x02);
    EXPECT_EQ(build_udp_packet(nullptr, src, dst,
                               kTestSrcIp, kTestDstIp,
                               kTestSrcPort, kTestDstPort,
                               nullptr, 0), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// UdpSenderStats
// ─────────────────────────────────────────────────────────────────────────────

TEST(UdpSenderStats, DefaultsZero) {
    UdpSenderStats stats;
    EXPECT_EQ(stats.tx_packets, 0u);
    EXPECT_EQ(stats.tx_bytes, 0u);
    EXPECT_EQ(stats.tx_errors, 0u);
}

TEST(UdpSenderStats, ToJsonFormat) {
    UdpSenderStats stats{.tx_packets = 100, .tx_bytes = 5000, .tx_errors = 2};
    auto json = stats.to_json();
    EXPECT_NE(json.find("\"tx_packets\":100"), std::string::npos);
    EXPECT_NE(json.find("\"tx_bytes\":5000"), std::string::npos);
    EXPECT_NE(json.find("\"tx_errors\":2"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// UdpSegment
// ─────────────────────────────────────────────────────────────────────────────

TEST(UdpSegment, DefaultsNull) {
    UdpSegment seg;
    EXPECT_EQ(seg.data, nullptr);
    EXPECT_EQ(seg.len, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// FlowProtocol (from flow_steering.hpp)
// ─────────────────────────────────────────────────────────────────────────────

#include "eph/dpdk/flow_steering.hpp"

TEST(FlowProtocol, NameTcp) {
    EXPECT_EQ(flow_protocol_name(FlowProtocol::Tcp), "TCP");
}

TEST(FlowProtocol, NameUdp) {
    EXPECT_EQ(flow_protocol_name(FlowProtocol::Udp), "UDP");
}

TEST(FlowProtocol, EnumValuesDistinct) {
    EXPECT_NE(static_cast<uint8_t>(FlowProtocol::Tcp),
              static_cast<uint8_t>(FlowProtocol::Udp));
}

TEST(FlowProtocol, FormatTcp) {
    auto s = std::format("{}", FlowProtocol::Tcp);
    EXPECT_EQ(s, "TCP");
}

TEST(FlowProtocol, FormatUdp) {
    auto s = std::format("{}", FlowProtocol::Udp);
    EXPECT_EQ(s, "UDP");
}
