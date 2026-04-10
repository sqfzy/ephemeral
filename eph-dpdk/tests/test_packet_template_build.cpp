/// @file test_packet_template_build.cpp
/// Round-trip + checksum tests for eph::dpdk::net::PacketTemplate.
///
/// PacketTemplate::build_packet() is the canonical TX-side packet
/// constructor for TcpSession.  It writes Ethernet + IPv4 + TCP
/// headers (with optional SYN options + payload) into a freshly
/// allocated mbuf and computes IP and TCP checksums.
///
/// Coverage strategy:
///   1. Round-trip: build_packet → parse_packet → assert all fields
///      match the inputs.  This catches any byte-order, offset, or
///      field-mapping bug end-to-end.
///   2. Checksums: software-computed TCP and IP checksums must
///      self-verify (sum of header words including the checksum
///      field equals 0xFFFF, per RFC 1071).
///   3. Boundary cases: zero payload, MSS-sized payload, SYN with
///      options, ACK without options, allocation failure (null pool).

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_tcp.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/net_header.hpp"
#include "eph/dpdk/packet_parse.hpp"
#include "eph/dpdk/packet_template.hpp"

using namespace eph::dpdk::net;

namespace {

// ─────────────────────────────────────────────────────────────────────────
// Per-test mempool fixture
// ─────────────────────────────────────────────────────────────────────────

class PacketTemplateTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Create a small mempool — net_null EAL setup with --no-huge
        // serves these from anonymous mmap memory, so a few KB pool is
        // free.  Name must be unique per test binary.
        pool_ = rte_pktmbuf_pool_create(
            "test_packet_template_pool",
            /*n=*/256,
            /*cache_size=*/32,
            /*priv_size=*/0,
            /*data_room_size=*/RTE_MBUF_DEFAULT_BUF_SIZE,
            SOCKET_ID_ANY);
        ASSERT_NE(pool_, nullptr) << "rte_pktmbuf_pool_create failed";
    }

    static void TearDownTestSuite() {
        if (pool_) {
            rte_mempool_free(pool_);
            pool_ = nullptr;
        }
    }

    /// Build a fresh template with deterministic addresses.
    PacketTemplate make_template() const {
        PacketTemplate t;
        // Predictable MAC addresses — easier to assert against.
        for (int i = 0; i < 6; ++i) t.src_mac.addr_bytes[i] = static_cast<uint8_t>(0xAA + i);
        for (int i = 0; i < 6; ++i) t.dst_mac.addr_bytes[i] = static_cast<uint8_t>(0x11 + i);
        t.tuple.src_ip   = 0x0A000001;
        t.tuple.dst_ip   = 0x0A000002;
        t.tuple.src_port = 12345;
        t.tuple.dst_port = 443;
        t.mss            = 1460;
        t.hw_cksum       = false;
        return t;
    }

    static rte_mempool* pool_;
};

rte_mempool* PacketTemplateTest::pool_ = nullptr;

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// build_packet round-trip — assert build → parse recovers all fields
// ═══════════════════════════════════════════════════════════════════════

