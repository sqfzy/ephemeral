/// @file test_packet_core_checksum.cpp
/// Unit tests for checksum functions in packet_core.hpp:
///   - internet_checksum()
///   - pseudo_header_sum()
///   - tcp_checksum()
///   - udp_checksum()
///
/// Covers: standard payloads, odd-length payloads, zero-length segments,
/// single-byte payloads, and boundary conditions.

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/packet_core.hpp"

using namespace eph::dpdk::net;

// ═══════════════════════════════════════════════════════════════════════
// internet_checksum
// ═══════════════════════════════════════════════════════════════════════

TEST(InternetChecksum, ZeroLengthReturnsAllOnes) {
    uint8_t buf[1] = {};
    EXPECT_EQ(internet_checksum(buf, 0), static_cast<uint16_t>(0xFFFF));
}

TEST(InternetChecksum, NullDataReturnsAllOnes) {
    EXPECT_EQ(internet_checksum(static_cast<const uint8_t*>(nullptr), 0),
              static_cast<uint16_t>(0xFFFF));
}

TEST(InternetChecksum, SingleByteNonZero) {
    uint8_t buf[1] = {0xAB};
    uint16_t cksum = internet_checksum(buf, 1);
    EXPECT_NE(cksum, 0u);
    EXPECT_NE(cksum, 0xFFFFu);
}

