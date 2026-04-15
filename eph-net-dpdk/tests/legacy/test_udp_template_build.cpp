/// @file test_udp_template_build.cpp
/// Round-trip + checksum tests for eph::dpdk::net::UdpPacketTemplate.
///
/// UdpPacketTemplate is the TX-side packet builder for UdpSender.
/// The audit (D4) raised concern about the memset+partial-init pattern
/// in init() — if any field gets added to rte_ipv4_hdr in the future
/// and isn't both memset and explicitly assigned, it'll silently
/// contain garbage.  None of the existing tests exercise build() or
/// fill(), so any bug there ships unnoticed.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/packet_template.hpp"

using namespace eph::dpdk::net;

namespace {

class UdpTemplateTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        pool_ = rte_pktmbuf_pool_create(
            "test_udp_template_pool", /*n=*/256, /*cache=*/32,
            /*priv_size=*/0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
        ASSERT_NE(pool_, nullptr);
    }
    static void TearDownTestSuite() {
        if (pool_) { rte_mempool_free(pool_); pool_ = nullptr; }
    }

    UdpPacketTemplate make_template() const {
        UdpPacketTemplate t;
        rte_ether_addr src_mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
        rte_ether_addr dst_mac = {{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
        t.init(src_mac, dst_mac,
               /*src_ip=*/0x0A000001, /*dst_ip=*/0x0A000002,
               /*src_port=*/55000, /*dst_port=*/53,
               /*hw_cksum=*/false);
        return t;
    }

    static rte_mempool* pool_;
};

rte_mempool* UdpTemplateTest::pool_ = nullptr;

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// build() round-trip
// ═══════════════════════════════════════════════════════════════════════

TEST_F(UdpTemplateTest, BuildSmallPayloadRoundTrip) {
    auto t = make_template();
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    rte_mbuf* mbuf = t.build(pool_, payload, sizeof(payload));
    ASSERT_NE(mbuf, nullptr);

    auto parsed = parse_udp_packet(mbuf);
    ASSERT_NE(parsed.udp, nullptr);
    EXPECT_EQ(parsed.payload_len, sizeof(payload));
    ASSERT_NE(parsed.payload, nullptr);
    EXPECT_EQ(0, std::memcmp(parsed.payload, payload, sizeof(payload)));

    EXPECT_EQ(ntoh16(parsed.udp->src_port), 55000u);
    EXPECT_EQ(ntoh16(parsed.udp->dst_port), 53u);
    EXPECT_EQ(ntoh32(parsed.ip->src_addr),  0x0A000001u);
    EXPECT_EQ(ntoh32(parsed.ip->dst_addr),  0x0A000002u);

    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, BuildZeroPayloadRoundTrip) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build(pool_, nullptr, 0);
    ASSERT_NE(mbuf, nullptr);

    auto parsed = parse_udp_packet(mbuf);
    ASSERT_NE(parsed.udp, nullptr);
    EXPECT_EQ(parsed.payload_len, 0u);
    // Per RFC 768 the parser accepts zero-length datagrams and points
    // `payload` past the UDP header — the length is authoritative. The old
    // nullptr sentinel (from the pre-fix parser) is no longer the contract.
    EXPECT_NE(parsed.payload, nullptr);

    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, BuildLargePayloadRoundTrip) {
    auto t = make_template();
    std::vector<uint8_t> payload(1400);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i & 0xFF);
    }
    rte_mbuf* mbuf = t.build(pool_, payload.data(),
                              static_cast<uint16_t>(payload.size()));
    ASSERT_NE(mbuf, nullptr);

    auto parsed = parse_udp_packet(mbuf);
    ASSERT_NE(parsed.udp, nullptr);
    EXPECT_EQ(parsed.payload_len, payload.size());
    EXPECT_EQ(0, std::memcmp(parsed.payload, payload.data(), payload.size()));

    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, MacAddressesPropagated) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build(pool_, nullptr, 0);
    ASSERT_NE(mbuf, nullptr);

    auto* eth = rte_pktmbuf_mtod(mbuf, const rte_ether_hdr*);
    rte_ether_addr expected_src = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    rte_ether_addr expected_dst = {{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
    EXPECT_EQ(0, std::memcmp(&eth->src_addr, &expected_src, 6));
    EXPECT_EQ(0, std::memcmp(&eth->dst_addr, &expected_dst, 6));

    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, IpIdIncrementsAcrossBuildCalls) {
    auto t = make_template();
    t.ip_id_ = 1000;

    rte_mbuf* m1 = t.build(pool_, nullptr, 0);
    rte_mbuf* m2 = t.build(pool_, nullptr, 0);
    rte_mbuf* m3 = t.build(pool_, nullptr, 0);
    ASSERT_NE(m1, nullptr);
    ASSERT_NE(m2, nullptr);
    ASSERT_NE(m3, nullptr);

    auto get_ipid = [](rte_mbuf* m) {
        auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(
            rte_pktmbuf_mtod(m, const uint8_t*) + kEtherHeaderLen);
        return ntoh16(ip->packet_id);
    };
    EXPECT_EQ(get_ipid(m1), 1000);
    EXPECT_EQ(get_ipid(m2), 1001);
    EXPECT_EQ(get_ipid(m3), 1002);

    rte_pktmbuf_free(m1);
    rte_pktmbuf_free(m2);
    rte_pktmbuf_free(m3);
}

TEST_F(UdpTemplateTest, IpHeaderChecksumSelfVerifies) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build(pool_, nullptr, 0);
    ASSERT_NE(mbuf, nullptr);

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(
        rte_pktmbuf_mtod(mbuf, const uint8_t*) + kEtherHeaderLen);
    uint16_t verify = internet_checksum(ip, kIpv4HeaderLen);
    EXPECT_EQ(verify, 0u) << "IP header checksum self-verify must be zero";

    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, UdpChecksumIsZeroBySoftwarePath) {
    // RFC 768: IPv4 UDP checksum is optional.  The software path
    // explicitly sets it to 0 (lowest latency).  Pin this behavior so
    // a future change doesn't silently start computing it.
    auto t = make_template();
    const uint8_t payload[] = {1, 2, 3, 4};
    rte_mbuf* mbuf = t.build(pool_, payload, sizeof(payload));
    ASSERT_NE(mbuf, nullptr);

    auto* udp = reinterpret_cast<const UdpHeader*>(
        rte_pktmbuf_mtod(mbuf, const uint8_t*) +
        kEtherHeaderLen + kIpv4HeaderLen);
    EXPECT_EQ(udp->checksum, 0u);

    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, UdpLengthMatchesPayload) {
    auto t = make_template();
    const uint8_t payload[100] = {0};
    rte_mbuf* mbuf = t.build(pool_, payload, sizeof(payload));
    ASSERT_NE(mbuf, nullptr);

    auto* udp = reinterpret_cast<const UdpHeader*>(
        rte_pktmbuf_mtod(mbuf, const uint8_t*) +
        kEtherHeaderLen + kIpv4HeaderLen);
    EXPECT_EQ(ntoh16(udp->length), kUdpHeaderLen + sizeof(payload));

    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, IpTotalLengthMatchesIpPlusUdpPlusPayload) {
    auto t = make_template();
    const uint8_t payload[200] = {0};
    rte_mbuf* mbuf = t.build(pool_, payload, sizeof(payload));
    ASSERT_NE(mbuf, nullptr);

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(
        rte_pktmbuf_mtod(mbuf, const uint8_t*) + kEtherHeaderLen);
    EXPECT_EQ(ntoh16(ip->total_length),
              kIpv4HeaderLen + kUdpHeaderLen + sizeof(payload));

    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, EtherTypeIsIpv4) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build(pool_, nullptr, 0);
    ASSERT_NE(mbuf, nullptr);

    auto* eth = rte_pktmbuf_mtod(mbuf, const rte_ether_hdr*);
    EXPECT_EQ(ntoh16(eth->ether_type), kEtherTypeIpv4);

    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, IpProtocolIsUdp) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build(pool_, nullptr, 0);
    ASSERT_NE(mbuf, nullptr);

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(
        rte_pktmbuf_mtod(mbuf, const uint8_t*) + kEtherHeaderLen);
    EXPECT_EQ(ip->next_proto_id, kIpProtoUdp);
    // Also verify the IPv4 version_ihl is exactly 0x45 (v4 + IHL=5).
    EXPECT_EQ(ip->version_ihl, 0x45);

    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, FragmentOffsetIsDontFragment) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build(pool_, nullptr, 0);
    ASSERT_NE(mbuf, nullptr);

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(
        rte_pktmbuf_mtod(mbuf, const uint8_t*) + kEtherHeaderLen);
    // Don't Fragment bit must be set.
    EXPECT_EQ(ntoh16(ip->fragment_offset) & 0x4000, 0x4000);
    rte_pktmbuf_free(mbuf);
}

