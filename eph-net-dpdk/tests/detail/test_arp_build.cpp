/// @file test_arp_build.cpp
/// Round-trip and adversarial tests for eph::dpdk::arp::build_arp_request.
///
/// The pre-existing test_arp.cpp covers parse_arp_reply edge cases
/// extensively (wrong hw_type, wrong addr_len, wrong opcode, etc.) but
/// uses a hand-rolled `build_fake_arp_reply` helper rather than
/// exercising the production `build_arp_request` function.  As a
/// result, ANY bug in build_arp_request — wrong byte order on a field,
/// wrong opcode, wrong sender MAC encoding — would never be caught by
/// the existing tests.
///
/// This file fills that gap with field-by-field assertions on the
/// bytes produced by build_arp_request, plus a few adversarial
/// parse_arp_reply cases not covered elsewhere.

#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/arp.hpp"
#include "eph/dpdk/net_header.hpp"

using namespace eph::dpdk;

namespace {

class ArpBuildTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        pool_ = rte_pktmbuf_pool_create(
            "test_arp_build_pool", /*n=*/64, /*cache=*/16,
            /*priv_size=*/0, RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
        ASSERT_NE(pool_, nullptr);
    }
    static void TearDownTestSuite() {
        if (pool_) { rte_mempool_free(pool_); pool_ = nullptr; }
    }
    static rte_mempool* pool_;
};

rte_mempool* ArpBuildTest::pool_ = nullptr;

// ─────────────────────────────────────────────────────────────────────────
// Helper: extract ArpPacket from a built mbuf.
// ─────────────────────────────────────────────────────────────────────────
const arp::ArpPacket* arp_payload(const rte_mbuf* mbuf) {
    const auto* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);
    return reinterpret_cast<const arp::ArpPacket*>(data + net::kEtherHeaderLen);
}

const rte_ether_hdr* eth_header(const rte_mbuf* mbuf) {
    return rte_pktmbuf_mtod(mbuf, const rte_ether_hdr*);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// build_arp_request — field-by-field verification
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ArpBuildTest, BuildArpRequestProducesValidFrame) {
    rte_ether_addr src_mac = {{0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01}};
    uint32_t src_ip    = 0x0A000001; // 10.0.0.1
    uint32_t target_ip = 0x0A000002; // 10.0.0.2

    rte_mbuf* mbuf = arp::build_arp_request(pool_, src_mac, src_ip, target_ip);
    ASSERT_NE(mbuf, nullptr);

    // Frame length must be exactly Ethernet header + ARP payload.
    EXPECT_EQ(rte_pktmbuf_data_len(mbuf),
              net::kEtherHeaderLen + sizeof(arp::ArpPacket));

    // Ethernet: dst = broadcast, src = our MAC, ether_type = ARP.
    auto* eth = eth_header(mbuf);
    rte_ether_addr broadcast = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    EXPECT_EQ(0, std::memcmp(&eth->dst_addr, &broadcast, 6));
    EXPECT_EQ(0, std::memcmp(&eth->src_addr, &src_mac,   6));
    EXPECT_EQ(net::ntoh16(eth->ether_type), arp::kEtherTypeArp);

    // ARP payload: hw=Ethernet, proto=IPv4, lens=6/4, opcode=REQUEST.
    auto* p = arp_payload(mbuf);
    EXPECT_EQ(net::ntoh16(p->hw_type),    arp::kArpHwTypeEthernet);
    EXPECT_EQ(net::ntoh16(p->proto_type), arp::kArpProtoIpv4);
    EXPECT_EQ(p->hw_addr_len,             arp::kArpHwAddrLen);
    EXPECT_EQ(p->proto_addr_len,          arp::kArpProtoAddrLen);
    EXPECT_EQ(net::ntoh16(p->opcode),     arp::kArpOpRequest);

    // Sender: our MAC and IP (host byte order → wire is big-endian).
    EXPECT_EQ(0, std::memcmp(p->sender_mac, src_mac.addr_bytes, 6));
    EXPECT_EQ(net::ntoh32(p->sender_ip), src_ip);

    // Target: zero MAC (unknown), our target IP.
    constexpr uint8_t zero_mac[6] = {0};
    EXPECT_EQ(0, std::memcmp(p->target_mac, zero_mac, 6));
    EXPECT_EQ(net::ntoh32(p->target_ip), target_ip);

    rte_pktmbuf_free(mbuf);
}

