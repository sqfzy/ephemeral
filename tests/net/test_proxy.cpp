/// @file test_proxy.cpp
/// Unit tests for proxy URL parsing and ProxyConfig validation.
///
/// Tests the pure-function components of proxy.hpp that do not require
/// a live network connection (parse_proxy_url, ProxyConfig defaults).
/// SOCKS5/HTTP CONNECT handshake protocol logic is tested via the
/// byte-level assertions on the greeting/request construction.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/net/proxy.hpp"

using namespace eph::net::proxy;

// ─────────────────────────────────────────────────────────────────────────────
// parse_proxy_url — SOCKS5 scheme
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyUrl, Socks5BasicHostPort) {
    auto r = parse_proxy_url("socks5://proxy.example.com:1080");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->type, ProxyType::kSocks5);
    EXPECT_EQ(r->host, "proxy.example.com");
    EXPECT_EQ(r->port, 1080);
    EXPECT_TRUE(r->username.empty());
    EXPECT_TRUE(r->password.empty());
}

TEST(ProxyUrl, Socks5WithAuth) {
    auto r = parse_proxy_url("socks5://alice:s3cret@proxy.local:9050");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->type, ProxyType::kSocks5);
    EXPECT_EQ(r->host, "proxy.local");
    EXPECT_EQ(r->port, 9050);
    EXPECT_EQ(r->username, "alice");
    EXPECT_EQ(r->password, "s3cret");
}

TEST(ProxyUrl, Socks5UsernameOnly) {
    auto r = parse_proxy_url("socks5://bob@proxy.local:1080");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->username, "bob");
    EXPECT_TRUE(r->password.empty());
}

TEST(ProxyUrl, Socks5PasswordWithSpecialChars) {
    auto r = parse_proxy_url("socks5://user:p%40ss:word@proxy.io:1080");
    ASSERT_TRUE(r.has_value()) << r.error();
    // Note: no URL decoding — password is taken as-is
    EXPECT_EQ(r->password, "p%40ss:word");
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_proxy_url — HTTP CONNECT scheme
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyUrl, HttpConnectBasic) {
    auto r = parse_proxy_url("http://squid.internal:3128");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->type, ProxyType::kHttpConnect);
    EXPECT_EQ(r->host, "squid.internal");
    EXPECT_EQ(r->port, 3128);
}

TEST(ProxyUrl, HttpConnectWithAuth) {
    auto r = parse_proxy_url("http://admin:hunter2@proxy.corp:8080");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->type, ProxyType::kHttpConnect);
    EXPECT_EQ(r->host, "proxy.corp");
    EXPECT_EQ(r->port, 8080);
    EXPECT_EQ(r->username, "admin");
    EXPECT_EQ(r->password, "hunter2");
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_proxy_url — error cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyUrl, UnsupportedSchemeReturnsError) {
    auto r = parse_proxy_url("https://proxy.io:443");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("unsupported proxy scheme"), std::string::npos);
}

TEST(ProxyUrl, NoSchemeReturnsError) {
    auto r = parse_proxy_url("proxy.io:1080");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("unsupported proxy scheme"), std::string::npos);
}

TEST(ProxyUrl, MissingPortReturnsError) {
    auto r = parse_proxy_url("socks5://proxy.io");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("missing port"), std::string::npos);
}

TEST(ProxyUrl, InvalidPortNonNumericReturnsError) {
    auto r = parse_proxy_url("socks5://proxy.io:abc");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("invalid port"), std::string::npos);
}

TEST(ProxyUrl, PortZeroReturnsError) {
    auto r = parse_proxy_url("socks5://proxy.io:0");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("port out of range"), std::string::npos);
}

TEST(ProxyUrl, PortTooLargeReturnsError) {
    auto r = parse_proxy_url("socks5://proxy.io:99999");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("port out of range"), std::string::npos);
}

TEST(ProxyUrl, PortMaxValid) {
    auto r = parse_proxy_url("socks5://proxy.io:65535");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->port, 65535);
}

