/// @file test_http_crlf_injection.cpp
/// @brief P0 security — CRLF injection defense in build_http_request.
///
/// Migrated from baseline `test_http_request_crlf_injection.cpp`.
///
/// Regression cases for CR / LF / NUL / whitespace injection in the
/// method, target, header name, and header value arguments of
/// `build_http_request` (the builder entry point). Each case uses a
/// byte sequence that downstream proxies could misinterpret as a
/// framing boundary, which would allow request smuggling.
///
/// The builder (`eph::net::build_http_request(buf, cap, method,
/// target, headers, body)`) validates these inputs via
/// `detail::is_valid_token`, `detail::builder_reject_unsafe_token`, and
/// `detail::builder_reject_unsafe_value`; every baseline attack maps
/// to one of those three reject paths.

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

std::array<uint8_t, 512> make_buf() noexcept { return {}; }

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// CRLF / LF / CR injection in method
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestCrlfInjection, MethodWithBareCrRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(), "GET\r", "/path",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestCrlfInjection, MethodWithBareLfRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(),
                                "GET\nX-Smuggled: yes", "/path",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestCrlfInjection, MethodWithFullCrlfRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(),
                                "GET\r\nHost: evil.com", "/path",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestCrlfInjection, NormalMethodAccepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/path",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// CRLF / LF / CR injection in target
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestCrlfInjection, TargetWithBareCrRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/api\r/v1",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestCrlfInjection, TargetWithBareLfRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(), "GET",
                                "/api\nGET /admin",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestCrlfInjection, TargetWithFullCrlfRejected) {
    auto buf = make_buf();
    auto r = build_http_request(
        buf.data(), buf.size(), "GET",
        "/legitimate\r\nHost: attacker.com\r\nX-Forwarded-For: pwned",
        std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestCrlfInjection, TargetWithQueryAccepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    auto r = build_http_request(buf.data(), buf.size(), "GET",
                                "/api/v3/depth?symbol=BTCUSDT&limit=100",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// CRLF injection in header VALUES
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestCrlfInjection, HeaderValueWithCrlfRejected) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"X-Custom", "foo\r\nInjected: bar"}};
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestCrlfInjection, HeaderValueWithBareCrRejected) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"X-Custom", "foo\rbar"}};
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestCrlfInjection, HeaderValueWithBareLfRejected) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"X-Custom", "foo\nbar"}};
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestCrlfInjection, HeaderValueWithNulRejected) {
    auto buf = make_buf();
    std::string evil_val("foo\0bar", 7);
    HttpHeader hdrs[] = {{"X-Custom", evil_val}};
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ═══════════════════════════════════════════════════════════════════════
// CRLF injection in header NAMES
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestCrlfInjection, HeaderNameWithCrlfRejected) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"X-Evil\r\nInjected", "value"}};
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestCrlfInjection, HeaderNameWithSpaceRejected) {
    // Space is not a tchar.
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"X Custom", "value"}};
    auto r = build_http_request(buf.data(), buf.size(), "GET", "/",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Combined: multiple injection vectors at once
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestCrlfInjection, AllThreeFieldsCleanAccepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "api.exchange.com"}};
    auto r = build_http_request(buf.data(), buf.size(), "POST", "/v3/order",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value()) << r.error().detail;
}

TEST(HttpRequestCrlfInjection, CleanMethodInjectedTargetRejected) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "api.exchange.com"}};
    auto r = build_http_request(buf.data(), buf.size(), "POST",
                                "/path\r\nContent-Length: 0",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}
