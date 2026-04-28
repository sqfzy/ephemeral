/// @file test_dns_adversarial.cpp
/// Adversarial / malformed-input tests for the DNS parser in
/// eph::dpdk::dns::detail.
///
/// The pre-existing test_dns.cpp covers happy-path parsing and a few
/// pointer-loop edge cases, but does NOT exercise:
///   * deep compression-pointer chains (the audit-flagged D2 issue —
///     up to kMaxIterations × an_count iterations of CPU work
///     per malicious response; kMaxIterations was narrowed 128 → 32
///     per audit-20260413 #11 so the budget is now bounded at 32 × 64
///     = 2048 hops, still far above any legitimate FQDN depth)
///   * RDATA length-claim vs. buffer boundary
///   * try_parse_dns_packet's IP/UDP layer attack surface (truncated
///     packets, IHL extremes, wrong proto, wrong source IP)
///   * label byte type 0x40 (extended) and 0x80 (reserved)
///
/// These tests treat the parser as adversarial input handling: every
/// edge case the audit raised gets a concrete vector that triggers
/// the relevant rejection branch.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/dns.hpp"
#include "eph/dpdk/net_header.hpp"

using namespace eph::dpdk;

namespace {

// ─────────────────────────────────────────────────────────────────────────
// DNS message builder helpers
// ─────────────────────────────────────────────────────────────────────────

/// Lay out a 12-byte DNS header at the start of `buf`.
/// Flags = QR(0x8000) | OPCODE_QUERY(0) | RA(0x80) — typical response.
void write_dns_header(uint8_t* buf, uint16_t tx_id, uint16_t qd, uint16_t an) {
    auto* hdr = reinterpret_cast<dns::DnsHeader*>(buf);
    hdr->id       = tx_id; // raw — caller picks endianness
    hdr->flags    = net::hton16(0x8080); // QR=1, RA=1
    hdr->qd_count = net::hton16(qd);
    hdr->an_count = net::hton16(an);
    hdr->ns_count = 0;
    hdr->ar_count = 0;
}

/// Append an inline-label question section: QNAME=root, QTYPE=A, QCLASS=IN
size_t append_root_question(uint8_t* buf, size_t off) {
    buf[off++] = 0x00; // root label
    buf[off++] = 0x00; buf[off++] = 0x01; // QTYPE=A
    buf[off++] = 0x00; buf[off++] = 0x01; // QCLASS=IN
    return off;
}

/// Append an A-record answer pointing back to the question via compression
/// pointer 0x000C.  RDATA = 4-byte IP, big-endian.
size_t append_a_answer(uint8_t* buf, size_t off, uint32_t ip_be) {
    buf[off++] = 0xC0; buf[off++] = 0x0C; // pointer to offset 12 (root question)
    buf[off++] = 0x00; buf[off++] = 0x01; // TYPE=A
    buf[off++] = 0x00; buf[off++] = 0x01; // CLASS=IN
    buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x00; buf[off++] = 0x3C; // TTL=60
    buf[off++] = 0x00; buf[off++] = 0x04; // RDLENGTH=4
    std::memcpy(buf + off, &ip_be, 4);
    off += 4;
    return off;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// skip_dns_name — pointer-chain depth attack (audit D2)
// ═══════════════════════════════════════════════════════════════════════

TEST(DnsAdversarial, PointerChainHittingIterationLimitReturnsZero) {
    // Construct a chain of 200 pointers, each pointing to the next.
    // The parser must detect after kMaxIterations=32 hops and bail.
    // (Was 128 pre-audit-20260413 #11; tightened to 32 as defence-
    // in-depth — 200 still trips it with margin to spare.)
    constexpr size_t kChainLen = 200;
    constexpr size_t kBufSize  = kChainLen * 2 + 16;
    std::vector<uint8_t> buf(kBufSize, 0);

    // Each pointer slot is 2 bytes: 0xC0 high-bits + offset hi+lo.
    // We point each slot to the NEXT slot, except the last which points
    // to a sentinel inline 0x00 (root) — but only if we make it past
    // the iteration limit.  Since we won't, the sentinel is unreachable.
    for (size_t i = 0; i < kChainLen; ++i) {
        size_t target = (i + 1) * 2;
        buf[i * 2]     = 0xC0 | static_cast<uint8_t>((target >> 8) & 0x3F);
        buf[i * 2 + 1] = static_cast<uint8_t>(target & 0xFF);
    }
    // Last slot's target is past kChainLen*2; place an inline 0 there.
    buf[kChainLen * 2] = 0x00;

    auto end = dns::detail::skip_dns_name(buf.data(), 0, buf.size());
    EXPECT_EQ(end, 0u) << "200-deep pointer chain must trip iteration limit";
}

TEST(DnsAdversarial, ShortPointerChainSucceeds) {
    // 10-pointer chain ending in inline root — must succeed and report
    // the offset right after the FIRST pointer slot (per skip_dns_name's
    // "first pointer determines end_offset" semantics).
    constexpr size_t kChainLen = 10;
    constexpr size_t kBufSize  = kChainLen * 2 + 16;
    std::vector<uint8_t> buf(kBufSize, 0);

    for (size_t i = 0; i < kChainLen; ++i) {
        size_t target = (i + 1) * 2;
        buf[i * 2]     = 0xC0 | static_cast<uint8_t>((target >> 8) & 0x3F);
        buf[i * 2 + 1] = static_cast<uint8_t>(target & 0xFF);
    }
    buf[kChainLen * 2] = 0x00; // root terminator

    auto end = dns::detail::skip_dns_name(buf.data(), 0, buf.size());
    EXPECT_EQ(end, 2u) << "first pointer occupies bytes [0, 2); end_offset == 2";
}

TEST(DnsAdversarial, PointerToSelfDirectReturnsZero) {
    // Direct self-loop: pointer at offset 0 targets offset 0.
    uint8_t buf[16] = {0xC0, 0x00};
    auto end = dns::detail::skip_dns_name(buf, 0, sizeof(buf));
    EXPECT_EQ(end, 0u);
}

TEST(DnsAdversarial, PointerJustOutsideBufferReturnsZero) {
    // Pointer targeting offset == len.  skip_dns_name checks `ptr >= len`.
    uint8_t buf[16] = {0xC0, 0x10}; // ptr = 16, len = 16
    auto end = dns::detail::skip_dns_name(buf, 0, sizeof(buf));
    EXPECT_EQ(end, 0u);
}

TEST(DnsAdversarial, ReservedLabelType01Rejected) {
    // Top two bits 01 is reserved (RFC 2671 EDNS0 OPT but only in OPT RR).
    // skip_dns_name's branches: 0x00 (inline), 0xC0 (pointer).  Anything
    // else with high bit 0 must be a normal label length (≤ 63), which
    // 0x40 fails (label_len > 63).
    uint8_t buf[16] = {0x40, 0x00};
    auto end = dns::detail::skip_dns_name(buf, 0, sizeof(buf));
    EXPECT_EQ(end, 0u);
}

TEST(DnsAdversarial, ReservedLabelType10Rejected) {
    uint8_t buf[16] = {0x80, 0x00};
    auto end = dns::detail::skip_dns_name(buf, 0, sizeof(buf));
    EXPECT_EQ(end, 0u);
}

TEST(DnsAdversarial, MaxValidLabelLength63Accepted) {
    // 63-byte label followed by root terminator — must succeed.
    uint8_t buf[80] = {0};
    buf[0] = 63;
    for (int i = 1; i <= 63; ++i) buf[i] = 'a';
    buf[64] = 0x00; // root

    auto end = dns::detail::skip_dns_name(buf, 0, sizeof(buf));
    EXPECT_EQ(end, 65u);
}

TEST(DnsAdversarial, LabelExtendingPastBufferReturnsZero) {
    // Label claims 10 bytes but buffer has only 5 left after the length byte.
    uint8_t buf[6] = {10, 'a', 'b', 'c', 'd', 'e'};
    auto end = dns::detail::skip_dns_name(buf, 0, sizeof(buf));
    EXPECT_EQ(end, 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// parse_dns_response — RDATA boundary attacks
// ═══════════════════════════════════════════════════════════════════════

TEST(DnsAdversarial, RdlengthClaimsBytesPastBufferReturnsError) {
    // Build a real header + question + answer-name-pointer + TYPE/CLASS/TTL
    // + RDLENGTH that exceeds the remaining bytes.
    std::vector<uint8_t> buf(64, 0);
    write_dns_header(buf.data(), /*tx_id=*/0xCAFE, 1, 1);
    size_t off = dns::kDnsHeaderLen;
    off = append_root_question(buf.data(), off);
    // Answer: pointer to question (offset 12)
    buf[off++] = 0xC0; buf[off++] = 0x0C;
    buf[off++] = 0x00; buf[off++] = 0x01; // TYPE=A
    buf[off++] = 0x00; buf[off++] = 0x01; // CLASS=IN
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; buf[off++] = 60; // TTL
    // RDLENGTH=255 (way beyond what we have)
    buf[off++] = 0x00; buf[off++] = 0xFF;
    // No RDATA actually written.
    size_t total_len = off;

    auto r = dns::detail::parse_dns_response(buf.data(), total_len, 0xCAFE);
    EXPECT_FALSE(r.has_value()) << "rdlength past buffer must be rejected";
}

TEST(DnsAdversarial, ManyAnswersExceedingCountLimitReturnsError) {
    // Header claims an_count=65 (over the 64 hard cap).
    std::vector<uint8_t> buf(128, 0);
    write_dns_header(buf.data(), 0xBEEF, 1, 65);
    auto r = dns::detail::parse_dns_response(buf.data(), buf.size(), 0xBEEF);
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("answer count"), std::string::npos);
}

TEST(DnsAdversarial, ManyQuestionsExceedingCountLimitReturnsError) {
    // Header claims qd_count=17 (over the 16 hard cap).
    std::vector<uint8_t> buf(128, 0);
    write_dns_header(buf.data(), 0xBEEF, 17, 1);
    auto r = dns::detail::parse_dns_response(buf.data(), buf.size(), 0xBEEF);
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("question count"), std::string::npos);
}

TEST(DnsAdversarial, ZeroLengthBufferReturnsError) {
    auto r = dns::detail::parse_dns_response(nullptr, 0, 0);
    EXPECT_FALSE(r.has_value());
}

TEST(DnsAdversarial, OnlyHeaderNoQuestionReturnsError) {
    // qd_count=1 but no question payload.
    uint8_t buf[dns::kDnsHeaderLen] = {0};
    write_dns_header(buf, 0x1234, 1, 1);
    auto r = dns::detail::parse_dns_response(buf, sizeof(buf), 0x1234);
    EXPECT_FALSE(r.has_value());
}

TEST(DnsAdversarial, ValidResponseRoundTrip) {
    // Sanity: a hand-built valid A-record response parses correctly.
    // tx_id passed to parse_dns_response is in NETWORK byte order
    // (compared raw against hdr->id).
    std::vector<uint8_t> buf(64, 0);
    uint16_t tx_id_net = net::hton16(0xABCD);
    write_dns_header(buf.data(), tx_id_net, 1, 1);
    size_t off = dns::kDnsHeaderLen;
    off = append_root_question(buf.data(), off);
    off = append_a_answer(buf.data(), off, net::hton32(0x08080404)); // 8.8.4.4

    auto r = dns::detail::parse_dns_response(buf.data(), off, tx_id_net);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r, 0x08080404u);
}

// ═══════════════════════════════════════════════════════════════════════
// try_parse_dns_packet — IP/UDP layer attack surface
// ═══════════════════════════════════════════════════════════════════════

namespace {

/// Build a fake mbuf containing Ethernet+IPv4+UDP+DNS, returning the
/// total length.  Uses the existing valid-response builder for the
/// DNS payload so we can independently corrupt the IP/UDP layers.
struct FakeDnsMbuf {
    alignas(8) uint8_t buf[512];
    rte_mbuf mbuf{};

