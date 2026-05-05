#include <gtest/gtest.h>
#include <string_view>

// Note: deliberately NOT including dpdk_test_env.hpp — all tests here are
// pure codec / validation tests that don't need EAL initialization.
// This allows them to actually RUN (not just SKIP) on machines without DPDK hardware.
#include "eph/dpdk/dns.hpp"

using namespace eph::dpdk;
using namespace eph::dpdk::dns;
using namespace eph::dpdk::dns::detail;

// ─────────────────────────────────────────────────────────────────────────────
// QNAME encoding
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsQnameEncode, SimpleHostname) {
    uint8_t buf[256];
    size_t len = encode_qname(buf, "example.com");
    // Expected: \x07 e x a m p l e \x03 c o m \x00
    ASSERT_EQ(len, 13u);
    EXPECT_EQ(buf[0], 7);
    EXPECT_EQ(std::memcmp(&buf[1], "example", 7), 0);
    EXPECT_EQ(buf[8], 3);
    EXPECT_EQ(std::memcmp(&buf[9], "com", 3), 0);
    EXPECT_EQ(buf[12], 0);
}

TEST(DnsQnameEncode, SingleLabel) {
    uint8_t buf[256];
    size_t len = encode_qname(buf, "localhost");
    ASSERT_EQ(len, 11u);  // \x09 l o c a l h o s t \x00 = 1+9+1
    EXPECT_EQ(buf[0], 9);
    EXPECT_EQ(buf[10], 0);  // Root label terminator
}

TEST(DnsQnameEncode, ThreeLabels) {
    uint8_t buf[256];
    size_t len = encode_qname(buf, "www.example.com");
    ASSERT_EQ(len, 17u);
    EXPECT_EQ(buf[0], 3);   // "www"
    EXPECT_EQ(buf[4], 7);   // "example"
    EXPECT_EQ(buf[12], 3);  // "com"
    EXPECT_EQ(buf[16], 0);  // root
}

TEST(DnsQnameEncode, EmptyHostnameReturnsZero) {
    uint8_t buf[256];
    EXPECT_EQ(encode_qname(buf, ""), 0u);
}

TEST(DnsQnameEncode, EmptyLabelReturnsZero) {
    uint8_t buf[256];
    EXPECT_EQ(encode_qname(buf, "example..com"), 0u);
}

TEST(DnsQnameEncode, LabelTooLongReturnsZero) {
    // Label > 63 chars
    std::string long_label(64, 'a');
    long_label += ".com";
    uint8_t buf[256];
    EXPECT_EQ(encode_qname(buf, long_label), 0u);
}

TEST(DnsQnameEncode, MaxLabelLength63) {
    std::string label_63(63, 'a');
    label_63 += ".com";
    uint8_t buf[256];
    size_t len = encode_qname(buf, label_63);
    EXPECT_GT(len, 0u);
    EXPECT_EQ(buf[0], 63);
}

// ─────────────────────────────────────────────────────────────────────────────
// DNS query building
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsBuildQuery, ValidQuery) {
    uint8_t buf[512];
    uint16_t tx_id = net::hton16(0x1234);
    size_t len = build_dns_query(buf, tx_id, "example.com");

    ASSERT_GT(len, kDnsHeaderLen);

    auto* hdr = reinterpret_cast<const DnsHeader*>(buf);
    EXPECT_EQ(hdr->id, tx_id);
    EXPECT_EQ(net::ntoh16(hdr->flags), kDnsFlagRd);
    EXPECT_EQ(net::ntoh16(hdr->qd_count), 1);
    EXPECT_EQ(net::ntoh16(hdr->an_count), 0);

    // Total = 12 (header) + 13 (qname) + 4 (qtype+qclass) = 29
    EXPECT_EQ(len, 29u);
}

// ─────────────────────────────────────────────────────────────────────────────
// DNS response parsing
// ─────────────────────────────────────────────────────────────────────────────

