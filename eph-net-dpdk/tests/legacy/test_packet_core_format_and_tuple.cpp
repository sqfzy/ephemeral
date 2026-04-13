/// @file test_packet_core_format_and_tuple.cpp
/// Unit tests for packet_core.hpp utility functions:
///   - format_ipv4() / format_mac()
///   - write_syn_options()
///   - ConnectionTuple (dump, to_json, equality, std::formatter)
///   - Ephemeral port constants

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <rte_ether.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/packet_core.hpp"

using namespace eph::dpdk::net;

// ═══════════════════════════════════════════════════════════════════════
// format_ipv4
// ═══════════════════════════════════════════════════════════════════════

TEST(FormatIpv4, LoopbackAddress) {
    auto buf = format_ipv4(0x7F000001);  // 127.0.0.1
    EXPECT_STREQ(buf.data(), "127.0.0.1");
}

TEST(FormatIpv4, AllZeros) {
    auto buf = format_ipv4(0x00000000);
    EXPECT_STREQ(buf.data(), "0.0.0.0");
}

TEST(FormatIpv4, AllOnes) {
    auto buf = format_ipv4(0xFFFFFFFF);
    EXPECT_STREQ(buf.data(), "255.255.255.255");
}

TEST(FormatIpv4, PrivateSubnet) {
    auto buf = format_ipv4(0x0A000001);  // 10.0.0.1
    EXPECT_STREQ(buf.data(), "10.0.0.1");
}

TEST(FormatIpv4, Class_C) {
    auto buf = format_ipv4(0xC0A80164);  // 192.168.1.100
    EXPECT_STREQ(buf.data(), "192.168.1.100");
}

TEST(FormatIpv4, MaxOctetsLength) {
    // Maximum length: "255.255.255.255" = 15 chars + null = 16, fits array
    auto buf = format_ipv4(0xFFFFFFFF);
    EXPECT_EQ(std::strlen(buf.data()), 15u);
}

// ═══════════════════════════════════════════════════════════════════════
// format_mac
// ═══════════════════════════════════════════════════════════════════════

TEST(FormatMac, AllZeros) {
    rte_ether_addr mac = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
    auto buf = format_mac(mac);
    EXPECT_STREQ(buf.data(), "00:00:00:00:00:00");
}

TEST(FormatMac, BroadcastAddress) {
    rte_ether_addr mac = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    auto buf = format_mac(mac);
    EXPECT_STREQ(buf.data(), "ff:ff:ff:ff:ff:ff");
}

TEST(FormatMac, ArbitraryAddress) {
    rte_ether_addr mac = {{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23}};
    auto buf = format_mac(mac);
    EXPECT_STREQ(buf.data(), "de:ad:be:ef:01:23");
}

TEST(FormatMac, ExactLength) {
    rte_ether_addr mac = {{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}};
    auto buf = format_mac(mac);
    EXPECT_EQ(std::strlen(buf.data()), 17u);  // "xx:xx:xx:xx:xx:xx"
}

// ═══════════════════════════════════════════════════════════════════════
// write_syn_options
// ═══════════════════════════════════════════════════════════════════════

TEST(WriteSynOptions, ReturnsCorrectLength) {
    uint8_t buf[kSynOptionsLen] = {};
    EXPECT_EQ(write_syn_options(buf, 1460), kSynOptionsLen);
}

TEST(WriteSynOptions, MssEncodedCorrectly) {
    uint8_t buf[kSynOptionsLen] = {};
    write_syn_options(buf, 1460);

    EXPECT_EQ(buf[0], 2);   // Kind: MSS
    EXPECT_EQ(buf[1], 4);   // Length: 4
    uint16_t mss_net;
    std::memcpy(&mss_net, &buf[2], 2);
    EXPECT_EQ(ntoh16(mss_net), 1460);
}

TEST(WriteSynOptions, SackPermitEncoded) {
    uint8_t buf[kSynOptionsLen] = {};
    write_syn_options(buf, 1460);
    EXPECT_EQ(buf[4], 4);   // Kind: SACK Permitted
    EXPECT_EQ(buf[5], 2);   // Length: 2
}

TEST(WriteSynOptions, WindowScaleEncoded) {
    uint8_t buf[kSynOptionsLen] = {};
    write_syn_options(buf, 1460);
    EXPECT_EQ(buf[6], 1);   // NOP padding
    EXPECT_EQ(buf[7], 3);   // Kind: Window Scale
    EXPECT_EQ(buf[8], 3);   // Length: 3
    EXPECT_EQ(buf[9], 0);   // Shift count: 0 (no scaling)
}

TEST(WriteSynOptions, NopPadding) {
    uint8_t buf[kSynOptionsLen] = {};
    write_syn_options(buf, 1460);
    EXPECT_EQ(buf[10], 1);  // NOP
    EXPECT_EQ(buf[11], 1);  // NOP
}