    /// Build a valid IP+UDP wrapper around a DNS payload.
    /// `nameserver_ip_host` is the source IP for the IP header.
    void build(uint32_t nameserver_ip_host, uint16_t tx_id_net) {
        constexpr size_t eth_len = net::kEtherHeaderLen;
        constexpr size_t ip_len  = 20;
        constexpr size_t udp_len = dns::kUdpHeaderLen;
        std::memset(buf, 0, sizeof(buf));

        // Build DNS payload first so we know its length.
        std::vector<uint8_t> dns_buf(64, 0);
        write_dns_header(dns_buf.data(), tx_id_net, 1, 1);
        size_t doff = dns::kDnsHeaderLen;
        doff = append_root_question(dns_buf.data(), doff);
        doff = append_a_answer(dns_buf.data(), doff, net::hton32(0x08080808));
        size_t dns_payload_len = doff;

        // Ethernet
        auto* eth = reinterpret_cast<rte_ether_hdr*>(buf);
        eth->ether_type = net::hton16(net::kEtherTypeIpv4);

        // IPv4
        auto* ip = reinterpret_cast<rte_ipv4_hdr*>(buf + eth_len);
        ip->version_ihl  = (4 << 4) | 5;
        ip->total_length  = net::hton16(
            static_cast<uint16_t>(ip_len + udp_len + dns_payload_len));
        ip->next_proto_id = net::kIpProtoUdp;
        ip->src_addr      = net::hton32(nameserver_ip_host);
        ip->dst_addr      = net::hton32(0x0A000001);

        // UDP
        auto* udp = reinterpret_cast<dns::UdpHeader*>(buf + eth_len + ip_len);
        udp->src_port = net::hton16(53);
        udp->dst_port = net::hton16(54321);
        udp->length   = net::hton16(static_cast<uint16_t>(udp_len + dns_payload_len));
        udp->checksum = 0;

        // DNS payload
        std::memcpy(buf + eth_len + ip_len + udp_len,
                    dns_buf.data(), dns_payload_len);

        size_t total = eth_len + ip_len + udp_len + dns_payload_len;
        mbuf = rte_mbuf{};
        mbuf.buf_addr = buf;
        mbuf.data_off = 0;
        mbuf.data_len = static_cast<uint16_t>(total);
        mbuf.pkt_len  = static_cast<uint32_t>(total);
    }
};

} // namespace

TEST(DnsAdversarial, TryParsePacketHappyPath) {
    FakeDnsMbuf fake;
    uint16_t tx_id_net = net::hton16(0xDEAD);
    fake.build(/*nameserver_ip=*/0x08080808, tx_id_net);

    auto r = dns::detail::try_parse_dns_packet(&fake.mbuf, tx_id_net,
                                                0x08080808);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0x08080808u);
}

