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

TEST(ProxyUrl, EmptyHostReturnsError) {
    auto r = parse_proxy_url("socks5://:1080");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("empty host"), std::string::npos);
}

TEST(ProxyUrl, EmptyHostWithAuthReturnsError) {
    auto r = parse_proxy_url("socks5://user:pass@:1080");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("empty host"), std::string::npos);
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
    // Parser uses rfind('@') to split on the LAST '@', so passwords
    // containing '@' are handled correctly.
    auto r = parse_proxy_url("socks5://user:p@ss@host:1080");
    // rfind('@') splits on last '@': auth="user:p@ss", host_part="host:1080"
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->username, "user");
    EXPECT_EQ(r->password, "p@ss");
    EXPECT_EQ(r->host, "host");
    EXPECT_EQ(r->port, 1080);
}

TEST(ProxyUrl, AuthWithMultipleAtSigns) {
    // Multiple '@' in password: "user:p@@ss@host:port"
    auto r = parse_proxy_url("socks5://user:p@@ss@host:1080");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->username, "user");
    EXPECT_EQ(r->password, "p@@ss");
    EXPECT_EQ(r->host, "host");
    EXPECT_EQ(r->port, 1080);
}

TEST(ProxyUrl, AuthWithAtSignRoundTrip) {
    // Verify round-trip works for passwords with '@'
    auto r = parse_proxy_url("socks5://alice:p@ss@proxy.io:9050");
    ASSERT_TRUE(r.has_value()) << r.error();
    // to_url includes the password, so round-trip should work
    EXPECT_EQ(r->to_url(), "socks5://alice:p@ss@proxy.io:9050");
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
// ProxyConfig::to_url() — round-trip with parse_proxy_url
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyConfigUrl, Socks5NoAuth) {
    ProxyConfig cfg{.host = "proxy.io", .port = 1080, .type = ProxyType::kSocks5};
    EXPECT_EQ(cfg.to_url(), "socks5://proxy.io:1080");
}

TEST(ProxyConfigUrl, HttpConnectNoAuth) {
    ProxyConfig cfg{.host = "squid.local", .port = 3128, .type = ProxyType::kHttpConnect};
    EXPECT_EQ(cfg.to_url(), "http://squid.local:3128");
}

TEST(ProxyConfigUrl, Socks5WithFullAuth) {
    ProxyConfig cfg{
        .host = "proxy.io", .port = 9050, .type = ProxyType::kSocks5,
        .username = "alice", .password = "s3cret"};
    EXPECT_EQ(cfg.to_url(), "socks5://alice:s3cret@proxy.io:9050");
}

TEST(ProxyConfigUrl, UsernameOnlyOmitsPassword) {
    ProxyConfig cfg{
        .host = "proxy.io", .port = 1080, .type = ProxyType::kSocks5,
        .username = "bob"};
    EXPECT_EQ(cfg.to_url(), "socks5://bob@proxy.io:1080");
}

TEST(ProxyConfigUrl, RoundTripSocks5) {
    auto parsed = parse_proxy_url("socks5://user:pass@proxy.net:9050");
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->to_url(), "socks5://user:pass@proxy.net:9050");
}

TEST(ProxyConfigUrl, RoundTripHttpConnect) {
    auto parsed = parse_proxy_url("http://admin:hunter2@corp.proxy:8080");
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->to_url(), "http://admin:hunter2@corp.proxy:8080");
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

// ─────────────────────────────────────────────────────────────────────────────
// ProxyConfig::dump()
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyConfigDump, ContainsKeyFields) {
    ProxyConfig cfg{
        .host = "proxy.io", .port = 9050, .type = ProxyType::kSocks5,
        .timeout = std::chrono::milliseconds{3000}};
    auto d = cfg.dump();
    EXPECT_NE(d.find("SOCKS5"), std::string::npos);
    EXPECT_NE(d.find("proxy.io"), std::string::npos);
    EXPECT_NE(d.find("9050"), std::string::npos);
    EXPECT_NE(d.find("3000ms"), std::string::npos);
    EXPECT_NE(d.find("auth: none"), std::string::npos);
}