TEST_F(PacketTemplateTest, BuildAckRoundTrip) {
    auto t = make_template();
    constexpr uint32_t kSeq = 0x12345678;
    constexpr uint32_t kAck = 0x87654321;
    constexpr uint8_t  kFlags = kTcpAck;
    constexpr uint16_t kWindow = 8192;

    rte_mbuf* mbuf = t.build_packet(pool_, kSeq, kAck, kFlags, kWindow);
    ASSERT_NE(mbuf, nullptr);

    auto parsed = parse_packet(mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.seq(),    kSeq);
    EXPECT_EQ(parsed.ack(),    kAck);
    EXPECT_EQ(parsed.window(), kWindow);
    EXPECT_TRUE(parsed.has_flag(kTcpAck));
    EXPECT_FALSE(parsed.has_flag(kTcpSyn));
    EXPECT_FALSE(parsed.has_flag(kTcpFin));
    EXPECT_FALSE(parsed.has_flag(kTcpRst));
    EXPECT_EQ(parsed.payload_len, 0u);

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, BuildAckWithPayloadRoundTrip) {
    auto t = make_template();
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};

    rte_mbuf* mbuf = t.build_packet(pool_, 1000, 2000, kTcpAck | kTcpPsh,
                                     16384, payload, sizeof(payload));
    ASSERT_NE(mbuf, nullptr);

    auto parsed = parse_packet(mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, sizeof(payload));
    ASSERT_NE(parsed.payload, nullptr);
    EXPECT_EQ(0, std::memcmp(parsed.payload, payload, sizeof(payload)));
    EXPECT_TRUE(parsed.has_flag(kTcpAck));
    EXPECT_TRUE(parsed.has_flag(kTcpPsh));

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, BuildSynIncludesOptions) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build_packet(pool_, 1000, 0, kTcpSyn, 65535);
    ASSERT_NE(mbuf, nullptr);

    auto parsed = parse_packet(mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_TRUE(parsed.has_flag(kTcpSyn));

    // SYN packets must use kSynTcpHeaderLen, not the bare 20-byte header.
    uint8_t doff_words = parsed.tcp->data_off >> 4;
    EXPECT_GT(doff_words, 5) << "SYN must include TCP options";

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, BuildFinAckRoundTrip) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build_packet(pool_, 5000, 6000,
                                     kTcpFin | kTcpAck, 4096);
    ASSERT_NE(mbuf, nullptr);

    auto parsed = parse_packet(mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_TRUE(parsed.has_flag(kTcpFin));
    EXPECT_TRUE(parsed.has_flag(kTcpAck));
    EXPECT_EQ(parsed.seq(), 5000u);
    EXPECT_EQ(parsed.ack(), 6000u);

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, BuildRstRoundTrip) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build_packet(pool_, 7000, 0, kTcpRst, 0);
    ASSERT_NE(mbuf, nullptr);

    auto parsed = parse_packet(mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_TRUE(parsed.has_flag(kTcpRst));
    EXPECT_EQ(parsed.window(), 0u);

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, BuildMaxPayloadMss) {
    auto t = make_template();
    std::vector<uint8_t> payload(1460, 0xAB);
    rte_mbuf* mbuf = t.build_packet(pool_, 1000, 2000, kTcpAck, 8192,
                                     payload.data(),
                                     static_cast<uint16_t>(payload.size()));
    ASSERT_NE(mbuf, nullptr);

    auto parsed = parse_packet(mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.payload_len, payload.size());
    EXPECT_EQ(0, std::memcmp(parsed.payload, payload.data(), payload.size()));

    rte_pktmbuf_free(mbuf);
}

// ═══════════════════════════════════════════════════════════════════════
// Checksum self-verification
// ═══════════════════════════════════════════════════════════════════════