TEST_F(ArpBuildTest, BuildArpRequestZeroSrcIpAccepted) {
    // RFC 5227 ARP probe: src_ip = 0 is legal.  build_arp_request must
    // not reject this — it has no validation, just bit packing.
    rte_ether_addr src_mac = {{0x02, 0, 0, 0, 0, 1}};
    rte_mbuf* mbuf = arp::build_arp_request(pool_, src_mac, /*src_ip=*/0, 0x0A000001);
    ASSERT_NE(mbuf, nullptr);
    auto* p = arp_payload(mbuf);
    EXPECT_EQ(net::ntoh32(p->sender_ip), 0u);
    rte_pktmbuf_free(mbuf);
}

TEST_F(ArpBuildTest, BuildArpRequestNullPoolReturnsNull) {
    rte_ether_addr src_mac = {{0x02, 0, 0, 0, 0, 1}};
    rte_mbuf* mbuf = arp::build_arp_request(/*pool=*/nullptr, src_mac, 1, 2);
    EXPECT_EQ(mbuf, nullptr);
}

TEST_F(ArpBuildTest, BuildArpRequestPreservesFullByteRangeAddresses) {
    // Stress all-bits-set addresses to catch any byte-narrowing bug.
    rte_ether_addr src_mac = {{0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA}};
    uint32_t src_ip    = 0xDEADBEEF;
    uint32_t target_ip = 0xCAFEBABE;

    rte_mbuf* mbuf = arp::build_arp_request(pool_, src_mac, src_ip, target_ip);
    ASSERT_NE(mbuf, nullptr);
    auto* p = arp_payload(mbuf);
    EXPECT_EQ(0, std::memcmp(p->sender_mac, src_mac.addr_bytes, 6));
    EXPECT_EQ(net::ntoh32(p->sender_ip),   src_ip);
    EXPECT_EQ(net::ntoh32(p->target_ip),   target_ip);
    rte_pktmbuf_free(mbuf);
}

// ═══════════════════════════════════════════════════════════════════════
// Round-trip: build_arp_request → mutate to reply → parse_arp_reply
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ArpBuildTest, BuildRequestThenSimulateReplyRoundTrip) {
    // Pretend we built a request, then mutate the same buffer into a
    // reply (as a peer would on the wire) and verify parse_arp_reply
    // recovers the sender MAC and matches the target IP.
    rte_ether_addr our_mac     = {{0x02, 0, 0, 0, 0, 1}};
    rte_ether_addr peer_mac    = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    uint32_t our_ip   = 0x0A000001;
    uint32_t peer_ip  = 0x0A000002;

    rte_mbuf* mbuf = arp::build_arp_request(pool_, our_mac, our_ip, peer_ip);
    ASSERT_NE(mbuf, nullptr);

    // Mutate to be a reply FROM peer TO us:
    //   eth.dst = our MAC (unicast), eth.src = peer MAC
    //   arp.opcode = REPLY
    //   arp.sender_mac/sender_ip = peer
    //   arp.target_mac/target_ip = us
    auto* mut = rte_pktmbuf_mtod(mbuf, uint8_t*);
    auto* eth = reinterpret_cast<rte_ether_hdr*>(mut);
    rte_ether_addr_copy(&our_mac,  &eth->dst_addr);
    rte_ether_addr_copy(&peer_mac, &eth->src_addr);

    auto* arp_pkt = reinterpret_cast<arp::ArpPacket*>(mut + net::kEtherHeaderLen);
    arp_pkt->opcode = net::hton16(arp::kArpOpReply);
    std::memcpy(arp_pkt->sender_mac, peer_mac.addr_bytes, 6);
    arp_pkt->sender_ip = net::hton32(peer_ip);
    std::memcpy(arp_pkt->target_mac, our_mac.addr_bytes, 6);
    arp_pkt->target_ip = net::hton32(our_ip);

    auto reply = arp::parse_arp_reply(mbuf, peer_ip);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(0, std::memcmp(reply->addr_bytes, peer_mac.addr_bytes, 6));

    rte_pktmbuf_free(mbuf);
}