// Helper: build a minimal DNS response for testing
static std::vector<uint8_t> build_test_response(
    uint16_t tx_id, uint16_t flags, uint16_t an_count,
    const std::string& hostname, uint32_t ip_host) {

    std::vector<uint8_t> pkt;

    // DNS header
    DnsHeader hdr{};
    hdr.id       = tx_id;
    hdr.flags    = net::hton16(flags);
    hdr.qd_count = net::hton16(1);
    hdr.an_count = net::hton16(an_count);
    hdr.ns_count = 0;
    hdr.ar_count = 0;
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&hdr),
               reinterpret_cast<uint8_t*>(&hdr) + sizeof(hdr));

    // Question section: QNAME + QTYPE + QCLASS
    uint8_t qname[256];
    size_t qname_len = encode_qname(qname, hostname);
    pkt.insert(pkt.end(), qname, qname + qname_len);
    uint16_t qtype = net::hton16(kDnsTypeA);
    uint16_t qclass = net::hton16(kDnsClassIn);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&qtype),
               reinterpret_cast<uint8_t*>(&qtype) + 2);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&qclass),
               reinterpret_cast<uint8_t*>(&qclass) + 2);

    // Answer section (if an_count > 0)
    if (an_count > 0) {
        // Name pointer to offset 12 (start of question QNAME)
        uint16_t ptr = net::hton16(0xC00C);
        pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ptr),
                   reinterpret_cast<uint8_t*>(&ptr) + 2);

        // TYPE = A
        uint16_t rr_type = net::hton16(kDnsTypeA);
        pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&rr_type),
                   reinterpret_cast<uint8_t*>(&rr_type) + 2);

        // CLASS = IN
        uint16_t rr_class = net::hton16(kDnsClassIn);
        pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&rr_class),
                   reinterpret_cast<uint8_t*>(&rr_class) + 2);

        // TTL = 300
        uint32_t ttl = net::hton32(300);
        pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ttl),
                   reinterpret_cast<uint8_t*>(&ttl) + 4);

        // RDLENGTH = 4
        uint16_t rdlen = net::hton16(4);
        pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&rdlen),
                   reinterpret_cast<uint8_t*>(&rdlen) + 2);

        // RDATA = IPv4 address
        uint32_t ip_net = net::hton32(ip_host);
        pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ip_net),
                   reinterpret_cast<uint8_t*>(&ip_net) + 4);
    }

    return pkt;
}

TEST(DnsParseResponse, ValidARecord) {
    uint16_t tx_id = net::hton16(0xABCD);
    uint32_t expected_ip = 0xC0A80101;  // 192.168.1.1

    auto pkt = build_test_response(
        tx_id, kDnsFlagQr | kDnsFlagRd, 1, "example.com", expected_ip);

    auto result = parse_dns_response(pkt.data(), pkt.size(), tx_id);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, expected_ip);
}

TEST(DnsParseResponse, WrongTxIdReturnsError) {
    auto pkt = build_test_response(
        net::hton16(0x1111), kDnsFlagQr | kDnsFlagRd, 1,
        "example.com", 0x01020304);

    auto result = parse_dns_response(pkt.data(), pkt.size(), net::hton16(0x2222));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("transaction ID"), std::string::npos);
}

TEST(DnsParseResponse, NonResponseReturnsError) {
    // QR bit not set (it's a query, not a response)
    auto pkt = build_test_response(
        net::hton16(0x1234), kDnsFlagRd, 1, "example.com", 0x01020304);

    auto result = parse_dns_response(pkt.data(), pkt.size(), net::hton16(0x1234));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("QR bit"), std::string::npos);
}

TEST(DnsParseResponse, NonZeroRcodeReturnsError) {
    // RCODE = 3 (NXDOMAIN)
    auto pkt = build_test_response(
        net::hton16(0x1234), kDnsFlagQr | kDnsFlagRd | 3, 0,
        "nonexistent.example.com", 0);

    auto result = parse_dns_response(pkt.data(), pkt.size(), net::hton16(0x1234));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("RCODE=3"), std::string::npos);
}