TEST_F(PacketTemplateTest, IpChecksumSelfVerifies) {
    // RFC 1071: an IP header (with its checksum field included) sums
    // to 0xFFFF when correct.
    auto t = make_template();
    rte_mbuf* mbuf = t.build_packet(pool_, 1000, 2000, kTcpAck, 8192);
    ASSERT_NE(mbuf, nullptr);

    const uint8_t* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);
    auto* ip = reinterpret_cast<const rte_ipv4_hdr*>(data + kEtherHeaderLen);
    uint16_t computed = internet_checksum(ip, kIpv4HeaderLen);
    EXPECT_EQ(computed, 0u) << "IP checksum self-verify must yield zero "
                                "(stored field already covers full header)";

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, TcpChecksumSelfVerifies) {
    auto t = make_template();
    const uint8_t payload[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    rte_mbuf* mbuf = t.build_packet(pool_, 1000, 2000, kTcpAck, 8192,
                                     payload, sizeof(payload));
    ASSERT_NE(mbuf, nullptr);

    const uint8_t* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);
    auto* ip  = reinterpret_cast<const rte_ipv4_hdr*>(data + kEtherHeaderLen);
    auto* tcp = reinterpret_cast<const rte_tcp_hdr*>(
        data + kEtherHeaderLen + kIpv4HeaderLen);

    // Re-run tcp_checksum on the assembled buffer (with the stored
    // checksum still in place).  The result should be 0 if the
    // stored field is correct.
    uint16_t tcp_total = kTcpHeaderLen + sizeof(payload);
    uint16_t verify = tcp_checksum(ip->src_addr, ip->dst_addr, tcp, tcp_total);
    EXPECT_EQ(verify, 0u);

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, TcpChecksumSelfVerifiesWithOddPayload) {
    // RFC 1071 padding case: odd-length payload pads the final byte
    // with zero before adding to the running 16-bit sum.  Off-by-one
    // here would corrupt every odd-length packet.
    auto t = make_template();
    const uint8_t payload[7] = {1, 2, 3, 4, 5, 6, 7};
    rte_mbuf* mbuf = t.build_packet(pool_, 1000, 2000, kTcpAck, 8192,
                                     payload, sizeof(payload));
    ASSERT_NE(mbuf, nullptr);

    const uint8_t* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);
    auto* ip  = reinterpret_cast<const rte_ipv4_hdr*>(data + kEtherHeaderLen);
    auto* tcp = reinterpret_cast<const rte_tcp_hdr*>(
        data + kEtherHeaderLen + kIpv4HeaderLen);

    uint16_t tcp_total = kTcpHeaderLen + sizeof(payload);
    uint16_t verify = tcp_checksum(ip->src_addr, ip->dst_addr, tcp, tcp_total);
    EXPECT_EQ(verify, 0u);

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, FlippedPayloadByteBreaksChecksum) {
    auto t = make_template();
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    rte_mbuf* mbuf = t.build_packet(pool_, 1000, 2000, kTcpAck, 8192,
                                     payload, sizeof(payload));
    ASSERT_NE(mbuf, nullptr);

    // Corrupt one payload byte and re-verify.
    auto* mut = rte_pktmbuf_mtod(mbuf, uint8_t*);
    mut[kEtherHeaderLen + kIpv4HeaderLen + kTcpHeaderLen + 3] ^= 0xFF;

    auto* ip  = reinterpret_cast<const rte_ipv4_hdr*>(mut + kEtherHeaderLen);
    auto* tcp = reinterpret_cast<const rte_tcp_hdr*>(
        mut + kEtherHeaderLen + kIpv4HeaderLen);
    uint16_t verify = tcp_checksum(ip->src_addr, ip->dst_addr, tcp,
                                    kTcpHeaderLen + sizeof(payload));
    EXPECT_NE(verify, 0u) << "checksum must detect single-bit corruption";

    rte_pktmbuf_free(mbuf);
}

// ═══════════════════════════════════════════════════════════════════════
// Address / ip_id / TTL fields
// ═══════════════════════════════════════════════════════════════════════

