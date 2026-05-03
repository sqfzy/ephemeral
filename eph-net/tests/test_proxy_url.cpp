/// @file test_proxy_url.cpp
/// Unit tests for `eph::net::ProxyConfig` — validate() semantics plus
/// the copy/move/default-construction shape of the struct.


#include <chrono>
#include <string>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "eph/core/detail/base64.hpp"
#include "eph/core/error.hpp"
#include "eph/net/proxy.hpp"

using eph::core::Error;
using eph::core::ErrorInfo;
using eph::net::ProxyConfig;

// ═══════════════════════════════════════════════════════════════════════════
// Default construction
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, DefaultConstructedIsInvalidDueToEmptyHost) {
    ProxyConfig cfg{};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
    // Sanity: default-constructed auth + timeout fields have the documented
    // shapes — optional empty and a positive timeout.
    EXPECT_FALSE(cfg.basic_auth_user.has_value());
    EXPECT_FALSE(cfg.basic_auth_pass.has_value());
    EXPECT_GT(cfg.timeout.count(), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// host / port validation
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, EmptyHostRejected) {
    ProxyConfig cfg{.host = "", .port = 8080};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(ProxyConfig, ZeroPortRejected) {
    ProxyConfig cfg{.host = "proxy.example.com", .port = 0};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(ProxyConfig, ValidMinimalConfigAccepted) {
    ProxyConfig cfg{.host = "10.0.0.1", .port = 3128};
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

// ═══════════════════════════════════════════════════════════════════════════
// Port boundary values
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, PortBoundaryValuesAccepted) {
    // Smallest legal port is 1 (0 is reserved and we treat it as invalid).
    ProxyConfig p1{.host = "10.0.0.1", .port = 1};
    EXPECT_TRUE(p1.validate().has_value());

    // HTTP default.
    ProxyConfig p80{.host = "10.0.0.1", .port = 80};
    EXPECT_TRUE(p80.validate().has_value());

    // HTTPS default.
    ProxyConfig p443{.host = "10.0.0.1", .port = 443};
    EXPECT_TRUE(p443.validate().has_value());

    // Common corporate proxy ports.
    ProxyConfig p8080{.host = "10.0.0.1", .port = 8080};
    EXPECT_TRUE(p8080.validate().has_value());
    ProxyConfig p3128{.host = "10.0.0.1", .port = 3128};
    EXPECT_TRUE(p3128.validate().has_value());

    // Upper bound — max uint16_t.
    ProxyConfig pmax{.host = "10.0.0.1", .port = 65535};
    EXPECT_TRUE(pmax.validate().has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Long host (DNS 253-char limit)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, HostnameRejectedAtValidateTime) {
    // The doc string declares `host` MUST be a dotted-quad IPv4 literal.
    // Pre-fix this was only enforced at KernelTcpStream::create — a caller
    // that pre-validate()d would see "OK", then burn a TCP setup roundtrip
    // before learning the hostname was unusable. validate() now surfaces
    // the contract up-front, mirroring what `Ipv4Addr::parse` would reject.
    ProxyConfig cfg{.host = "proxy.example.com", .port = 8080};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(ProxyConfig, MalformedIpv4Rejected) {
    // 999.0.0.1 is syntactically a dotted-quad but the leftmost octet is
    // out of range. Ipv4Addr::parse rejects it; ProxyConfig::validate must
    // surface the same rejection rather than passing through.
    ProxyConfig cfg{.host = "999.0.0.1", .port = 8080};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

// ═══════════════════════════════════════════════════════════════════════════
// Basic auth pair coupling
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, UserWithoutPasswordRejected) {
    ProxyConfig cfg{.host = "10.0.0.1", .port = 1};
    cfg.basic_auth_user = "alice";
    // pass deliberately left unset
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(ProxyConfig, PasswordWithoutUserRejected) {
    ProxyConfig cfg{.host = "10.0.0.1", .port = 1};
    cfg.basic_auth_pass = "secret";
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(ProxyConfig, BothAuthFieldsSetAccepted) {
    ProxyConfig cfg{.host = "10.0.0.1", .port = 1};
    cfg.basic_auth_user = "alice";
    cfg.basic_auth_pass = "wonderland";
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

// Empty-string credentials must be rejected — has_value() distinguishes
// "auth requested" from "no auth", but a defaulted std::string sneaks
// through as has_value()==true with size()==0, producing
// `Authorization: Basic Og==` (base64 of ":") which some proxies treat
// as anonymous.
TEST(ProxyConfig, EmptyUserRejectedWhenAuthRequested) {
    ProxyConfig cfg{.host = "10.0.0.1", .port = 1};
    cfg.basic_auth_user = "";          // empty string, not nullopt
    cfg.basic_auth_pass = "wonderland";
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(ProxyConfig, EmptyPassRejectedWhenAuthRequested) {
    ProxyConfig cfg{.host = "10.0.0.1", .port = 1};
    cfg.basic_auth_user = "alice";
    cfg.basic_auth_pass = "";          // empty string, not nullopt
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

// ═══════════════════════════════════════════════════════════════════════════
// Timeout validation
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, ZeroTimeoutRejected) {
    ProxyConfig cfg{.host = "10.0.0.1", .port = 1};
    cfg.timeout = std::chrono::milliseconds{0};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(ProxyConfig, NegativeTimeoutRejected) {
    // A negative duration is nonsensical; validate() should bail.
    ProxyConfig cfg{.host = "10.0.0.1", .port = 1};
    cfg.timeout = std::chrono::milliseconds{-1};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

// ═══════════════════════════════════════════════════════════════════════════
// Basic auth base64 round-trip (RFC 7617 worked example)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, RFC7617Base64Example) {
    // RFC 7617 §2 worked example: "Aladdin:open sesame" →
    // "QWxhZGRpbjpvcGVuIHNlc2FtZQ=="
    std::string joined = "Aladdin:open sesame";
    std::string encoded = eph::core::detail::base64_encode(
        reinterpret_cast<const uint8_t*>(joined.data()), joined.size());
    EXPECT_EQ(encoded, "QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
}

TEST(ProxyConfig, SimpleUserPassBase64Example) {
    // A common smoke-test vector used in many HTTP proxy test suites.
    std::string joined = "user:pass";
    std::string encoded = eph::core::detail::base64_encode(
        reinterpret_cast<const uint8_t*>(joined.data()), joined.size());
    EXPECT_EQ(encoded, "dXNlcjpwYXNz");
}

// ═══════════════════════════════════════════════════════════════════════════
// Copy / move semantics
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, CopyConstructPreservesFields) {
    ProxyConfig a{.host = "192.168.1.1", .port = 8080};
    a.basic_auth_user = "alice";
    a.basic_auth_pass = "secret";
    a.timeout         = std::chrono::milliseconds{5000};

    ProxyConfig b = a;  // copy
    EXPECT_EQ(b.host, "192.168.1.1");
    EXPECT_EQ(b.port, 8080);
    ASSERT_TRUE(b.basic_auth_user.has_value());
    EXPECT_EQ(*b.basic_auth_user, "alice");
    ASSERT_TRUE(b.basic_auth_pass.has_value());
    EXPECT_EQ(*b.basic_auth_pass, "secret");
    EXPECT_EQ(b.timeout.count(), 5000);
}

TEST(ProxyConfig, MoveConstructTransfersStrings) {
    ProxyConfig a{.host = "192.168.1.1", .port = 8080};
    a.basic_auth_user = "alice";
    a.basic_auth_pass = "secret";

    ProxyConfig b = std::move(a);
    EXPECT_EQ(b.host, "192.168.1.1");
    ASSERT_TRUE(b.basic_auth_user.has_value());
    EXPECT_EQ(*b.basic_auth_user, "alice");
    ASSERT_TRUE(b.basic_auth_pass.has_value());
    EXPECT_EQ(*b.basic_auth_pass, "secret");
}

// ═══════════════════════════════════════════════════════════════════════════
// Validate is noexcept
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, ValidateIsNoexcept) {
    ProxyConfig cfg{.host = "10.0.0.1", .port = 1};
    static_assert(noexcept(cfg.validate()),
                  "ProxyConfig::validate must be noexcept");
    EXPECT_TRUE(cfg.validate().has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Optional fields default-initialized correctly
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, OptionalDefaultsEmpty) {
    ProxyConfig cfg{.host = "10.0.0.1", .port = 1};
    EXPECT_FALSE(cfg.basic_auth_user.has_value());
    EXPECT_FALSE(cfg.basic_auth_pass.has_value());
}