TEST(DnsParseResponse, NoAnswersReturnsError) {
    auto pkt = build_test_response(
        net::hton16(0x1234), kDnsFlagQr | kDnsFlagRd, 0, "example.com", 0);

    auto result = parse_dns_response(pkt.data(), pkt.size(), net::hton16(0x1234));
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("no answer"), std::string::npos);
}

TEST(DnsParseResponse, TooShortReturnsError) {
    uint8_t buf[4] = {0};
    auto result = parse_dns_response(buf, 4, 0);
    ASSERT_FALSE(result.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// DNS name skipping (pointer compression)
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsSkipName, InlineLabels) {
    // \x03 f o o \x03 b a r \x00
    uint8_t data[] = {3, 'f', 'o', 'o', 3, 'b', 'a', 'r', 0};
    size_t offset = skip_dns_name(data, 0, sizeof(data));
    EXPECT_EQ(offset, 9u);
}

TEST(DnsSkipName, PointerLabel) {
    // At offset 0: pointer to offset 10 (arbitrary, just test skipping)
    uint8_t data[20] = {};
    data[0] = 0xC0;  // Pointer
    data[1] = 0x0A;  // Offset = 10
    data[10] = 3;
    data[11] = 'f'; data[12] = 'o'; data[13] = 'o';
    data[14] = 0;   // Root label
    size_t offset = skip_dns_name(data, 0, sizeof(data));
    EXPECT_EQ(offset, 2u);  // After the 2-byte pointer
}

// ─────────────────────────────────────────────────────────────────────────────
// DnsConfig defaults
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsConfig, DefaultNameserver) {
    DnsConfig cfg{};
    EXPECT_EQ(cfg.nameserver_ip, 0x08080808u);  // 8.8.8.8
    EXPECT_EQ(cfg.port, 53);
    EXPECT_EQ(cfg.timeout, std::chrono::milliseconds{3000});
}

TEST(DnsConfig, DumpContainsNameserverAndTimeout) {
    DnsConfig cfg{};
    auto dump = cfg.dump();
    EXPECT_NE(dump.find("DnsConfig:"), std::string::npos);
    EXPECT_NE(dump.find("8.8.8.8"), std::string::npos);
    EXPECT_NE(dump.find("53"), std::string::npos);
    EXPECT_NE(dump.find("3000ms"), std::string::npos);
}

TEST(DnsConfig, DumpCustomConfig) {
    DnsConfig cfg{
        .nameserver_ip = 0x01010101,  // 1.1.1.1
        .port = 5353,
        .timeout = std::chrono::milliseconds{500},
    };
    auto dump = cfg.dump();
    EXPECT_NE(dump.find("1.1.1.1"), std::string::npos);
    EXPECT_NE(dump.find("5353"), std::string::npos);
    EXPECT_NE(dump.find("500ms"), std::string::npos);
}

TEST(DnsConfig, ToJsonDefaultConfig) {
    DnsConfig cfg{};
    auto json = cfg.to_json();
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
    EXPECT_NE(json.find("\"nameserver_ip\":\"8.8.8.8\""), std::string::npos);
    EXPECT_NE(json.find("\"port\":53"), std::string::npos);
    EXPECT_NE(json.find("\"timeout_ms\":3000"), std::string::npos);
}

TEST(DnsConfig, ToJsonCustomConfig) {
    DnsConfig cfg{
        .nameserver_ip = 0x01010101,  // 1.1.1.1
        .port = 5353,
        .timeout = std::chrono::milliseconds{500},
    };
    auto json = cfg.to_json();
    EXPECT_NE(json.find("\"nameserver_ip\":\"1.1.1.1\""), std::string::npos);
    EXPECT_NE(json.find("\"port\":5353"), std::string::npos);
    EXPECT_NE(json.find("\"timeout_ms\":500"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// DnsConfig::validate
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsConfigValidation, DefaultIsValid) {
    DnsConfig cfg{};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(DnsConfigValidation, ZeroNameserverFails) {
    DnsConfig cfg{.nameserver_ip = 0};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("nameserver_ip"), std::string_view::npos);
}

TEST(DnsConfigValidation, ZeroPortFails) {
    DnsConfig cfg{.port = 0};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("port"), std::string_view::npos);
}

TEST(DnsConfigValidation, ZeroTimeoutFails) {
    DnsConfig cfg{.timeout = std::chrono::milliseconds{0}};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("timeout"), std::string_view::npos);
}

