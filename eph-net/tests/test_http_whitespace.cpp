/// @file test_http_whitespace.cpp
/// @brief P0 security — RFC 7230 whitespace-in-token defense.
///
/// Migrated from v3.3 baseline `test_http_request_whitespace.cpp`.
///
/// RFC 7230 §3.1.1 request-line grammar:
///     method SP request-target SP HTTP-version CRLF
///
/// The SP delimiters are LITERAL single spaces — any embedded SP / HTAB
/// inside method or target produces a malformed line that downstream
/// parsers may interpret differently, which is a desync attack vector
/// in the exact same class as commits e336990 and e108fcb.
///
/// The builder rejects SP / HTAB / CR / LF / NUL in method and target
/// via `detail::is_valid_token` (method) and
/// `detail::builder_reject_unsafe_token` (target). This file pins the
/// defense so a future refactor can't silently remove it.

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
// Method
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestWhitespace, MethodWithSpaceRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(), "GET POST", "/path",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestWhitespace, MethodWithTabRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(), "GET\tPOST", "/path",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestWhitespace, MethodTrailingSpaceRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(), "GET ", "/path",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestWhitespace, NormalGetAccepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    EXPECT_TRUE(build_http_request(buf.data(), buf.size(), "GET", "/",
                                   std::span<const HttpHeader>(hdrs, 1)).has_value());
}

TEST(HttpRequestWhitespace, NormalPostAccepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    EXPECT_TRUE(build_http_request(buf.data(), buf.size(), "POST", "/",
                                   std::span<const HttpHeader>(hdrs, 1)).has_value());
}

TEST(HttpRequestWhitespace, NormalDeleteAccepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    EXPECT_TRUE(build_http_request(buf.data(), buf.size(), "DELETE", "/",
                                   std::span<const HttpHeader>(hdrs, 1)).has_value());
}

TEST(HttpRequestWhitespace, NormalPropfindAccepted) {
    // WebDAV PROPFIND is a valid tchar token.
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    EXPECT_TRUE(build_http_request(buf.data(), buf.size(), "PROPFIND", "/",
                                   std::span<const HttpHeader>(hdrs, 1)).has_value());
}

// ═══════════════════════════════════════════════════════════════════════
// Target
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestWhitespace, TargetWithSpaceRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(), "GET",
                                "/api/v1/depth?symbol=BTC USDT",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestWhitespace, TargetWithTabRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(), "GET",
                                "/api/v1\tdepth",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestWhitespace, TargetWithLeadingSpaceRejected) {
    auto buf = make_buf();
    auto r = build_http_request(buf.data(), buf.size(), "GET", " /path",
                                std::span<const HttpHeader>{});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestWhitespace, TargetWithEncodedSpaceAccepted) {
    // %20 is the proper percent-encoded space, valid in targets.
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    auto r = build_http_request(buf.data(), buf.size(), "GET",
                                "/api?q=BTC%20USDT",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value()) << r.error().detail;
}

TEST(HttpRequestWhitespace, TargetWithSpecialCharsAccepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    auto r = build_http_request(
        buf.data(), buf.size(), "GET",
        "/api/v3/depth?symbol=BTCUSDT&limit=100&extra=foo,bar;baz",
        std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value()) << r.error().detail;
}

// ═══════════════════════════════════════════════════════════════════════
// Host header value — must be valid as a header value (no CR/LF/NUL),
// not a token. Whitespace is technically allowed mid-value, but embedded
// space in the host ident is a pathological payload we keep defended.
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestWhitespace, HostHeaderWithPortAccepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "host.com:8443"}};
    EXPECT_TRUE(build_http_request(buf.data(), buf.size(), "GET", "/",
                                   std::span<const HttpHeader>(hdrs, 1)).has_value());
}

TEST(HttpRequestWhitespace, HostHeaderWithIpv4Accepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "192.168.1.1"}};
    EXPECT_TRUE(build_http_request(buf.data(), buf.size(), "GET", "/",
                                   std::span<const HttpHeader>(hdrs, 1)).has_value());
}

TEST(HttpRequestWhitespace, HostHeaderWithIpv6BracketsAccepted) {
    auto buf = make_buf();
    HttpHeader hdrs[] = {{"Host", "[::1]:8080"}};
    EXPECT_TRUE(build_http_request(buf.data(), buf.size(), "GET", "/",
                                   std::span<const HttpHeader>(hdrs, 1)).has_value());
}
