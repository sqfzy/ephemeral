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
    ProxyConfig p1{.host = "x", .port = 1};
    EXPECT_TRUE(p1.validate().has_value());

    // HTTP default.
    ProxyConfig p80{.host = "x", .port = 80};
    EXPECT_TRUE(p80.validate().has_value());

    // HTTPS default.
    ProxyConfig p443{.host = "x", .port = 443};
    EXPECT_TRUE(p443.validate().has_value());

    // Common corporate proxy ports.
    ProxyConfig p8080{.host = "x", .port = 8080};
    EXPECT_TRUE(p8080.validate().has_value());
    ProxyConfig p3128{.host = "x", .port = 3128};
    EXPECT_TRUE(p3128.validate().has_value());

    // Upper bound — max uint16_t.
    ProxyConfig pmax{.host = "x", .port = 65535};
    EXPECT_TRUE(pmax.validate().has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Long host (DNS 253-char limit)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, LongHostWithinDnsLimitAccepted) {
    // Build a 253-char name of repeating "a.b.c.d..." labels. This is at
    // the RFC 1035 maximum. validate() doesn't inspect length, but we
    // assert the struct tolerates long hosts without truncation / UB.
    std::string host;
    while (host.size() + 10 <= 253) {
        host += "abcdefghi.";
    }
    while (host.size() < 253) host.push_back('x');
    ASSERT_EQ(host.size(), 253u);
    ProxyConfig cfg{.host = host, .port = 8080};
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

// ═══════════════════════════════════════════════════════════════════════════
// Basic auth pair coupling
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, UserWithoutPasswordRejected) {
    ProxyConfig cfg{.host = "x", .port = 1};
    cfg.basic_auth_user = "alice";
    // pass deliberately left unset
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(ProxyConfig, PasswordWithoutUserRejected) {
    ProxyConfig cfg{.host = "x", .port = 1};
    cfg.basic_auth_pass = "secret";
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(ProxyConfig, BothAuthFieldsSetAccepted) {
    ProxyConfig cfg{.host = "x", .port = 1};
    cfg.basic_auth_user = "alice";
    cfg.basic_auth_pass = "wonderland";
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

// ═══════════════════════════════════════════════════════════════════════════
// Timeout validation
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, ZeroTimeoutRejected) {
    ProxyConfig cfg{.host = "x", .port = 1};
    cfg.timeout = std::chrono::milliseconds{0};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(ProxyConfig, NegativeTimeoutRejected) {
    // A negative duration is nonsensical; validate() should bail.
    ProxyConfig cfg{.host = "x", .port = 1};
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
    ProxyConfig a{.host = "proxy.local", .port = 8080};
    a.basic_auth_user = "alice";
    a.basic_auth_pass = "secret";
    a.timeout         = std::chrono::milliseconds{5000};

    ProxyConfig b = a;  // copy
    EXPECT_EQ(b.host, "proxy.local");
    EXPECT_EQ(b.port, 8080);
    ASSERT_TRUE(b.basic_auth_user.has_value());
    EXPECT_EQ(*b.basic_auth_user, "alice");
    ASSERT_TRUE(b.basic_auth_pass.has_value());
    EXPECT_EQ(*b.basic_auth_pass, "secret");
    EXPECT_EQ(b.timeout.count(), 5000);
}

TEST(ProxyConfig, MoveConstructTransfersStrings) {
    ProxyConfig a{.host = "proxy.local", .port = 8080};
    a.basic_auth_user = "alice";
    a.basic_auth_pass = "secret";

    ProxyConfig b = std::move(a);
    EXPECT_EQ(b.host, "proxy.local");
    ASSERT_TRUE(b.basic_auth_user.has_value());
    EXPECT_EQ(*b.basic_auth_user, "alice");
    ASSERT_TRUE(b.basic_auth_pass.has_value());
    EXPECT_EQ(*b.basic_auth_pass, "secret");
}

// ═══════════════════════════════════════════════════════════════════════════
// Validate is noexcept
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, ValidateIsNoexcept) {
    ProxyConfig cfg{.host = "x", .port = 1};
    static_assert(noexcept(cfg.validate()),
                  "ProxyConfig::validate must be noexcept");
    EXPECT_TRUE(cfg.validate().has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Optional fields default-initialized correctly
// ═══════════════════════════════════════════════════════════════════════════

TEST(ProxyConfig, OptionalDefaultsEmpty) {
    ProxyConfig cfg{.host = "x", .port = 1};
    EXPECT_FALSE(cfg.basic_auth_user.has_value());
    EXPECT_FALSE(cfg.basic_auth_pass.has_value());
}