TEST(InternetChecksum, EvenLengthComplementVerification) {
    // Standard complement property: checksum of (data ++ le16(cksum)) = 0
    uint8_t data[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    uint16_t cksum = internet_checksum(data, sizeof(data));
    EXPECT_NE(cksum, 0u);

    // Append checksum in LE16 format and re-verify
    std::vector<uint8_t> verify(data, data + sizeof(data));
    verify.push_back(static_cast<uint8_t>(cksum & 0xFF));
    verify.push_back(static_cast<uint8_t>(cksum >> 8));
    EXPECT_EQ(internet_checksum(verify.data(), verify.size()),
              static_cast<uint16_t>(0));
}

TEST(InternetChecksum, AllZerosPayload) {
    uint8_t data[16] = {};
    EXPECT_EQ(internet_checksum(data, sizeof(data)), static_cast<uint16_t>(0xFFFF));
}

TEST(InternetChecksum, AllOnesPayload) {
    uint8_t data[4];
    std::memset(data, 0xFF, sizeof(data));
    uint16_t cksum = internet_checksum(data, sizeof(data));
    EXPECT_EQ(cksum, static_cast<uint16_t>(0x0000));
}

TEST(InternetChecksum, TwoBytesSymmetric) {
    uint8_t d1[] = {0x12, 0x34};
    uint8_t d2[] = {0x12, 0x34};
    EXPECT_EQ(internet_checksum(d1, 2), internet_checksum(d2, 2));
}

TEST(InternetChecksum, OddLengthDeterministic) {
    uint8_t data[] = {0x45, 0x00, 0x00, 0x3c, 0x1c};
    uint16_t c1 = internet_checksum(data, 5);
    uint16_t c2 = internet_checksum(data, 5);
    EXPECT_EQ(c1, c2);
    EXPECT_NE(c1, 0u);
}

TEST(InternetChecksum, ConstexprEvaluation) {
    // Verify the function works at compile time
    constexpr uint8_t data[] = {0xAA, 0xBB};
    constexpr uint16_t cksum = internet_checksum(data, 2);
    static_assert(cksum != 0, "Constexpr checksum should be non-zero");
    EXPECT_NE(cksum, 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// pseudo_header_sum
// ═══════════════════════════════════════════════════════════════════════

TEST(PseudoHeaderSum, Deterministic) {
    uint32_t src = hton32(0x0A000001);
    uint32_t dst = hton32(0x0A000002);
    uint32_t sum1 = pseudo_header_sum(src, dst, kIpProtoTcp, 20);
    uint32_t sum2 = pseudo_header_sum(src, dst, kIpProtoTcp, 20);
    EXPECT_EQ(sum1, sum2);
    EXPECT_NE(sum1, 0u);
}

TEST(PseudoHeaderSum, TcpAndUdpDiffer) {
    uint32_t src = hton32(0x0A000001);
    uint32_t dst = hton32(0x0A000002);
    EXPECT_NE(pseudo_header_sum(src, dst, kIpProtoTcp, 28),
              pseudo_header_sum(src, dst, kIpProtoUdp, 28));
}

TEST(PseudoHeaderSum, ZeroLengthSegment) {
    uint32_t src = hton32(0x0A000001);
    uint32_t dst = hton32(0x0A000002);
    uint32_t sum = pseudo_header_sum(src, dst, kIpProtoUdp, 0);
    // Zero-length contributes nothing but IP+proto still produce a sum
    EXPECT_NE(sum, 0u);
}

TEST(PseudoHeaderSum, DifferentIpsDiffer) {
    uint32_t src1 = hton32(0x0A000001);
    uint32_t src2 = hton32(0x0A000099);
    uint32_t dst  = hton32(0x0A000002);
    EXPECT_NE(pseudo_header_sum(src1, dst, kIpProtoTcp, 20),
              pseudo_header_sum(src2, dst, kIpProtoTcp, 20));
}

// ═══════════════════════════════════════════════════════════════════════
// tcp_checksum — verify self-consistency (store checksum, re-verify = 0)
// ═══════════════════════════════════════════════════════════════════════

TEST(TcpChecksum, MinimalHeaderSelfVerifies) {
    uint8_t tcp_hdr[20] = {};
    uint16_t src_port = hton16(12345);
    uint16_t dst_port = hton16(80);
    std::memcpy(tcp_hdr + 0, &src_port, 2);
    std::memcpy(tcp_hdr + 2, &dst_port, 2);
    tcp_hdr[12] = 5 << 4;

    uint32_t src_ip = hton32(0x0A000001);
    uint32_t dst_ip = hton32(0x0A000002);
    uint16_t cksum = tcp_checksum(src_ip, dst_ip, tcp_hdr, 20);
    EXPECT_NE(cksum, 0u);

    // Store checksum in header and re-verify → should be 0
    std::memcpy(tcp_hdr + 16, &cksum, 2);
    EXPECT_EQ(tcp_checksum(src_ip, dst_ip, tcp_hdr, 20), 0u);
}

TEST(TcpChecksum, HeaderPlusOddPayloadSelfVerifies) {
    constexpr size_t total = 25;  // 20-byte header + 5-byte payload (odd)
    uint8_t seg[total] = {};
    seg[12] = 5 << 4;
    seg[20] = 'H'; seg[21] = 'e'; seg[22] = 'l'; seg[23] = 'l'; seg[24] = 'o';

    uint32_t src_ip = hton32(0xC0A80001);
    uint32_t dst_ip = hton32(0xC0A80002);
    uint16_t cksum = tcp_checksum(src_ip, dst_ip, seg, total);
    EXPECT_NE(cksum, 0u);

    std::memcpy(seg + 16, &cksum, 2);
    EXPECT_EQ(tcp_checksum(src_ip, dst_ip, seg, total), 0u);
}

TEST(TcpChecksum, LargeEvenPayloadSelfVerifies) {
    constexpr size_t total = 20 + 1460;  // typical MSS
    std::vector<uint8_t> seg(total, 0);
    seg[12] = 5 << 4;
    for (size_t i = 20; i < total; ++i) seg[i] = static_cast<uint8_t>(i & 0xFF);

    uint32_t src_ip = hton32(0x0A000001);
    uint32_t dst_ip = hton32(0x0A000002);
    uint16_t cksum = tcp_checksum(src_ip, dst_ip, seg.data(), total);

    std::memcpy(seg.data() + 16, &cksum, 2);
    EXPECT_EQ(tcp_checksum(src_ip, dst_ip, seg.data(), total), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// udp_checksum
// ═══════════════════════════════════════════════════════════════════════

TEST(UdpChecksum, MinimalHeaderSelfVerifies) {
    uint8_t udp_hdr[8] = {};
    uint16_t src_port = hton16(12345);
    uint16_t dst_port = hton16(53);
    uint16_t length   = hton16(8);
    std::memcpy(udp_hdr + 0, &src_port, 2);
    std::memcpy(udp_hdr + 2, &dst_port, 2);
    std::memcpy(udp_hdr + 4, &length, 2);

    uint32_t src_ip = hton32(0x0A000001);
    uint32_t dst_ip = hton32(0x0A000002);
    uint16_t cksum = udp_checksum(src_ip, dst_ip, udp_hdr, 8);
    // RFC 768: transmitted checksum must never be 0x0000
    EXPECT_NE(cksum, static_cast<uint16_t>(0));

    // Store and re-verify
    std::memcpy(udp_hdr + 6, &cksum, 2);
    uint16_t verify = udp_checksum(src_ip, dst_ip, udp_hdr, 8);
    // After storing valid checksum, re-computation should produce 0xFFFF
    // (because ~0 = 0xFFFF, and the RFC 768 rule maps that to 0xFFFF)
    EXPECT_EQ(verify, static_cast<uint16_t>(0xFFFF));
}

TEST(UdpChecksum, OddLengthPayloadNonZero) {
    constexpr size_t total = 11;  // 8-byte header + 3-byte payload
    uint8_t seg[total] = {};
    uint16_t length = hton16(total);
    std::memcpy(seg + 4, &length, 2);
    seg[8] = 0xDE; seg[9] = 0xAD; seg[10] = 0xBE;

    uint32_t src_ip = hton32(0x0A000001);
    uint32_t dst_ip = hton32(0x0A000002);
    uint16_t cksum = udp_checksum(src_ip, dst_ip, seg, total);
    EXPECT_NE(cksum, static_cast<uint16_t>(0));
}

TEST(UdpChecksum, NeverReturnsZero) {
    // RFC 768: UDP checksum 0x0000 must be transmitted as 0xFFFF
    uint8_t seg[16] = {};
    uint16_t length = hton16(16);
    std::memcpy(seg + 4, &length, 2);

    uint32_t src_ip = hton32(0x00000000);
    uint32_t dst_ip = hton32(0x00000000);
    uint16_t cksum = udp_checksum(src_ip, dst_ip, seg, 16);
    EXPECT_NE(cksum, static_cast<uint16_t>(0));
}
