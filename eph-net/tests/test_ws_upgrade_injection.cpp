/// @file test_ws_upgrade_injection.cpp
/// @brief P0 security — WS upgrade header injection defense.
///
/// Migrated from v3.3 baseline
/// `eph-transport/tests/test_build_upgrade_request_injection.cpp`.
///
/// The baseline had a dedicated `build_upgrade_request(host, path, ws_key,
/// extra_headers)` helper for building the HTTP/1.1 Upgrade request that
/// kicks off a WebSocket handshake. The injection defense lives in the generic
/// `build_http_request` — WS upgrade is simply "GET / HTTP/1.1" with the
/// four mandatory headers (Host, Upgrade, Connection, Sec-WebSocket-Key,
/// Sec-WebSocket-Version) plus optional extras.
///
/// Every baseline attack byte sequence is preserved verbatim; the test
/// wraps it in the corresponding `build_http_request` call and asserts
/// rejection with `Error::CodecBad`. When 9.5 lands and `build_upgrade_request`
/// returns, these tests can be rerouted to it with one find/replace.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/net/http.hpp"

using eph::core::Error;
using eph::net::HttpHeader;
using eph::net::build_http_request;

namespace {

constexpr std::string_view kSampleKey = "dGhlIHNhbXBsZSBub25jZQ==";

/// @brief Build the fixed header set a WS upgrade needs, parameterized on
///        host / ws_key. Tests override individual entries when they want
///        to inject into one specific field.
std::array<HttpHeader, 5> ws_headers(std::string_view host,
                                     std::string_view ws_key) noexcept {
    return {{
        {"Host",                  host},
        {"Upgrade",               "websocket"},
        {"Connection",            "Upgrade"},
        {"Sec-WebSocket-Key",     ws_key},
        {"Sec-WebSocket-Version", "13"},
    }};
}

std::array<uint8_t, 1024> make_buf() noexcept { return {}; }

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// host (Host header value)
// ═══════════════════════════════════════════════════════════════════════

TEST(WsUpgradeInjection, HostWithCrlfRejected) {
    auto buf = make_buf();
    auto hdrs = ws_headers("evil.com\r\nX-Smuggled: yes", kSampleKey);
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws",
                                std::span<const HttpHeader>(hdrs));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(WsUpgradeInjection, HostWithBareLfRejected) {
    auto buf = make_buf();
    auto hdrs = ws_headers("evil\nhost", kSampleKey);
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws",
                                std::span<const HttpHeader>(hdrs));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(WsUpgradeInjection, HostWithNormalValueAccepted) {
    auto buf = make_buf();
    auto hdrs = ws_headers("ws.example.com", kSampleKey);
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws",
                                std::span<const HttpHeader>(hdrs));
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    std::string_view s(reinterpret_cast<const char*>(buf.data()), *r);
    EXPECT_NE(s.find("Host: ws.example.com\r\n"), std::string_view::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// path (request-target)
// ═══════════════════════════════════════════════════════════════════════

TEST(WsUpgradeInjection, PathWithCrlfRejected) {
    auto buf = make_buf();
    auto hdrs = ws_headers("host", kSampleKey);
    auto r = build_http_request(buf.data(), buf.size(), "GET",
                                "/ws\r\nHost: evil.com",
                                std::span<const HttpHeader>(hdrs));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(WsUpgradeInjection, PathWithSpaceRejected) {
    auto buf = make_buf();
    auto hdrs = ws_headers("host", kSampleKey);
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws path",
                                std::span<const HttpHeader>(hdrs));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(WsUpgradeInjection, PathWithTabRejected) {
    auto buf = make_buf();
    auto hdrs = ws_headers("host", kSampleKey);
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws\tpath",
                                std::span<const HttpHeader>(hdrs));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Sec-WebSocket-Key value
// ═══════════════════════════════════════════════════════════════════════

TEST(WsUpgradeInjection, KeyWithCrlfRejected) {
    auto buf = make_buf();
    auto hdrs = ws_headers("host", "valid==\r\nX-Injected: yes");
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws",
                                std::span<const HttpHeader>(hdrs));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(WsUpgradeInjection, KeyWithBareCrRejected) {
    auto buf = make_buf();
    auto hdrs = ws_headers("host", "valid\rkey==");
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws",
                                std::span<const HttpHeader>(hdrs));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(WsUpgradeInjection, KeyWithNulRejected) {
    auto buf = make_buf();
    std::string nul_key("valid\0key==", 11);
    HttpHeader hdrs[] = {
        {"Host", "host"},
        {"Upgrade", "websocket"},
        {"Connection", "Upgrade"},
        {"Sec-WebSocket-Key", nul_key},
        {"Sec-WebSocket-Version", "13"},
    };
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws",
                                std::span<const HttpHeader>(hdrs, 5));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Extra caller-supplied headers
// ═══════════════════════════════════════════════════════════════════════

TEST(WsUpgradeInjection, ExtraHeaderValueWithCrlfRejected) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {
        {"Host", "host"},
        {"Upgrade", "websocket"},
        {"Connection", "Upgrade"},
        {"Sec-WebSocket-Key", kSampleKey},
        {"Sec-WebSocket-Version", "13"},
        {"X-Custom", "ok\r\n\r\nGET /evil HTTP/1.1"},
    };
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws",
                                std::span<const HttpHeader>(hdrs, 6));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(WsUpgradeInjection, ExtraHeaderValueWithBareLfRejected) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {
        {"Host", "host"},
        {"Upgrade", "websocket"},
        {"Connection", "Upgrade"},
        {"Sec-WebSocket-Key", kSampleKey},
        {"Sec-WebSocket-Version", "13"},
        {"X-Custom", "bad\nX-Smuggled: yes"},
    };
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws",
                                std::span<const HttpHeader>(hdrs, 6));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(WsUpgradeInjection, ExtraHeaderNameWithCrlfRejected) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {
        {"Host", "host"},
        {"Upgrade", "websocket"},
        {"Connection", "Upgrade"},
        {"Sec-WebSocket-Key", kSampleKey},
        {"Sec-WebSocket-Version", "13"},
        {"X-Evil\r\nInjected", "v"},
    };
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws",
                                std::span<const HttpHeader>(hdrs, 6));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(WsUpgradeInjection, NormalExtraHeadersAccepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {
        {"Host", "host"},
        {"Upgrade", "websocket"},
        {"Connection", "Upgrade"},
        {"Sec-WebSocket-Key", kSampleKey},
        {"Sec-WebSocket-Version", "13"},
        {"Authorization", "Bearer abc"},
        {"X-Custom", "ok"},
    };
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/ws",
                                std::span<const HttpHeader>(hdrs, 7));
    ASSERT_TRUE(r.has_value()) << r.error().detail;
}

// ═══════════════════════════════════════════════════════════════════════
// Positive round-trip: a canonical clean upgrade request serializes
// ═══════════════════════════════════════════════════════════════════════

TEST(WsUpgradeInjection, CanonicalUpgradeRequestSerializes) {
    auto buf = make_buf();
    auto hdrs = ws_headers("stream.binance.com", kSampleKey);
    auto r = build_http_request(buf.data(), buf.size(), "GET",
                                "/ws/btcusdt@bookTicker",
                                std::span<const HttpHeader>(hdrs));
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    std::string_view s(reinterpret_cast<const char*>(buf.data()), *r);
    EXPECT_NE(s.find("GET /ws/btcusdt@bookTicker HTTP/1.1\r\n"),
              std::string_view::npos);
    EXPECT_NE(s.find("Upgrade: websocket\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("Connection: Upgrade\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("Sec-WebSocket-Version: 13\r\n"), std::string_view::npos);
}
