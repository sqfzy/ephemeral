/// @file test_http_request_crlf_injection.cpp
/// CRLF-injection tests for build_http_request method/host/path arguments.
///
/// The pre-existing test_http_client.cpp covers CRLF rejection in
/// extra_headers but never specifically tests the method/host/path
/// arguments — those got CRLF validation added in a later cleanup
/// batch (commit f647465 "fix(eph-net): reject CR/LF in
/// build_http_request method/host/path") with no specific test.
///
/// Without these tests a future refactor could remove the validation
/// without any test failure.

#include <string>

#include <gtest/gtest.h>

#include "eph/net/http_message.hpp"

using namespace eph::net;

// ═══════════════════════════════════════════════════════════════════════
// CRLF injection in method
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestCrlfInjection, MethodWithBareCrRejected) {
    auto r = build_http_request("GET\r", "host.com", "/path");
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("injection"), std::string::npos);
}

TEST(HttpRequestCrlfInjection, MethodWithBareLfRejected) {
    auto r = build_http_request("GET\nX-Smuggled: yes", "host.com", "/path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestCrlfInjection, MethodWithFullCrlfRejected) {
    auto r = build_http_request("GET\r\nHost: evil.com", "host.com", "/path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestCrlfInjection, NormalMethodAccepted) {
    auto r = build_http_request("GET", "host.com", "/path");
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
}

// ═══════════════════════════════════════════════════════════════════════
// CRLF injection in host
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestCrlfInjection, HostWithBareCrRejected) {
    auto r = build_http_request("GET", "evil\r.com", "/path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestCrlfInjection, HostWithBareLfRejected) {
    auto r = build_http_request("GET", "evil\nX-Hijack: 1", "/path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestCrlfInjection, HostWithFullCrlfRejected) {
    // The classic "request smuggling" payload: a host that injects a
    // new HTTP request after the Host: line.
    auto r = build_http_request(
        "GET", "evil.com\r\nX-Smuggled: yes\r\nGET /admin HTTP/1.1",
        "/path");
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("injection"), std::string::npos);
}

TEST(HttpRequestCrlfInjection, NormalHostAccepted) {
    auto r = build_http_request("GET", "exchange.binance.com", "/api/v3/time");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_NE(r->find("Host: exchange.binance.com"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// CRLF injection in path
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestCrlfInjection, PathWithBareCrRejected) {
    auto r = build_http_request("GET", "host.com", "/api\r/v1");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestCrlfInjection, PathWithBareLfRejected) {
    auto r = build_http_request("GET", "host.com", "/api\nGET /admin");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestCrlfInjection, PathWithFullCrlfRejected) {
    auto r = build_http_request(
        "GET", "host.com",
        "/legitimate\r\nHost: attacker.com\r\nX-Forwarded-For: pwned");
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("injection"), std::string::npos);
}

TEST(HttpRequestCrlfInjection, NormalPathWithQueryAccepted) {
    auto r = build_http_request("GET", "host.com",
                                 "/api/v3/depth?symbol=BTCUSDT&limit=100");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_NE(r->find("/api/v3/depth?symbol=BTCUSDT&limit=100"),
              std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// Combined: multiple injection vectors at once
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestCrlfInjection, AllThreeFieldsCleanAccepted) {
    auto r = build_http_request("POST", "api.exchange.com", "/v3/order");
    ASSERT_TRUE(r.has_value()) << r.error();
}

TEST(HttpRequestCrlfInjection, MethodCleanHostInjectedRejected) {
    auto r = build_http_request("POST", "api.exchange.com\r\nX: y", "/path");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestCrlfInjection, MethodCleanPathInjectedRejected) {
    auto r = build_http_request("POST", "api.exchange.com",
                                 "/path\r\nContent-Length: 0");
    EXPECT_FALSE(r.has_value());
}
