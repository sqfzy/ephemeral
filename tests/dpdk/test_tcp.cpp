/// @file test_tcp.cpp
/// Unit tests for DPDK TcpConfig validation, formatting, and TcpSession
/// compile-time concept satisfaction.

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/tcp.hpp"

using namespace eph::dpdk;

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::validate — happy path
// ─────────────────────────────────────────────────────────────────────────────

static TcpConfig make_valid_config() {
    TcpConfig cfg;
    cfg.tuple.src_ip   = 0x0A000001; // 10.0.0.1
    cfg.tuple.dst_ip   = 0x0A000002; // 10.0.0.2
    cfg.tuple.src_port = 12345;
    cfg.tuple.dst_port = 443;
    cfg.mss            = 1460;
    cfg.recv_window    = 65535;
    return cfg;
}

TEST(TcpConfig, ValidConfigPassesValidation) {
    auto cfg = make_valid_config();
    EXPECT_TRUE(cfg.validate().empty()) << cfg.validate();
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::validate — error cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, ZeroSrcIpFails) {
    auto cfg = make_valid_config();
    cfg.tuple.src_ip = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("src_ip"), std::string_view::npos);
}

TEST(TcpConfig, ZeroDstIpFails) {
    auto cfg = make_valid_config();
    cfg.tuple.dst_ip = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("dst_ip"), std::string_view::npos);
}

TEST(TcpConfig, ZeroSrcPortFails) {
    auto cfg = make_valid_config();
    cfg.tuple.src_port = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("src_port"), std::string_view::npos);
}

TEST(TcpConfig, ZeroDstPortFails) {
    auto cfg = make_valid_config();
    cfg.tuple.dst_port = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("dst_port"), std::string_view::npos);
}

TEST(TcpConfig, ZeroMssFails) {
    auto cfg = make_valid_config();
    cfg.mss = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("mss"), std::string_view::npos);
}

TEST(TcpConfig, MssExceedsJumboFrameFails) {
    auto cfg = make_valid_config();
    cfg.mss = 9001;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("jumbo"), std::string_view::npos);
}

TEST(TcpConfig, MssAtJumboFrameLimit) {
    auto cfg = make_valid_config();
    cfg.mss = 9000;
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(TcpConfig, ZeroRecvWindowFails) {
    auto cfg = make_valid_config();
    cfg.recv_window = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("recv_window"), std::string_view::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::format_mac
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, FormatMacZero) {
    rte_ether_addr mac{};
    std::memset(&mac, 0, sizeof(mac));
    EXPECT_EQ(TcpConfig::format_mac(mac), "00:00:00:00:00:00");
}

TEST(TcpConfig, FormatMacBroadcast) {
    rte_ether_addr mac{};
    std::memset(&mac, 0xFF, sizeof(mac));
    EXPECT_EQ(TcpConfig::format_mac(mac), "ff:ff:ff:ff:ff:ff");
}

TEST(TcpConfig, FormatMacCustom) {
    rte_ether_addr mac{};
    mac.addr_bytes[0] = 0xDE;
    mac.addr_bytes[1] = 0xAD;
    mac.addr_bytes[2] = 0xBE;
    mac.addr_bytes[3] = 0xEF;
    mac.addr_bytes[4] = 0x01;
    mac.addr_bytes[5] = 0x23;
    EXPECT_EQ(TcpConfig::format_mac(mac), "de:ad:be:ef:01:23");
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig::dump and to_json
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, DumpContainsKeyFields) {
    auto cfg = make_valid_config();
    auto dump = cfg.dump();
    EXPECT_NE(dump.find("TcpConfig"), std::string::npos);
    EXPECT_NE(dump.find("mss: 1460"), std::string::npos);
    EXPECT_NE(dump.find("recv_window: 65535"), std::string::npos);
}

TEST(TcpConfig, ToJsonIsValidStructure) {
    auto cfg = make_valid_config();
    auto json = cfg.to_json();
    EXPECT_NE(json.find("\"src_ip\""), std::string::npos);
    EXPECT_NE(json.find("\"dst_ip\""), std::string::npos);
    EXPECT_NE(json.find("\"mss\":1460"), std::string::npos);
    // Verify braces
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpConfig equality
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpConfig, EqualityIdentical) {
    auto a = make_valid_config();
    auto b = make_valid_config();
    EXPECT_EQ(a, b);
}

TEST(TcpConfig, EqualityDifferentPort) {
    auto a = make_valid_config();
    auto b = make_valid_config();
    b.tuple.src_port = 9999;
    EXPECT_NE(a, b);
}

TEST(TcpConfig, EqualityDifferentMac) {
    auto a = make_valid_config();
    auto b = make_valid_config();
    b.src_mac.addr_bytes[0] = 0xFF;
    EXPECT_NE(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpSession concept satisfaction
// ─────────────────────────────────────────────────────────────────────────────

TEST(TcpSession, SatisfiesTcpTransportConcept) {
    static_assert(eph::net::TcpTransport<TcpSession<>>,
                  "TcpSession must satisfy TcpTransport concept");
}