TEST_F(ArpBuildTest, RoundTripRejectsWhenSenderIpMismatchesTarget) {
    // Same as above but parse_arp_reply is called with a DIFFERENT
    // target_ip than the one in the simulated reply — must reject.
    rte_ether_addr our_mac  = {{0x02, 0, 0, 0, 0, 1}};
    rte_ether_addr peer_mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    rte_mbuf* mbuf = arp::build_arp_request(pool_, our_mac, 0x0A000001, 0x0A000002);
    ASSERT_NE(mbuf, nullptr);

    auto* mut = rte_pktmbuf_mtod(mbuf, uint8_t*);
    auto* arp_pkt = reinterpret_cast<arp::ArpPacket*>(mut + net::kEtherHeaderLen);
    arp_pkt->opcode = net::hton16(arp::kArpOpReply);
    std::memcpy(arp_pkt->sender_mac, peer_mac.addr_bytes, 6);
    arp_pkt->sender_ip = net::hton32(0x0A000099); // wrong IP

    auto reply = arp::parse_arp_reply(mbuf, /*target_ip=*/0x0A000002);
    EXPECT_FALSE(reply.has_value());

    rte_pktmbuf_free(mbuf);
}

// ═══════════════════════════════════════════════════════════════════════
// Adversarial parse_arp_reply cases not in test_arp.cpp
// ═══════════════════════════════════════════════════════════════════════

TEST_F(ArpBuildTest, ParseArpReplyAcceptsExactMinLengthFrame) {
    // The minimum valid ARP-over-Ethernet frame is exactly
    // kEtherHeaderLen + sizeof(ArpPacket).  Verify the parser accepts
    // it without padding.
    rte_ether_addr peer_mac = {{0x02, 0, 0, 0, 0, 1}};
    rte_mbuf* mbuf = arp::build_arp_request(pool_, peer_mac, 0x0A000002, 0x0A000001);
    ASSERT_NE(mbuf, nullptr);

    // Mutate into a reply.
    auto* mut = rte_pktmbuf_mtod(mbuf, uint8_t*);
    auto* arp_pkt = reinterpret_cast<arp::ArpPacket*>(mut + net::kEtherHeaderLen);
    arp_pkt->opcode = net::hton16(arp::kArpOpReply);

    EXPECT_EQ(rte_pktmbuf_data_len(mbuf),
              net::kEtherHeaderLen + sizeof(arp::ArpPacket));
    auto reply = arp::parse_arp_reply(mbuf, 0x0A000002);
    EXPECT_TRUE(reply.has_value());

    rte_pktmbuf_free(mbuf);
}

TEST_F(ArpBuildTest, ParseArpReplyAcceptsPaddedFrame) {
    // Real Ethernet frames are padded to 60 bytes (the 14-byte Eth +
    // 46-byte minimum payload).  ARP packets are only 28 bytes so the
    // wire frame has 18 bytes of trailing padding.  parse_arp_reply
    // must accept this — it only requires the buffer to be AT LEAST
    // min_len, not equal.
    rte_mbuf* mbuf = rte_pktmbuf_alloc(pool_);
    ASSERT_NE(mbuf, nullptr);

    constexpr size_t padded_len = 60;
    auto* pkt = rte_pktmbuf_append(mbuf, padded_len);
    ASSERT_NE(pkt, nullptr);
    std::memset(pkt, 0, padded_len);

    rte_ether_addr peer_mac = {{0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01}};
    auto* eth = reinterpret_cast<rte_ether_hdr*>(pkt);
    rte_ether_addr_copy(&peer_mac, &eth->src_addr);
    eth->ether_type = net::hton16(arp::kEtherTypeArp);

    auto* arp_pkt = reinterpret_cast<arp::ArpPacket*>(
        reinterpret_cast<uint8_t*>(pkt) + net::kEtherHeaderLen);
    arp_pkt->hw_type        = net::hton16(arp::kArpHwTypeEthernet);
    arp_pkt->proto_type     = net::hton16(arp::kArpProtoIpv4);
    arp_pkt->hw_addr_len    = arp::kArpHwAddrLen;
    arp_pkt->proto_addr_len = arp::kArpProtoAddrLen;
    arp_pkt->opcode         = net::hton16(arp::kArpOpReply);
    std::memcpy(arp_pkt->sender_mac, peer_mac.addr_bytes, 6);
    arp_pkt->sender_ip      = net::hton32(0x0A000001);

    auto reply = arp::parse_arp_reply(mbuf, 0x0A000001);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(0, std::memcmp(reply->addr_bytes, peer_mac.addr_bytes, 6));

    rte_pktmbuf_free(mbuf);
}

TEST_F(ArpBuildTest, ParseArpReplyRejectsZeroLengthFrame) {
    rte_mbuf* mbuf = rte_pktmbuf_alloc(pool_);
    ASSERT_NE(mbuf, nullptr);
    // Don't append any data — data_len stays 0.
    auto reply = arp::parse_arp_reply(mbuf, 0x0A000001);
    EXPECT_FALSE(reply.has_value());
    rte_pktmbuf_free(mbuf);
}