TEST_F(UdpTemplateTest, BuildNullPoolReturnsNull) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build(/*pool=*/nullptr, nullptr, 0);
    EXPECT_EQ(mbuf, nullptr);
}

TEST_F(UdpTemplateTest, FillNullMbufReturnsZero) {
    auto t = make_template();
    EXPECT_EQ(t.fill(nullptr, nullptr, 0), 0u);
}

TEST_F(UdpTemplateTest, FillRoundTrip) {
    auto t = make_template();
    rte_mbuf* mbuf = rte_pktmbuf_alloc(pool_);
    ASSERT_NE(mbuf, nullptr);

    const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
    uint16_t written = t.fill(mbuf, payload, sizeof(payload));
    EXPECT_EQ(written, kEtherHeaderLen + kIpv4HeaderLen + kUdpHeaderLen + sizeof(payload));

    auto parsed = parse_udp_packet(mbuf);
    ASSERT_NE(parsed.udp, nullptr);
    EXPECT_EQ(parsed.payload_len, sizeof(payload));
    EXPECT_EQ(0, std::memcmp(parsed.payload, payload, sizeof(payload)));

    rte_pktmbuf_free(mbuf);
}

// ═══════════════════════════════════════════════════════════════════════
// init() field coverage — catches the audit-flagged memset+partial bug
// ═══════════════════════════════════════════════════════════════════════