TEST_F(PacketTemplateTest, MacAddressesPropagated) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build_packet(pool_, 1000, 2000, kTcpAck, 8192);
    ASSERT_NE(mbuf, nullptr);

    const uint8_t* data = rte_pktmbuf_mtod(mbuf, const uint8_t*);
    auto* eth = reinterpret_cast<const rte_ether_hdr*>(data);
    EXPECT_EQ(0, std::memcmp(&eth->src_addr, &t.src_mac, sizeof(rte_ether_addr)));
    EXPECT_EQ(0, std::memcmp(&eth->dst_addr, &t.dst_mac, sizeof(rte_ether_addr)));

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, IpAddressesAndPortsPropagated) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build_packet(pool_, 1000, 2000, kTcpAck, 8192);
    ASSERT_NE(mbuf, nullptr);

    auto parsed = parse_packet(mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(ntoh32(parsed.ip->src_addr),  t.tuple.src_ip);
    EXPECT_EQ(ntoh32(parsed.ip->dst_addr),  t.tuple.dst_ip);
    EXPECT_EQ(ntoh16(parsed.tcp->src_port), t.tuple.src_port);
    EXPECT_EQ(ntoh16(parsed.tcp->dst_port), t.tuple.dst_port);

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, IpIdIncrementsPerPacket) {
    auto t = make_template();
    t.ip_id = 100;

    rte_mbuf* m1 = t.build_packet(pool_, 1, 1, kTcpAck, 100);
    rte_mbuf* m2 = t.build_packet(pool_, 1, 1, kTcpAck, 100);
    rte_mbuf* m3 = t.build_packet(pool_, 1, 1, kTcpAck, 100);
    ASSERT_NE(m1, nullptr);
    ASSERT_NE(m2, nullptr);
    ASSERT_NE(m3, nullptr);

    auto* ip1 = reinterpret_cast<const rte_ipv4_hdr*>(
        rte_pktmbuf_mtod(m1, const uint8_t*) + kEtherHeaderLen);
    auto* ip2 = reinterpret_cast<const rte_ipv4_hdr*>(
        rte_pktmbuf_mtod(m2, const uint8_t*) + kEtherHeaderLen);
    auto* ip3 = reinterpret_cast<const rte_ipv4_hdr*>(
        rte_pktmbuf_mtod(m3, const uint8_t*) + kEtherHeaderLen);

    EXPECT_EQ(ntoh16(ip1->packet_id), 100);
    EXPECT_EQ(ntoh16(ip2->packet_id), 101);
    EXPECT_EQ(ntoh16(ip3->packet_id), 102);

    rte_pktmbuf_free(m1);
    rte_pktmbuf_free(m2);
    rte_pktmbuf_free(m3);
}

TEST_F(PacketTemplateTest, NullPoolReturnsNull) {
    auto t = make_template();
    rte_mbuf* mbuf = t.build_packet(/*pool=*/nullptr, 1, 1, kTcpAck, 100);
    EXPECT_EQ(mbuf, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════
// fill_packet — zero-alloc variant
// ═══════════════════════════════════════════════════════════════════════

TEST_F(PacketTemplateTest, FillPacketRoundTrip) {
    auto t = make_template();
    rte_mbuf* mbuf = rte_pktmbuf_alloc(pool_);
    ASSERT_NE(mbuf, nullptr);

    const uint8_t payload[] = {0x42, 0x43, 0x44};
    uint16_t written = t.fill_packet(mbuf, /*seq=*/100, /*ack=*/200,
                                       kTcpAck, 4096, payload, sizeof(payload));
    EXPECT_GT(written, 0);

    auto parsed = parse_packet(mbuf);
    ASSERT_NE(parsed.tcp, nullptr);
    EXPECT_EQ(parsed.seq(), 100u);
    EXPECT_EQ(parsed.ack(), 200u);
    EXPECT_EQ(parsed.payload_len, sizeof(payload));
    EXPECT_EQ(0, std::memcmp(parsed.payload, payload, sizeof(payload)));

    rte_pktmbuf_free(mbuf);
}

TEST_F(PacketTemplateTest, FillPacketRejectsSyn) {
    // SYN must go through build_packet (which writes options); fill_packet
    // refuses it because the bare 20-byte TCP header has no option space.
    auto t = make_template();
    rte_mbuf* mbuf = rte_pktmbuf_alloc(pool_);
    ASSERT_NE(mbuf, nullptr);

    uint16_t written = t.fill_packet(mbuf, 1, 1, kTcpSyn, 100);
    EXPECT_EQ(written, 0);

    rte_pktmbuf_free(mbuf);
}
