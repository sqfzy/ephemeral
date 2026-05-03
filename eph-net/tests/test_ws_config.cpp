/// @file test_ws_config.cpp
/// Unit tests for `eph::net::WsConfig` — validate() semantics plus the
/// default-constructed / disabled-vs-active behaviours.

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/net/ws_config.hpp"

using eph::core::Error;
using eph::net::HttpHeader;
using eph::net::WsConfig;
using namespace std::chrono_literals;

// ═══════════════════════════════════════════════════════════════════════════
// Default construction (disabled state)
// ═══════════════════════════════════════════════════════════════════════════

TEST(WsConfig, DefaultConstructedIsEmptyAndValid) {
    WsConfig cfg{};
    EXPECT_TRUE(cfg.empty()) << "default-constructed cfg must report empty";
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
    EXPECT_TRUE(cfg.path.empty());
    EXPECT_TRUE(cfg.host.empty());
    EXPECT_TRUE(cfg.extra_headers.empty());
    EXPECT_GT(cfg.timeout.count(), 0);
    EXPECT_TRUE(cfg.permessage_deflate); // default on
}

// ═══════════════════════════════════════════════════════════════════════════
// Active state — path non-empty
// ═══════════════════════════════════════════════════════════════════════════

TEST(WsConfig, NonEmptyPathDisablesEmpty) {
    WsConfig cfg{.path = "/ws/feed"};
    EXPECT_FALSE(cfg.empty());
}

TEST(WsConfig, ActiveCfgWithDefaultsIsValid) {
    WsConfig cfg{.path = "/ws/btcusdt@bookTicker", .host = "stream.example.com"};
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

TEST(WsConfig, ActiveCfgZeroTimeoutRejected) {
    WsConfig cfg{.path = "/ws", .timeout = 0ms};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgNegativeTimeoutRejected) {
    WsConfig cfg{.path = "/ws", .timeout = std::chrono::milliseconds{-1}};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

// Empty path with bad timeout — must still pass because the upgrade is
// disabled and the field is irrelevant. This is the "harmless leftover
// state" case (e.g. user partially populated then cleared `path`).
TEST(WsConfig, EmptyPathIgnoresBadTimeout) {
    WsConfig cfg{.path = "", .timeout = 0ms};
    EXPECT_TRUE(cfg.empty());
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

// Path missing leading '/' — RFC 7230 origin-form request-target must
// start with '/'. Without this guard a "ws/path" typo silently produces
// GET ws/path HTTP/1.1 on the wire, which most servers reject with 400.
// Catching the typo at config validation gives a pinpoint error.
TEST(WsConfig, ActiveCfgPathWithoutLeadingSlashRejected) {
    WsConfig cfg{.path = "ws/feed", .host = "stream.example.com"};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgRootSlashAccepted) {
    WsConfig cfg{.path = "/", .host = "stream.example.com"};
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

// ═══════════════════════════════════════════════════════════════════════════
// extra_headers carry-through
// ═══════════════════════════════════════════════════════════════════════════

TEST(WsConfig, ExtraHeadersStored) {
    WsConfig cfg{};
    cfg.path = "/ws";
    cfg.extra_headers.push_back(HttpHeader{"Authorization", "Bearer xyz"});
    cfg.extra_headers.push_back(HttpHeader{"X-Trace-Id", "abc-123"});
    ASSERT_EQ(cfg.extra_headers.size(), 2u);
    EXPECT_EQ(cfg.extra_headers[0].name, "Authorization");
    EXPECT_EQ(cfg.extra_headers[1].name, "X-Trace-Id");
    EXPECT_TRUE(cfg.validate().has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// permessage_deflate flag
// ═══════════════════════════════════════════════════════════════════════════

TEST(WsConfig, PermessageDeflateOptOut) {
    WsConfig cfg{.path = "/ws", .permessage_deflate = false};
    EXPECT_FALSE(cfg.permessage_deflate);
    EXPECT_TRUE(cfg.validate().has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Header / path injection — early validation at config time
// ═══════════════════════════════════════════════════════════════════════════
//
// These mirror the rejections that build_http_request enforces downstream
// (test_ws_upgrade_injection.cpp covers that layer). The point of duplicating
// the checks here is to surface the failure SYNCHRONOUSLY at config time —
// before any TCP / TLS handshake cost is paid — so an operator misconfiguring
// `host` with an injected header sees `InvalidConfig` from `cfg.validate()`
// instead of a `WsHandshakeFailed` returned by `Stream::create()` after
// hundreds of milliseconds of futile network I/O.

TEST(WsConfig, ActiveCfgPathWithCrlfRejected) {
    WsConfig cfg{.path = "/ws\r\nX-Inject: foo", .host = "h"};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgPathWithSpaceRejected) {
    // SP inside the request-target would split it into two HTTP/1.1 tokens.
    WsConfig cfg{.path = "/ws feed", .host = "h"};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgHostWithCrlfRejected) {
    // Header injection: a Host containing CR/LF would smuggle a second
    // header line into the Upgrade request.
    WsConfig cfg{.path = "/ws", .host = "stream.example.com\r\nX-Inject: foo"};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgHostWithBareLfRejected) {
    WsConfig cfg{.path = "/ws", .host = "stream.example.com\nX-Inject: foo"};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgHostWithNulRejected) {
    WsConfig cfg{.path = "/ws"};
    cfg.host = std::string{"stream.example.com\0X-Inject: foo", 32};
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgEmptyHostStillValid) {
    // Empty host falls back to tls.hostname / remote.to_string() at the
    // handshake layer; the config alone shouldn't reject it.
    WsConfig cfg{.path = "/ws", .host = ""};
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}

TEST(WsConfig, ActiveCfgExtraHeaderEmptyNameRejected) {
    WsConfig cfg{.path = "/ws", .host = "h"};
    cfg.extra_headers.push_back(HttpHeader{"", "value"});
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgExtraHeaderNameWithCrlfRejected) {
    WsConfig cfg{.path = "/ws", .host = "h"};
    cfg.extra_headers.push_back(HttpHeader{"Bad\r\nX-Inject", "value"});
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgExtraHeaderValueWithCrlfRejected) {
    WsConfig cfg{.path = "/ws", .host = "h"};
    cfg.extra_headers.push_back(
        HttpHeader{"Authorization", "Bearer xyz\r\nX-Inject: foo"});
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgExtraHeaderValueWithBareLfRejected) {
    WsConfig cfg{.path = "/ws", .host = "h"};
    cfg.extra_headers.push_back(
        HttpHeader{"Authorization", "Bearer xyz\nX-Inject: foo"});
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgExtraHeaderValueWithNulRejected) {
    WsConfig cfg{.path = "/ws", .host = "h"};
    cfg.extra_headers.push_back(
        HttpHeader{"Authorization", std::string{"Bearer\0xyz", 9}});
    auto v = cfg.validate();
    ASSERT_FALSE(v.has_value());
    EXPECT_EQ(v.error().code, Error::InvalidConfig);
}

TEST(WsConfig, ActiveCfgExtraHeaderWithOwsAccepted) {
    // RFC 7230 allows SP / HTAB inside header values — only CR / LF / NUL
    // are rejected. Verify a value with embedded spaces still validates.
    WsConfig cfg{.path = "/ws", .host = "h"};
    cfg.extra_headers.push_back(
        HttpHeader{"Authorization", "Bearer xyz with\tspaces"});
    auto v = cfg.validate();
    EXPECT_TRUE(v.has_value()) << (v ? "" : v.error().detail);
}