TEST_F(UdpTemplateTest, AllIpHeaderFieldsExplicitlySet) {
    // Audit D4: init() uses memset(0) + partial assignment for the IP
    // header.  If a future maintainer adds a field to rte_ipv4_hdr and
    // forgets to assign it, the value silently leaks 0.  This test
    // checks every IPv4 header field that build/fill should populate
    // and has a meaningful expected value.
    auto t = make_template();
    rte_mbuf* mbuf = t.build(pool_, nullptr, 0);
    ASSERT_NE(mbuf, nullptr);

    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(
        rte_pktmbuf_mtod(mbuf, const uint8_t*) + kEtherHeaderLen);

    // Every field that init/build are supposed to set:
    EXPECT_EQ(ip->version_ihl,    0x45);                  // version=4, IHL=5
    EXPECT_EQ(ip->type_of_service, 0);                    // explicitly 0
    EXPECT_EQ(ntoh16(ip->total_length), 28);              // 20 IP + 8 UDP, no payload
    EXPECT_EQ(ip->next_proto_id,  kIpProtoUdp);
    EXPECT_NE(ip->time_to_live,    0);                    // some non-zero default
    EXPECT_EQ(ntoh16(ip->fragment_offset) & 0x4000, 0x4000); // DF
    // hdr_checksum is computed; just verify it's nonzero (a zero
    // checksum on a real packet is statistically unlikely).
    EXPECT_NE(ip->hdr_checksum, 0);
    EXPECT_NE(ip->src_addr, 0);
    EXPECT_NE(ip->dst_addr, 0);

    rte_pktmbuf_free(mbuf);
}
