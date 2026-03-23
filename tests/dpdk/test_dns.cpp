#include <gtest/gtest.h>

#include "dpdk_test_env.hpp" // IWYU pragma: keep
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
    ASSERT_EQ(len, 10u);  // \x09 l o c a l h o s t \x00
    EXPECT_EQ(buf[0], 9);
    EXPECT_EQ(buf[10 - 1], 0);
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

TEST(DnsResolve, EmptyHostnameReturnsError) {
    rte_ether_addr dummy_mac{};
    auto result = dns::resolve(0, 0, nullptr, dummy_mac, dummy_mac, 0, "");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("empty"), std::string::npos);
}

TEST(DnsResolve, NullMempoolReturnsError) {
    rte_ether_addr dummy_mac{};
    auto result = dns::resolve(0, 0, nullptr, dummy_mac, dummy_mac, 0,
                               "example.com");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("mempool"), std::string::npos);
}