TEST(DnsAdversarial, TryParsePacketWrongSourceIpRejected) {
    FakeDnsMbuf fake;
    uint16_t tx_id_net = net::hton16(0xDEAD);
    fake.build(/*nameserver_ip=*/0x08080808, tx_id_net);

    // Same packet but caller expects a different nameserver.
    auto r = dns::detail::try_parse_dns_packet(&fake.mbuf, tx_id_net,
                                                /*expected=*/0x01010101);
    EXPECT_FALSE(r.has_value());
}

TEST(DnsAdversarial, TryParsePacketWrongTxIdRejected) {
    FakeDnsMbuf fake;
    fake.build(0x08080808, net::hton16(0xDEAD));
    auto r = dns::detail::try_parse_dns_packet(&fake.mbuf,
                                                net::hton16(0xBEEF),
                                                0x08080808);
    EXPECT_FALSE(r.has_value());
}

TEST(DnsAdversarial, TryParsePacketTooShortRejected) {
    FakeDnsMbuf fake;
    fake.build(0x08080808, net::hton16(0xDEAD));
    // Truncate to less than minimum length.
    fake.mbuf.data_len = 20;
    fake.mbuf.pkt_len  = 20;
    auto r = dns::detail::try_parse_dns_packet(&fake.mbuf,
                                                net::hton16(0xDEAD),
                                                0x08080808);
    EXPECT_FALSE(r.has_value());
}