TEST(DnsConfigValidation, NegativeTimeoutFails) {
    DnsConfig cfg{.timeout = std::chrono::milliseconds{-1}};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("timeout"), std::string_view::npos);
}

TEST(DnsConfigValidation, CustomValidConfig) {
    DnsConfig cfg{
        .nameserver_ip = 0x01010101,  // 1.1.1.1
        .port = 5353,
        .timeout = std::chrono::milliseconds{100},
    };
    EXPECT_TRUE(cfg.validate().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// resolve() — fast path: dotted-decimal IPv4 bypass
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsResolve, DottedDecimalBypassesDns) {
    // resolve() with a dotted-decimal hostname should return immediately
    // without needing any DPDK infrastructure (pool/port/mac don't matter)
    rte_ether_addr dummy_mac{};
    auto result = dns::resolve(0, 0, nullptr, dummy_mac, dummy_mac, 0,
                               "192.168.1.1");
    // This should succeed via parse_ipv4 fast path (pool=nullptr is fine
    // because we never reach the packet-sending code)
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, 0xC0A80101u);
}

// resolve() now returns std::expected<uint32_t, eph::core::ErrorInfo>
// (T3.18 alignment with project-wide error type). Each pre-flight
// rejection maps to Error::InvalidConfig with a static-literal `detail`
// string; we assert on .detail substring + .code rather than the old
// std::string error.
TEST(DnsResolve, EmptyHostnameReturnsError) {
    rte_ether_addr dummy_mac{};
    auto result = dns::resolve(0, 0, nullptr, dummy_mac, dummy_mac, 0, "");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{result.error().detail}.find("empty"),
              std::string_view::npos);
}

TEST(DnsResolve, NullMempoolReturnsError) {
    rte_ether_addr dummy_mac{};
    auto result = dns::resolve(0, 0, nullptr, dummy_mac, dummy_mac, 0,
                               "example.com");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, eph::core::Error::InvalidConfig);
    EXPECT_NE(std::string_view{result.error().detail}.find("mempool"),
              std::string_view::npos);
}

TEST(DnsResolve, ZeroNameserverReturnsError) {
    rte_ether_addr dummy_mac{};
    DnsConfig cfg{.nameserver_ip = 0};
    // Use a non-null pool so we reach the nameserver check (pool check comes first)
    auto* fake_pool = reinterpret_cast<rte_mempool*>(0x1);
    auto result = dns::resolve(0, 0, fake_pool, dummy_mac, dummy_mac, 0,
                               "example.com", cfg);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, eph::core::Error::InvalidConfig);
    // detail is a static literal "Invalid DNS config" — the per-field
    // diagnostic ("nameserver_ip must not be 0") goes to the spdlog ERROR
    // line. We accept either form so the assertion remains stable across
    // future detail-string tweaks.
    auto sv = std::string_view{result.error().detail};
    EXPECT_TRUE(sv.find("Invalid DNS config") != std::string_view::npos ||
                sv.find("nameserver_ip") != std::string_view::npos)
        << "unexpected detail: '" << sv << "'";
}

// ─────────────────────────────────────────────────────────────────────────────
// QNAME edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsQnameEncode, TrailingDotReturnsZero) {
    // "example.com." has an empty label after the trailing dot
    uint8_t buf[256];
    EXPECT_EQ(encode_qname(buf, "example.com."), 0u);
}

TEST(DnsQnameEncode, LeadingDotReturnsZero) {
    uint8_t buf[256];
    EXPECT_EQ(encode_qname(buf, ".example.com"), 0u);
}