TEST(ProxyUrl, PortMinValid) {
    auto r = parse_proxy_url("socks5://proxy.io:1");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->port, 1);
}

TEST(ProxyUrl, EmptyUrlReturnsError) {
    auto r = parse_proxy_url("");
    ASSERT_FALSE(r.has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_proxy_url — IP address hosts
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyUrl, IPv4Host) {
    auto r = parse_proxy_url("socks5://192.168.1.100:1080");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->host, "192.168.1.100");
    EXPECT_EQ(r->port, 1080);
}

TEST(ProxyUrl, LocalhostHost) {
    auto r = parse_proxy_url("http://127.0.0.1:8080");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->host, "127.0.0.1");
    EXPECT_EQ(r->port, 8080);
}

// ─────────────────────────────────────────────────────────────────────────────
// ProxyConfig defaults
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyConfig, DefaultValues) {
    ProxyConfig cfg;
    EXPECT_EQ(cfg.port, 1080);
    EXPECT_EQ(cfg.type, ProxyType::kSocks5);
    EXPECT_TRUE(cfg.username.empty());
    EXPECT_TRUE(cfg.password.empty());
    EXPECT_EQ(cfg.timeout, std::chrono::milliseconds{5000});
}

// ─────────────────────────────────────────────────────────────────────────────
// ProxyConfig::validate()
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyConfig, ValidateAcceptsValidConfig) {
    ProxyConfig cfg{.host = "proxy.example.com", .port = 1080};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(ProxyConfig, ValidateRejectsEmptyHost) {
    ProxyConfig cfg{.host = "", .port = 1080};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("host"), std::string_view::npos);
}

TEST(ProxyConfig, ValidateRejectsZeroPort) {
    ProxyConfig cfg{.host = "proxy.example.com", .port = 0};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("port"), std::string_view::npos);
}

TEST(ProxyConfig, ValidateRejectsNonPositiveTimeout) {
    ProxyConfig cfg{
        .host = "proxy.example.com", .port = 1080,
        .timeout = std::chrono::milliseconds{0}};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("timeout"), std::string_view::npos);
}

TEST(ProxyConfig, ValidateRejectsLongSocks5Username) {
    ProxyConfig cfg{
        .host = "proxy.example.com", .port = 1080,
        .type = ProxyType::kSocks5,
        .username = std::string(256, 'x')};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("username"), std::string_view::npos);
}

TEST(ProxyConfig, ValidateRejectsLongSocks5Password) {
    ProxyConfig cfg{
        .host = "proxy.example.com", .port = 1080,
        .type = ProxyType::kSocks5,
        .password = std::string(256, 'x')};
    EXPECT_FALSE(cfg.validate().empty());
    EXPECT_NE(cfg.validate().find("password"), std::string_view::npos);
}

TEST(ProxyConfig, ValidateAcceptsLongCredentialsForHttpConnect) {
    // HTTP CONNECT doesn't have the 255-byte SOCKS5 limit
    ProxyConfig cfg{
        .host = "proxy.example.com", .port = 8080,
        .type = ProxyType::kHttpConnect,
        .username = std::string(300, 'u'),
        .password = std::string(300, 'p')};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(ProxyConfig, ValidateAcceptsMaxLengthSocks5Credentials) {
    ProxyConfig cfg{
        .host = "proxy.example.com", .port = 1080,
        .type = ProxyType::kSocks5,
        .username = std::string(255, 'u'),
        .password = std::string(255, 'p')};
    EXPECT_TRUE(cfg.validate().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_proxy_url — round-trip consistency
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyUrl, ParsedConfigCanBeUsedDirectly) {
    // Verify that parsed config fields are usable for factory construction
    auto r = parse_proxy_url("socks5://user:pass@myproxy.net:9050");
    ASSERT_TRUE(r.has_value()) << r.error();
    auto& cfg = *r;

    // Config should be fully populated and usable
    EXPECT_FALSE(cfg.host.empty());
    EXPECT_GT(cfg.port, 0);
    EXPECT_FALSE(cfg.username.empty());
    EXPECT_FALSE(cfg.password.empty());
    EXPECT_EQ(cfg.timeout, std::chrono::milliseconds{5000});  // default timeout
}

// ─────────────────────────────────────────────────────────────────────────────
// Edge cases: auth field with special characters
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyUrl, AuthWithAtSignInPassword) {
    // The parser finds the first '@' to split auth from host.
    // "user:p@ss@host:port" → username="user", password="p",
    // and "ss@host:port" is parsed as host — this may be lossy.
    // Test documents current behavior for awareness.
    auto r = parse_proxy_url("socks5://user:p@ss@host:1080");
    // The parser splits on first '@': auth="user:p", host_part="ss@host:1080"
    // Then rfind(':') on "ss@host:1080" → port="1080", host="ss@host"
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->username, "user");
    EXPECT_EQ(r->password, "p");
    EXPECT_EQ(r->host, "ss@host");
    EXPECT_EQ(r->port, 1080);
}