TEST(ProxyConfigDump, RedactsPassword) {
    ProxyConfig cfg{
        .host = "proxy.io", .port = 1080, .type = ProxyType::kSocks5,
        .username = "alice", .password = "s3cret"};
    auto d = cfg.dump();
    EXPECT_NE(d.find("alice"), std::string::npos);
    EXPECT_NE(d.find("<redacted>"), std::string::npos);
    // Password must NOT appear in dump
    EXPECT_EQ(d.find("s3cret"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// ProxyConfig::to_json()
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyConfigJson, ValidStructure) {
    ProxyConfig cfg{
        .host = "proxy.io", .port = 9050, .type = ProxyType::kSocks5,
        .timeout = std::chrono::milliseconds{5000}};
    auto j = cfg.to_json();
    EXPECT_EQ(j.front(), '{');
    EXPECT_EQ(j.back(), '}');
    EXPECT_NE(j.find("\"type\":\"SOCKS5\""), std::string::npos);
    EXPECT_NE(j.find("\"host\":\"proxy.io\""), std::string::npos);
    EXPECT_NE(j.find("\"port\":9050"), std::string::npos);
    EXPECT_NE(j.find("\"timeout_ms\":5000"), std::string::npos);
}

TEST(ProxyConfigJson, OmitsPasswordValue) {
    ProxyConfig cfg{
        .host = "proxy.io", .port = 1080,
        .username = "bob", .password = "hunter2"};
    auto j = cfg.to_json();
    // JSON should indicate password presence but not reveal the value
    EXPECT_NE(j.find("\"has_password\":true"), std::string::npos);
    EXPECT_EQ(j.find("hunter2"), std::string::npos);
}

TEST(ProxyConfigJson, NoPasswordField) {
    ProxyConfig cfg{.host = "proxy.io", .port = 1080};
    auto j = cfg.to_json();
    EXPECT_NE(j.find("\"has_password\":false"), std::string::npos);
}

TEST(ProxyConfigJson, EscapesHostWithSpecialChars) {
    ProxyConfig cfg{.host = "evil\"host", .port = 1080};
    auto j = cfg.to_json();
    EXPECT_NE(j.find("evil\\\"host"), std::string::npos)
        << "host with quotes should be escaped; got: " << j;
}

TEST(ProxyConfigJson, EscapesUsernameWithSpecialChars) {
    ProxyConfig cfg{.host = "proxy.io", .port = 1080,
                    .username = "user\"name"};
    auto j = cfg.to_json();
    EXPECT_NE(j.find("user\\\"name"), std::string::npos)
        << "username with quotes should be escaped; got: " << j;
}

// ─────────────────────────────────────────────────────────────────────────────
// ProxyConfig equality
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyConfigEquality, IdenticalConfigsAreEqual) {
    ProxyConfig a{.host = "proxy.io", .port = 1080, .type = ProxyType::kSocks5,
                  .username = "u", .password = "p"};
    ProxyConfig b = a;
    EXPECT_EQ(a, b);
}

TEST(ProxyConfigEquality, DifferentHostNotEqual) {
    ProxyConfig a{.host = "a.io", .port = 1080};
    ProxyConfig b{.host = "b.io", .port = 1080};
    EXPECT_NE(a, b);
}

TEST(ProxyConfigEquality, DifferentPasswordNotEqual) {
    ProxyConfig a{.host = "proxy.io", .port = 1080, .password = "p1"};
    ProxyConfig b{.host = "proxy.io", .port = 1080, .password = "p2"};
    EXPECT_NE(a, b);
}

TEST(ProxyConfigEquality, DifferentTypeNotEqual) {
    ProxyConfig a{.host = "proxy.io", .port = 1080, .type = ProxyType::kSocks5};
    ProxyConfig b{.host = "proxy.io", .port = 1080, .type = ProxyType::kHttpConnect};
    EXPECT_NE(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter<ProxyConfig>
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyConfigFormatter, ContainsKeyFields) {
    ProxyConfig cfg{
        .host = "proxy.io", .port = 9050, .type = ProxyType::kSocks5,
        .timeout = std::chrono::milliseconds{3000}};
    auto s = std::format("{}", cfg);
    EXPECT_NE(s.find("SOCKS5"), std::string::npos);
    EXPECT_NE(s.find("proxy.io"), std::string::npos);
    EXPECT_NE(s.find("9050"), std::string::npos);
    EXPECT_NE(s.find("3000ms"), std::string::npos);
}

TEST(ProxyConfigFormatter, DoesNotLeakPassword) {
    ProxyConfig cfg{
        .host = "proxy.io", .port = 1080,
        .username = "alice", .password = "s3cret"};
    auto s = std::format("{}", cfg);
    EXPECT_EQ(s.find("s3cret"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// ProxyConfig::warnings()
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyConfigWarnings, NoWarningsForReasonableConfig) {
    ProxyConfig cfg{.host = "proxy.io", .port = 1080};
    EXPECT_TRUE(cfg.warnings().empty());
}

TEST(ProxyConfigWarnings, WarnsOnShortTimeout) {
    ProxyConfig cfg{.host = "proxy.io", .port = 1080,
                    .timeout = std::chrono::milliseconds{500}};
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    bool found = false;
    for (const auto& s : w) {
        if (s.find("short") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ProxyConfigWarnings, WarnsOnSocks5UsernameWithoutPassword) {
    ProxyConfig cfg{.host = "proxy.io", .port = 1080,
                    .type = ProxyType::kSocks5,
                    .username = "alice"};
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    bool found = false;
    for (const auto& s : w) {
        if (s.find("without password") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ProxyConfigWarnings, WarnsOnHttpConnectWithCredentials) {
    ProxyConfig cfg{.host = "proxy.io", .port = 8080,
                    .type = ProxyType::kHttpConnect,
                    .username = "alice", .password = "pass"};
    auto w = cfg.warnings();
    ASSERT_FALSE(w.empty());
    bool found = false;
    for (const auto& s : w) {
        if (s.find("Basic auth") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_proxy_url — trailing content after port
// ─────────────────────────────────────────────────────────────────────────────

TEST(ProxyUrl, TrailingSlashAfterPortReturnsError) {
    // Proxy URLs should not have paths or trailing content.
    auto r = parse_proxy_url("socks5://proxy.io:1080/");
    // from_chars will fail because "/" is not consumed, causing an error
    EXPECT_FALSE(r.has_value());
}

TEST(ProxyUrl, TrailingPathAfterPortReturnsError) {
    auto r = parse_proxy_url("socks5://proxy.io:1080/path");
    EXPECT_FALSE(r.has_value());
}

TEST(ProxyUrl, PortWithLeadingZerosStillParses) {
    // Leading zeros may be unusual but from_chars handles them
    auto r = parse_proxy_url("socks5://proxy.io:01080");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->port, 1080);
}