TEST(DnsQnameEncode, HostnameTooLongReturnsZero) {
    // 254 chars exceeds 253 limit
    std::string long_hostname;
    for (int i = 0; i < 50; ++i) {
        if (i > 0) long_hostname += '.';
        long_hostname += "abcd";
    }
    // Make it exceed 253
    while (long_hostname.size() <= 253) long_hostname += "x";
    uint8_t buf[512];
    EXPECT_EQ(encode_qname(buf, long_hostname), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// build_dns_query edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsBuildQuery, InvalidHostnameReturnsZero) {
    uint8_t buf[512];
    // Empty label in hostname → encode_qname fails → build_dns_query returns 0
    EXPECT_EQ(build_dns_query(buf, 0x1234, "bad..hostname"), 0u);
}

TEST(DnsBuildQuery, QueryContainsCorrectQtypeAndQclass) {
    uint8_t buf[512];
    size_t len = build_dns_query(buf, net::hton16(0x5678), "test.io");

    // Last 4 bytes should be QTYPE=A(1) + QCLASS=IN(1)
    ASSERT_GE(len, 4u);
    uint16_t qtype, qclass;
    std::memcpy(&qtype, &buf[len - 4], 2);
    std::memcpy(&qclass, &buf[len - 2], 2);
    EXPECT_EQ(net::ntoh16(qtype), kDnsTypeA);
    EXPECT_EQ(net::ntoh16(qclass), kDnsClassIn);
}

// ─────────────────────────────────────────────────────────────────────────────
// skip_dns_name edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsSkipName, PointerLoopReturnsZero) {
    // Two pointers pointing at each other: offset 0 → offset 2, offset 2 → offset 0
    uint8_t data[4] = {0xC0, 0x02, 0xC0, 0x00};
    size_t offset = skip_dns_name(data, 0, sizeof(data));
    EXPECT_EQ(offset, 0u);  // Should detect the loop via iteration limit
}

TEST(DnsSkipName, SelfReferencePointerReturnsZero) {
    // Pointer at offset 0 pointing to itself
    uint8_t data[2] = {0xC0, 0x00};
    size_t offset = skip_dns_name(data, 0, sizeof(data));
    EXPECT_EQ(offset, 0u);
}

TEST(DnsSkipName, TruncatedPointerReturnsZero) {
    // Pointer byte at end of data (missing second byte)
    uint8_t data[1] = {0xC0};
    size_t offset = skip_dns_name(data, 0, sizeof(data));
    EXPECT_EQ(offset, 0u);
}

TEST(DnsSkipName, PointerBeyondDataReturnsZero) {
    // Pointer to offset 100, but data is only 4 bytes
    uint8_t data[4] = {0xC0, 0x64, 0, 0};
    size_t offset = skip_dns_name(data, 0, sizeof(data));
    EXPECT_EQ(offset, 0u);
}

TEST(DnsSkipName, LabelExceedsBoundsReturnsZero) {
    // Label says 10 bytes but only 3 bytes remain
    uint8_t data[4] = {10, 'a', 'b', 'c'};
    size_t offset = skip_dns_name(data, 0, sizeof(data));
    EXPECT_EQ(offset, 0u);
}

TEST(DnsSkipName, EmptyNameJustRoot) {
    uint8_t data[1] = {0};
    size_t offset = skip_dns_name(data, 0, sizeof(data));
    EXPECT_EQ(offset, 1u);
}