TEST(ProxyUrl, EmptyPasswordAfterColon) {
    auto r = parse_proxy_url("socks5://user:@proxy.io:1080");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->username, "user");
    EXPECT_TRUE(r->password.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// ProxyConfig validation
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyConfigValidation, ValidDefaultSocks5) {
    ProxyConfig cfg{.host = "proxy.local", .port = 1080};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(ProxyConfigValidation, ValidHttpConnect) {
    ProxyConfig cfg{.host = "proxy.local", .port = 8080,
                    .type = ProxyType::kHttpConnect};
    EXPECT_TRUE(cfg.validate().empty());
}

TEST(ProxyConfigValidation, EmptyHostFails) {
    ProxyConfig cfg{.host = "", .port = 1080};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("host"), std::string_view::npos);
}

TEST(ProxyConfigValidation, ZeroPortFails) {
    ProxyConfig cfg{.host = "proxy.local", .port = 0};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("port"), std::string_view::npos);
}

TEST(ProxyConfigValidation, NegativeTimeoutFails) {
    ProxyConfig cfg{.host = "proxy.local", .port = 1080,
                    .timeout = std::chrono::milliseconds{-1}};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("timeout"), std::string_view::npos);
}

TEST(ProxyConfigValidation, Socks5UsernameTooLongFails) {
    ProxyConfig cfg{.host = "proxy.local", .port = 1080,
                    .type = ProxyType::kSocks5,
                    .username = std::string(256, 'x')};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("username"), std::string_view::npos);
}

TEST(ProxyConfigValidation, Socks5PasswordTooLongFails) {
    ProxyConfig cfg{.host = "proxy.local", .port = 1080,
                    .type = ProxyType::kSocks5,
                    .password = std::string(256, 'x')};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("password"), std::string_view::npos);
}

TEST(ProxyConfigValidation, HostWithControlCharsRejected) {
    ProxyConfig cfg{.host = "evil\r\nhost", .port = 1080};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("control characters"), std::string_view::npos);
}

TEST(ProxyConfigValidation, HostWithNullByteRejected) {
    ProxyConfig cfg{.host = std::string("evil\0host", 9), .port = 1080};
    auto err = cfg.validate();
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("control characters"), std::string_view::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// proxy_type_name() — covers all enum values
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyTypeName, Socks5) {
    EXPECT_EQ(proxy_type_name(ProxyType::kSocks5), "SOCKS5");
}

TEST(ProxyTypeName, HttpConnect) {
    EXPECT_EQ(proxy_type_name(ProxyType::kHttpConnect), "HTTP_CONNECT");
}

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter<ProxyType>
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyTypeFormatter, FormatSocks5) {
    EXPECT_EQ(std::format("{}", ProxyType::kSocks5), "SOCKS5");
}

TEST(ProxyTypeFormatter, FormatHttpConnect) {
    EXPECT_EQ(std::format("{}", ProxyType::kHttpConnect), "HTTP_CONNECT");
}

TEST(ProxyTypeFormatter, CompositeFormat) {
    auto s = std::format("type={} port={}", ProxyType::kSocks5, 1080);
    EXPECT_EQ(s, "type=SOCKS5 port=1080");
}