TEST(WriteSynOptions, MinimumMss) {
    uint8_t buf[kSynOptionsLen] = {};
    write_syn_options(buf, 536);  // RFC 879 minimum
    uint16_t mss_net;
    std::memcpy(&mss_net, &buf[2], 2);
    EXPECT_EQ(ntoh16(mss_net), 536);
}

TEST(WriteSynOptions, MaxMss) {
    uint8_t buf[kSynOptionsLen] = {};
    write_syn_options(buf, 65535);
    uint16_t mss_net;
    std::memcpy(&mss_net, &buf[2], 2);
    EXPECT_EQ(ntoh16(mss_net), 65535);
}

TEST(WriteSynOptions, ZeroMss) {
    uint8_t buf[kSynOptionsLen] = {};
    write_syn_options(buf, 0);
    uint16_t mss_net;
    std::memcpy(&mss_net, &buf[2], 2);
    EXPECT_EQ(ntoh16(mss_net), 0);
}

// ═══════════════════════════════════════════════════════════════════════
// ConnectionTuple
// ═══════════════════════════════════════════════════════════════════════

TEST(ConnectionTuple, DefaultIsAllZeros) {
    ConnectionTuple t{};
    EXPECT_EQ(t.src_ip, 0u);
    EXPECT_EQ(t.dst_ip, 0u);
    EXPECT_EQ(t.src_port, 0u);
    EXPECT_EQ(t.dst_port, 0u);
}

TEST(ConnectionTuple, EqualityOperator) {
    ConnectionTuple a{0x0A000001, 0x0A000002, 12345, 443};
    ConnectionTuple b{0x0A000001, 0x0A000002, 12345, 443};
    ConnectionTuple c{0x0A000001, 0x0A000002, 12345, 80};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(ConnectionTuple, DumpFormat) {
    ConnectionTuple t{0x0A000001, 0x0A000002, 12345, 443};
    std::string s = t.dump();
    EXPECT_EQ(s, "10.0.0.1:12345 -> 10.0.0.2:443");
}

TEST(ConnectionTuple, DumpLoopbackAddress) {
    ConnectionTuple t{0x7F000001, 0x7F000001, 80, 8080};
    std::string s = t.dump();
    EXPECT_EQ(s, "127.0.0.1:80 -> 127.0.0.1:8080");
}

TEST(ConnectionTuple, DumpBroadcastAddress) {
    ConnectionTuple t{0xFFFFFFFF, 0x00000000, 1, 2};
    std::string s = t.dump();
    EXPECT_EQ(s, "255.255.255.255:1 -> 0.0.0.0:2");
}

TEST(ConnectionTuple, ToJsonFormat) {
    ConnectionTuple t{0x0A000001, 0x0A000002, 12345, 443};
    std::string j = t.to_json();
    EXPECT_NE(j.find("\"src_ip\":\"10.0.0.1\""), std::string::npos);
    EXPECT_NE(j.find("\"dst_ip\":\"10.0.0.2\""), std::string::npos);
    EXPECT_NE(j.find("\"src_port\":12345"), std::string::npos);
    EXPECT_NE(j.find("\"dst_port\":443"), std::string::npos);
}

TEST(ConnectionTuple, StdFormatSpecialization) {
    ConnectionTuple t{0x0A000001, 0x0A000002, 12345, 443};
    std::string s = std::format("{}", t);
    EXPECT_EQ(s, "10.0.0.1:12345 -> 10.0.0.2:443");
}

// ═══════════════════════════════════════════════════════════════════════
// Ephemeral port constants
// ═══════════════════════════════════════════════════════════════════════

TEST(EphemeralPort, ConstantsCorrect) {
    EXPECT_EQ(kEphemeralPortMin, 49152u);
    EXPECT_EQ(kEphemeralPortRange, 16384u);
    // Verify range is a power of 2
    EXPECT_EQ(kEphemeralPortRange & (kEphemeralPortRange - 1), 0u);
    // Verify range covers [49152, 65535]
    EXPECT_EQ(kEphemeralPortMin + kEphemeralPortRange - 1, 65535u);
}

// ═══════════════════════════════════════════════════════════════════════
// Byte order helpers (sanity)
// ═══════════════════════════════════════════════════════════════════════

TEST(ByteOrder, Hton16RoundTrip) {
    uint16_t val = 0x1234;
    EXPECT_EQ(ntoh16(hton16(val)), val);
}

TEST(ByteOrder, Hton32RoundTrip) {
    uint32_t val = 0x12345678;
    EXPECT_EQ(ntoh32(hton32(val)), val);
}

TEST(ByteOrder, Hton16ZeroAndMax) {
    EXPECT_EQ(ntoh16(hton16(0)), 0u);
    EXPECT_EQ(ntoh16(hton16(0xFFFF)), 0xFFFFu);
}

TEST(ByteOrder, Hton32ZeroAndMax) {
    EXPECT_EQ(ntoh32(hton32(0)), 0u);
    EXPECT_EQ(ntoh32(hton32(0xFFFFFFFF)), 0xFFFFFFFFu);
}