TEST(DnsSkipName, InvalidLabelType) {
    // Label byte 0x80 is neither regular (< 64) nor pointer (0xC0)
    // It has top bit set but not both → label_len > 63 → return 0
    uint8_t data[4] = {0x80, 'a', 'b', 0};
    size_t offset = skip_dns_name(data, 0, sizeof(data));
    EXPECT_EQ(offset, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_dns_response edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsParseResponse, SkipsNonARecordFindsSecondAnswer) {
    // Build a response with: 1 CNAME record (type 5) then 1 A record
    uint16_t tx_id = net::hton16(0x9999);
    uint32_t expected_ip = 0x0A000001;  // 10.0.0.1

    std::vector<uint8_t> pkt;

    // DNS header: 2 answers
    DnsHeader hdr{};
    hdr.id       = tx_id;
    hdr.flags    = net::hton16(kDnsFlagQr | kDnsFlagRd);
    hdr.qd_count = net::hton16(1);
    hdr.an_count = net::hton16(2);
    hdr.ns_count = 0;
    hdr.ar_count = 0;
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&hdr),
               reinterpret_cast<uint8_t*>(&hdr) + sizeof(hdr));

    // Question: example.com
    uint8_t qname[256];
    size_t qname_len = encode_qname(qname, "example.com");
    pkt.insert(pkt.end(), qname, qname + qname_len);
    uint16_t qtype = net::hton16(kDnsTypeA);
    uint16_t qclass = net::hton16(kDnsClassIn);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&qtype),
               reinterpret_cast<uint8_t*>(&qtype) + 2);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&qclass),
               reinterpret_cast<uint8_t*>(&qclass) + 2);

    // Answer 1: CNAME (type 5), should be skipped
    uint16_t ptr = net::hton16(0xC00C);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ptr),
               reinterpret_cast<uint8_t*>(&ptr) + 2);
    uint16_t cname_type = net::hton16(5);  // CNAME
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&cname_type),
               reinterpret_cast<uint8_t*>(&cname_type) + 2);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&qclass),
               reinterpret_cast<uint8_t*>(&qclass) + 2);
    uint32_t ttl = net::hton32(300);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ttl),
               reinterpret_cast<uint8_t*>(&ttl) + 4);
    // CNAME RDATA: a pointer name (2 bytes)
    uint16_t cname_rdlen = net::hton16(2);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&cname_rdlen),
               reinterpret_cast<uint8_t*>(&cname_rdlen) + 2);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ptr),
               reinterpret_cast<uint8_t*>(&ptr) + 2);

    // Answer 2: A record
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ptr),
               reinterpret_cast<uint8_t*>(&ptr) + 2);
    uint16_t a_type = net::hton16(kDnsTypeA);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&a_type),
               reinterpret_cast<uint8_t*>(&a_type) + 2);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&qclass),
               reinterpret_cast<uint8_t*>(&qclass) + 2);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ttl),
               reinterpret_cast<uint8_t*>(&ttl) + 4);
    uint16_t a_rdlen = net::hton16(4);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&a_rdlen),
               reinterpret_cast<uint8_t*>(&a_rdlen) + 2);
    uint32_t ip_net = net::hton32(expected_ip);
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&ip_net),
               reinterpret_cast<uint8_t*>(&ip_net) + 4);

    auto result = parse_dns_response(pkt.data(), pkt.size(), tx_id);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(*result, expected_ip);
}

TEST(DnsParseResponse, TruncatedAnswerRrReturnsError) {
    // Build a valid header claiming 1 answer, but truncate the RR data
    uint16_t tx_id = net::hton16(0xAAAA);

    auto pkt = build_test_response(
        tx_id, kDnsFlagQr | kDnsFlagRd, 1, "example.com", 0x01020304);

    // Truncate: remove last 2 bytes (partial RDATA)
    pkt.resize(pkt.size() - 2);

    auto result = parse_dns_response(pkt.data(), pkt.size(), tx_id);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("RDATA exceeds"), std::string::npos);
}

