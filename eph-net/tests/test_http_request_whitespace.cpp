/// @file test_http_request_whitespace.cpp
/// Whitespace-in-token tests for build_http_request.
///
/// RFC 7230 §3.1.1: the request-line is
///   method SP request-target SP HTTP-version CRLF
/// where the SP delimiters are LITERAL spaces.  Embedding a space
/// (or tab) inside method, request-target, or host produces a
/// malformed request line that downstream parsers may interpret
/// differently than us — desync attack surface.
///
/// We already reject CR/LF in these fields (commit f647465).  Add
/// whitespace rejection for completeness.

#include <string>

#include <gtest/gtest.h>

#include "eph/net/http_message.hpp"

using namespace eph::net;

// ═══════════════════════════════════════════════════════════════════════
// Method
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestWhitespace, MethodWithSpaceRejected) {
    auto r = build_http_request("GET POST", "host.com", "/path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestWhitespace, MethodWithTabRejected) {
    auto r = build_http_request("GET\tPOST", "host.com", "/path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestWhitespace, MethodTrailingSpaceRejected) {
    auto r = build_http_request("GET ", "host.com", "/path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestWhitespace, NormalMethodAccepted) {
    EXPECT_TRUE(build_http_request("GET",     "host.com", "/").has_value());
    EXPECT_TRUE(build_http_request("POST",    "host.com", "/").has_value());
    EXPECT_TRUE(build_http_request("DELETE",  "host.com", "/").has_value());
    EXPECT_TRUE(build_http_request("PROPFIND","host.com", "/").has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// Path
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestWhitespace, PathWithSpaceRejected) {
    auto r = build_http_request("GET", "host.com", "/api/v1/depth?symbol=BTC USDT");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestWhitespace, PathWithTabRejected) {
    auto r = build_http_request("GET", "host.com", "/api/v1\tdepth");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestWhitespace, PathWithLeadingSpaceRejected) {
    auto r = build_http_request("GET", "host.com", " /path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestWhitespace, NormalPathWithEncodedSpaceAccepted) {
    // %20 is the proper URL encoding for a space.
    auto r = build_http_request("GET", "host.com", "/api?q=BTC%20USDT");
    ASSERT_TRUE(r.has_value()) << r.error();
}

TEST(HttpRequestWhitespace, NormalPathWithSpecialCharsAccepted) {
    auto r = build_http_request("GET", "host.com",
                                 "/api/v3/depth?symbol=BTCUSDT&limit=100&extra=foo,bar;baz");
    ASSERT_TRUE(r.has_value()) << r.error();
}

// ═══════════════════════════════════════════════════════════════════════
// Host
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestWhitespace, HostWithSpaceRejected) {
    auto r = build_http_request("GET", "host com", "/path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestWhitespace, HostWithTabRejected) {
    auto r = build_http_request("GET", "host\tcom", "/path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestWhitespace, HostWithPortAccepted) {
    auto r = build_http_request("GET", "host.com:8443", "/path");
    ASSERT_TRUE(r.has_value()) << r.error();
}

TEST(HttpRequestWhitespace, HostWithIpv4Accepted) {
    auto r = build_http_request("GET", "192.168.1.1", "/path");
    ASSERT_TRUE(r.has_value()) << r.error();
}

TEST(HttpRequestWhitespace, HostWithIpv6BracketsAccepted) {
    auto r = build_http_request("GET", "[::1]:8080", "/path");
    ASSERT_TRUE(r.has_value()) << r.error();
}
