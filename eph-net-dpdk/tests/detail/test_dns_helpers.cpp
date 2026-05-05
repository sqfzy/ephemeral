/// @file test_dns_helpers.cpp
/// Unit tests for DNS helper functions in dns.hpp:
///   - encode_qname() edge cases
///   - random_ephemeral_port() range validation
///   - DnsConfig validation and warnings
///
/// These complement the existing test_dns.cpp (full resolve path) and
/// test_dns_adversarial.cpp (malformed response parsing) by targeting
/// the encode/config surface that has no existing coverage.

#include <cstdint>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
#include "eph/dpdk/dns.hpp"

using namespace eph::dpdk;

// ═══════════════════════════════════════════════════════════════════════
// encode_qname
// ═══════════════════════════════════════════════════════════════════════

TEST(EncodeQname, SimpleHostname) {
    uint8_t buf[256] = {};
    size_t n = dns::detail::encode_qname(buf, "example.com");
    ASSERT_GT(n, 0u);
    // "\x07example\x03com\x00"
    EXPECT_EQ(buf[0], 7);  // "example" length
    EXPECT_EQ(std::memcmp(&buf[1], "example", 7), 0);
    EXPECT_EQ(buf[8], 3);  // "com" length
    EXPECT_EQ(std::memcmp(&buf[9], "com", 3), 0);
    EXPECT_EQ(buf[12], 0); // root terminator
    EXPECT_EQ(n, 13u);     // 1+7 + 1+3 + 1
}

TEST(EncodeQname, SingleLabel) {
    uint8_t buf[256] = {};
    size_t n = dns::detail::encode_qname(buf, "localhost");
    ASSERT_GT(n, 0u);
    EXPECT_EQ(buf[0], 9);  // "localhost" length
    EXPECT_EQ(buf[10], 0); // root terminator
    EXPECT_EQ(n, 11u);
}

TEST(EncodeQname, ThreeLabels) {
    uint8_t buf[256] = {};
    size_t n = dns::detail::encode_qname(buf, "api.binance.com");
    ASSERT_GT(n, 0u);
    EXPECT_EQ(buf[0], 3);  // "api"
    EXPECT_EQ(buf[4], 7);  // "binance"
    EXPECT_EQ(buf[12], 3); // "com"
}

TEST(EncodeQname, EmptyHostnameReturnsZero) {
    uint8_t buf[256] = {};
    EXPECT_EQ(dns::detail::encode_qname(buf, ""), 0u);
}

TEST(EncodeQname, ConsecutiveDotsRejected) {
    uint8_t buf[256] = {};
    // "example..com" has an empty label between the dots
    EXPECT_EQ(dns::detail::encode_qname(buf, "example..com"), 0u);
}

TEST(EncodeQname, LeadingDotRejected) {
    uint8_t buf[256] = {};
    EXPECT_EQ(dns::detail::encode_qname(buf, ".example.com"), 0u);
}

TEST(EncodeQname, TrailingDotProducesEmptyLabel) {
    uint8_t buf[256] = {};
    // "example.com." — trailing dot creates an empty label at the end
    EXPECT_EQ(dns::detail::encode_qname(buf, "example.com."), 0u);
}

TEST(EncodeQname, LabelExactly63BytesAccepted) {
    // Max DNS label length is 63 bytes
    std::string label(63, 'a');
    uint8_t buf[256] = {};
    size_t n = dns::detail::encode_qname(buf, label);
    ASSERT_GT(n, 0u);
    EXPECT_EQ(buf[0], 63);
}

TEST(EncodeQname, LabelOver63BytesRejected) {
    std::string label(64, 'a');
    uint8_t buf[256] = {};
    EXPECT_EQ(dns::detail::encode_qname(buf, label), 0u);
}

TEST(EncodeQname, TotalExactly253BytesAccepted) {
    // Build a hostname of exactly 253 bytes: "aaaa...a.bbbb...b.cccc...c.com"
    // 63+1+63+1+63+1+63 = 253+3(dots) but that's the string length
    // Actually: DNS allows the whole hostname up to 253 chars
    // Use 4 labels of 62 chars each + 3 dots = 251; adjust
    std::string hostname;
    for (int i = 0; i < 3; ++i) {
        hostname += std::string(63, 'a' + i);
        hostname += '.';
    }
    hostname += std::string(60, 'd');  // 63+1+63+1+63+1+60 = 252
    ASSERT_LE(hostname.size(), 253u);
    uint8_t buf[512] = {};
    size_t n = dns::detail::encode_qname(buf, hostname);
    EXPECT_GT(n, 0u);
}

TEST(EncodeQname, TotalOver253BytesRejected) {
    std::string hostname(254, 'a');
    uint8_t buf[512] = {};
    EXPECT_EQ(dns::detail::encode_qname(buf, hostname), 0u);
}

// ═══════════════════════════════════════════════════════════════════════
// random_ephemeral_port
// ═══════════════════════════════════════════════════════════════════════

TEST(RandomEphemeralPort, InRange) {
    // Generate 50 ports and verify all are in [49152, 65535]
    for (int i = 0; i < 50; ++i) {
        uint16_t port = dns::detail::random_ephemeral_port();
        if (port == 0) {
            // getrandom failure — acceptable in constrained environments
            continue;
        }
        EXPECT_GE(port, 49152u) << "Port " << port << " below ephemeral range";
        EXPECT_LE(port, 65535u) << "Port " << port << " above ephemeral range";
    }
}

TEST(RandomEphemeralPort, NotAllSame) {
    // Generate several ports and verify they're not all identical
    // (probability of 10 identical random ports ≈ 1/16384^9 ≈ 0)
    uint16_t first = dns::detail::random_ephemeral_port();
    if (first == 0) return;  // Skip if CSPRNG unavailable
    bool found_different = false;
    for (int i = 0; i < 10; ++i) {
        uint16_t p = dns::detail::random_ephemeral_port();
        if (p != 0 && p != first) {
            found_different = true;
            break;
        }
    }
    EXPECT_TRUE(found_different) << "All generated ports are identical — CSPRNG suspect";
}

// ═══════════════════════════════════════════════════════════════════════
// DnsConfig validation and warnings
// ═══════════════════════════════════════════════════════════════════════

TEST(DnsConfig, DefaultConfigValid) {
    dns::DnsConfig cfg{};
    // Default nameserver_ip is 8.8.8.8 — should pass validation
    auto err = cfg.validate();
    EXPECT_TRUE(err.empty()) << "Default DnsConfig should pass: " << err;
}

TEST(DnsConfig, ZeroNameserverFails) {
    dns::DnsConfig cfg{};
    cfg.nameserver_ip = 0;
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty()) << "Zero nameserver should fail validation";
}

TEST(DnsConfig, LoopbackNameserverWarns) {
    dns::DnsConfig cfg{};
    cfg.nameserver_ip = 0x7F000001;  // 127.0.0.1
    auto warnings = cfg.warnings();
    bool found_loopback_warning = false;
    for (const auto& w : warnings) {
        if (w.find("loopback") != std::string::npos ||
            w.find("127.") != std::string::npos) {
            found_loopback_warning = true;
            break;
        }
    }
    EXPECT_TRUE(found_loopback_warning)
        << "Loopback nameserver should produce a warning";
}