TEST(DnsParseResponse, WrongRdlengthForARecordSkips) {
    // A record with rdlength=3 (should be 4) — should skip, not crash
    uint16_t tx_id = net::hton16(0xBBBB);
    auto pkt = build_test_response(
        tx_id, kDnsFlagQr | kDnsFlagRd, 1, "example.com", 0x01020304);

    // Patch rdlength from 4 to 3 (it's the 2 bytes before the last 4 bytes of RDATA)
    size_t rdlen_offset = pkt.size() - 4 - 2;  // 2 bytes before RDATA
    pkt[rdlen_offset] = 0;
    pkt[rdlen_offset + 1] = 3;

    auto result = parse_dns_response(pkt.data(), pkt.size(), tx_id);
    // Should fail: A record with rdlength != 4 is skipped, no other A record → no A record found
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("no A record"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// DnsConfig std::formatter
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// DnsConfig::warnings
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsConfigWarnings, DefaultConfigNoWarnings) {
    DnsConfig cfg{};
    auto w = cfg.warnings();
    EXPECT_TRUE(w.empty()) << "Unexpected warning: " << (w.empty() ? "" : w[0]);
}

TEST(DnsConfigWarnings, LoopbackNameserver) {
    DnsConfig cfg{.nameserver_ip = 0x7F000001}; // 127.0.0.1
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("loopback") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Expected loopback warning";
}

TEST(DnsConfigWarnings, ShortTimeout) {
    DnsConfig cfg{.timeout = std::chrono::milliseconds{200}};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("200ms") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Expected short timeout warning";
}

TEST(DnsConfigWarnings, NonStandardPort) {
    DnsConfig cfg{.port = 5353};
    auto w = cfg.warnings();
    bool found = false;
    for (const auto& msg : w) {
        if (msg.find("non-standard") != std::string::npos) { found = true; break; }
    }
    EXPECT_TRUE(found) << "Expected non-standard port warning";
}

TEST(DnsConfigWarnings, StandardPortNoWarning) {
    DnsConfig cfg{.port = 53};
    auto w = cfg.warnings();
    for (const auto& msg : w) {
        EXPECT_EQ(msg.find("non-standard"), std::string::npos)
            << "Standard port should not trigger warning";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DnsConfig std::formatter
// ─────────────────────────────────────────────────────────────────────────────

TEST(DnsConfig, EqualityDefault) {
    DnsConfig a{};
    DnsConfig b{};
    EXPECT_EQ(a, b);
}

TEST(DnsConfig, EqualityDifferentNameserver) {
    DnsConfig a{.nameserver_ip = 0x08080808};
    DnsConfig b{.nameserver_ip = 0x01010101};
    EXPECT_NE(a, b);
}

TEST(DnsConfig, EqualityDifferentPort) {
    DnsConfig a{.port = 53};
    DnsConfig b{.port = 5353};
    EXPECT_NE(a, b);
}

TEST(DnsConfig, EqualityDifferentTimeout) {
    DnsConfig a{.timeout = std::chrono::milliseconds{3000}};
    DnsConfig b{.timeout = std::chrono::milliseconds{1000}};
    EXPECT_NE(a, b);
}

TEST(DnsConstants, HeaderLength) {
    EXPECT_EQ(kDnsHeaderLen, 12u);
    EXPECT_EQ(sizeof(DnsHeader), kDnsHeaderLen);
}

TEST(DnsConstants, StandardPort) {
    EXPECT_EQ(kDnsPort, 53u);
}

TEST(DnsConstants, MaxPacketLength) {
    EXPECT_EQ(kMaxDnsPacketLen, 512u);
}

TEST(DnsConstants, FlagValues) {
    EXPECT_EQ(kDnsFlagQr, 0x8000u);
    EXPECT_EQ(kDnsFlagRd, 0x0100u);
    EXPECT_EQ(kDnsRcodeMask, 0x000Fu);
}

TEST(DnsConstants, RecordTypes) {
    EXPECT_EQ(kDnsTypeA, 1u);
    EXPECT_EQ(kDnsClassIn, 1u);
}

TEST(DnsConfig, StdFormatterContainsKeyFields) {
    DnsConfig cfg{
        .nameserver_ip = 0x01010101,  // 1.1.1.1
        .port = 5353,
        .timeout = std::chrono::milliseconds{750},
    };
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("DnsConfig"), std::string::npos);
    EXPECT_NE(s.find("1.1.1.1"), std::string::npos);
    EXPECT_NE(s.find("5353"), std::string::npos);
    EXPECT_NE(s.find("750ms"), std::string::npos);
}
