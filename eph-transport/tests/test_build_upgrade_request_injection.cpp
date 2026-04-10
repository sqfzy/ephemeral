/// @file test_build_upgrade_request_injection.cpp
/// CRLF/whitespace injection tests for http::build_upgrade_request.
///
/// build_upgrade_request previously checked only that host/path/key
/// were non-empty.  CR, LF, space, and tab in any of these fields
/// produce a malformed HTTP request line — the same desync vector
/// we fixed in build_http_request (eph-net) in earlier rounds.
/// This file demonstrates the gap and pins the fix.

#include <string>

#include <gtest/gtest.h>

#include "eph/transport/detail/http.hpp"

using namespace eph::net::http;

namespace {
constexpr std::string_view kSampleKey = "dGhlIHNhbXBsZSBub25jZQ==";
}

// ═══════════════════════════════════════════════════════════════════════
// host
// ═══════════════════════════════════════════════════════════════════════

TEST(BuildUpgradeInjection, HostWithCrlfRejected) {
    auto r = build_upgrade_request(
        "evil.com\r\nX-Smuggled: yes",
        "/ws", kSampleKey);
    EXPECT_FALSE(r.has_value());
}

TEST(BuildUpgradeInjection, HostWithBareLfRejected) {
    auto r = build_upgrade_request("evil\nhost", "/ws", kSampleKey);
    EXPECT_FALSE(r.has_value());
}

TEST(BuildUpgradeInjection, HostWithSpaceRejected) {
    auto r = build_upgrade_request("evil host", "/ws", kSampleKey);
    EXPECT_FALSE(r.has_value());
}

TEST(BuildUpgradeInjection, HostWithTabRejected) {
    auto r = build_upgrade_request("evil\thost", "/ws", kSampleKey);
    EXPECT_FALSE(r.has_value());
}

TEST(BuildUpgradeInjection, NormalHostAccepted) {
    auto r = build_upgrade_request("ws.example.com", "/ws", kSampleKey);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_NE(r->find("Host: ws.example.com\r\n"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// path
// ═══════════════════════════════════════════════════════════════════════

TEST(BuildUpgradeInjection, PathWithCrlfRejected) {
    auto r = build_upgrade_request(
        "host", "/ws\r\nHost: evil.com", kSampleKey);
    EXPECT_FALSE(r.has_value());
}

TEST(BuildUpgradeInjection, PathWithSpaceRejected) {
    auto r = build_upgrade_request("host", "/ws path", kSampleKey);
    EXPECT_FALSE(r.has_value());
}

TEST(BuildUpgradeInjection, PathWithTabRejected) {
    auto r = build_upgrade_request("host", "/ws\tpath", kSampleKey);
    EXPECT_FALSE(r.has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// ws_key
// ═══════════════════════════════════════════════════════════════════════

TEST(BuildUpgradeInjection, KeyWithCrlfRejected) {
    auto r = build_upgrade_request(
        "host", "/ws", "valid==\r\nX-Injected: yes");
    EXPECT_FALSE(r.has_value());
}

TEST(BuildUpgradeInjection, KeyWithSpaceRejected) {
    auto r = build_upgrade_request("host", "/ws", "valid key==");
    EXPECT_FALSE(r.has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// extra_headers (already protected — pin the existing behavior)
// ═══════════════════════════════════════════════════════════════════════

TEST(BuildUpgradeInjection, ExtraHeadersWithCrlfCrlfRejected) {
    auto r = build_upgrade_request("host", "/ws", kSampleKey,
        "X-Custom: ok\r\n\r\nGET /evil HTTP/1.1\r\n");
    EXPECT_FALSE(r.has_value());
}

TEST(BuildUpgradeInjection, ExtraHeadersWithBareLfRejected) {
    auto r = build_upgrade_request("host", "/ws", kSampleKey,
        "X-Custom: bad\nX-Smuggled: yes\r\n");
    EXPECT_FALSE(r.has_value());
}

TEST(BuildUpgradeInjection, NormalExtraHeadersAccepted) {
    auto r = build_upgrade_request("host", "/ws", kSampleKey,
        "Authorization: Bearer abc\r\nX-Custom: ok\r\n");
    ASSERT_TRUE(r.has_value()) << r.error();
}
