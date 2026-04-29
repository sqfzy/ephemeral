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