TEST(DnsAdversarial, TryParsePacketTcpProtocolRejected) {
    FakeDnsMbuf fake;
    fake.build(0x08080808, net::hton16(0xDEAD));
    // Flip protocol byte to TCP.
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(fake.buf + net::kEtherHeaderLen);
    ip->next_proto_id = net::kIpProtoTcp;
    auto r = dns::detail::try_parse_dns_packet(&fake.mbuf,
                                                net::hton16(0xDEAD),
                                                0x08080808);
    EXPECT_FALSE(r.has_value());
}

TEST(DnsAdversarial, TryParsePacketIhlBelow20Rejected) {
    FakeDnsMbuf fake;
    fake.build(0x08080808, net::hton16(0xDEAD));
    // Set IHL to 4 (16 bytes — below the IPv4 minimum of 20).
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(fake.buf + net::kEtherHeaderLen);
    ip->version_ihl = (4 << 4) | 4;
    auto r = dns::detail::try_parse_dns_packet(&fake.mbuf,
                                                net::hton16(0xDEAD),
                                                0x08080808);
    EXPECT_FALSE(r.has_value());
}

TEST(DnsAdversarial, TryParsePacketIhlMaxFitsBuffer) {
    // IHL=15 (60-byte IP header).  Our packet doesn't actually have
    // 40 bytes of options, so the bounds check at line 466 of dns.hpp
    // (eth + ihl + udp + dns_hdr > data_len) should reject it.
    FakeDnsMbuf fake;
    fake.build(0x08080808, net::hton16(0xDEAD));
    auto* ip = reinterpret_cast<rte_ipv4_hdr*>(fake.buf + net::kEtherHeaderLen);
    ip->version_ihl = (4 << 4) | 15;
    auto r = dns::detail::try_parse_dns_packet(&fake.mbuf,
                                                net::hton16(0xDEAD),
                                                0x08080808);
    // Don't strictly require failure — depends on whether the inflated
    // IHL still fits inside the actual mbuf data_len.  Just verify no
    // crash and the result is well-defined.
    if (r.has_value()) {
        FAIL() << "IHL=15 with no options should not parse to a valid IP";
    }
}

TEST(DnsAdversarial, TryParsePacketWrongUdpSourcePortRejected) {
    FakeDnsMbuf fake;
    fake.build(0x08080808, net::hton16(0xDEAD));
    // Change UDP src_port from 53 to 8888.
    auto* udp = reinterpret_cast<dns::UdpHeader*>(
        fake.buf + net::kEtherHeaderLen + 20);
    udp->src_port = net::hton16(8888);
    auto r = dns::detail::try_parse_dns_packet(&fake.mbuf,
                                                net::hton16(0xDEAD),
                                                0x08080808);
    EXPECT_FALSE(r.has_value());
}

TEST(DnsAdversarial, TryParsePacketUdpLengthExceedsBufferRejected) {
    FakeDnsMbuf fake;
    fake.build(0x08080808, net::hton16(0xDEAD));
    // Inflate UDP length to claim more than the actual buffer.
    auto* udp = reinterpret_cast<dns::UdpHeader*>(
        fake.buf + net::kEtherHeaderLen + 20);
    udp->length = net::hton16(1500);
    auto r = dns::detail::try_parse_dns_packet(&fake.mbuf,
                                                net::hton16(0xDEAD),
                                                0x08080808);
    EXPECT_FALSE(r.has_value());
}

TEST(DnsAdversarial, TryParsePacketNonIpv4EtherTypeRejected) {
    FakeDnsMbuf fake;
    fake.build(0x08080808, net::hton16(0xDEAD));
    // Change to IPv6 ether_type.
    auto* eth = reinterpret_cast<rte_ether_hdr*>(fake.buf);
    eth->ether_type = net::hton16(0x86DD);
    auto r = dns::detail::try_parse_dns_packet(&fake.mbuf,
                                                net::hton16(0xDEAD),
                                                0x08080808);
    EXPECT_FALSE(r.has_value());
}
