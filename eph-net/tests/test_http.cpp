/// @file test_http.cpp
/// @brief Full migration of HTTP/1.x parser regression suite (sub-phase 9.4).
///
/// Started life as the 9.3 smoke set (33 cases) and is now expanded with
/// parser-level regression cases pulled from the v3.3 baseline
/// (test_http.cpp, test_http_client.cpp, test_http_response_complete_adv.cpp).
/// Anything touching Transfer-Encoding / chunked / cookies / Set-Cookie /
/// redirect / Expect: 100-continue / multipart is skipped per plan §D-1 —
/// those cases either migrate to test_http_te_edge.cpp (inverted to prove
/// outright rejection) or are dropped.
///
/// All input byte sequences are copied verbatim from baseline; only the
/// parser call and return-value handling are rewritten to the new
/// `parse_http_request(bytes, header_storage)` →
/// `std::expected<std::optional<ParseResult<T>>, ErrorInfo>` shape.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "eph/core/error.hpp"
#include "eph/net/http.hpp"

using eph::core::Error;
using eph::core::ErrorInfo;
using eph::net::HttpHeader;
using eph::net::HttpRequest;
using eph::net::HttpResponse;
using eph::net::ParseResult;
using eph::net::parse_http_request;
using eph::net::parse_http_response;
using eph::net::build_http_request;
using eph::net::build_http_response;

namespace {

/// @brief Convenience: wrap a string literal as a span<const uint8_t>.
std::span<const uint8_t> as_bytes(std::string_view s) noexcept {
    return std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

/// @brief Reusable default-size header storage for tests.
using HdrStorage = std::array<HttpHeader, 16>;

/// @brief Look up a header by case-insensitive name in a parsed request/response.
std::string_view find_hdr(std::span<const HttpHeader> hs,
                          std::string_view            name) noexcept {
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i];
            char cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
            if (ca != cb) return false;
        }
        return true;
    };
    for (const auto& h : hs) {
        if (ieq(h.name, name)) return h.value;
    }
    return {};
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Positive parse cases — requests (smoke + migration)
// ─────────────────────────────────────────────────────────────────────────────

TEST(HttpParseRequest, MinimalGet) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.method, "GET");
    EXPECT_EQ((*r)->value.target, "/");
    EXPECT_EQ((*r)->value.version_minor, 1);
    ASSERT_EQ((*r)->value.headers.size(), 1u);
    EXPECT_EQ((*r)->value.headers[0].name,  "Host");
    EXPECT_EQ((*r)->value.headers[0].value, "example.com");
    EXPECT_EQ((*r)->value.body.size(), 0u);
    EXPECT_EQ((*r)->consumed, wire.size());
}

TEST(HttpParseRequest, GetWithContentLengthZero) {
    constexpr std::string_view wire =
        "GET /empty HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 0u);
    EXPECT_EQ((*r)->consumed, wire.size());
}

TEST(HttpParseRequest, PostWithContentLengthMatchingBody) {
    constexpr std::string_view wire =
        "POST /v3/order HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "Content-Length: 11\r\n\r\n"
        "hello=world";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.method, "POST");
    EXPECT_EQ((*r)->value.target, "/v3/order");
    EXPECT_EQ((*r)->value.body.size(), 11u);
    EXPECT_EQ(std::string_view(
                  reinterpret_cast<const char*>((*r)->value.body.data()),
                  (*r)->value.body.size()),
              "hello=world");
    EXPECT_EQ((*r)->consumed, wire.size());
}

TEST(HttpParseRequest, IncompleteHeaderReturnsNone) {
    constexpr std::string_view wire = "GET / HTTP/1.1\r\nHost: x\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value()); // need more
}

TEST(HttpParseRequest, IncompleteBodyReturnsNone) {
    // Content-Length claims 100, only 50 body bytes present.
    std::string wire = "POST / HTTP/1.1\r\nContent-Length: 100\r\n\r\n";
    wire.append(50, 'x');
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value()); // need more
}

TEST(HttpParseRequest, ConnectMethodSupported) {
    // Needed for Sub-phase 9.6 HTTP proxy support.
    constexpr std::string_view wire =
        "CONNECT stream.binance.com:443 HTTP/1.1\r\n"
        "Host: stream.binance.com:443\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.method, "CONNECT");
    EXPECT_EQ((*r)->value.target, "stream.binance.com:443");
}

TEST(HttpParseRequest, MultipleHeaders) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: eph/0.1\r\n"
        "Accept: */*\r\n"
        "X-API-Key: abc123\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    ASSERT_EQ((*r)->value.headers.size(), 4u);
    EXPECT_EQ((*r)->value.headers[1].name,  "User-Agent");
    EXPECT_EQ((*r)->value.headers[1].value, "eph/0.1");
    EXPECT_EQ((*r)->value.headers[3].name,  "X-API-Key");
    EXPECT_EQ((*r)->value.headers[3].value, "abc123");
}

TEST(HttpParseRequest, Http10) {
    constexpr std::string_view wire = "GET / HTTP/1.0\r\nHost: a\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.version_minor, 0);
}

TEST(HttpParseRequest, Http11VersionMinorField) {
    constexpr std::string_view wire = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.version_minor, 1);
}

TEST(HttpParseRequest, EmptyBufferReturnsNone) {
    HdrStorage hdrs{};
    auto r = parse_http_request(std::span<const uint8_t>{}, hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(HttpParseRequest, BufferShorterThanStartLineReturnsNone) {
    constexpr std::string_view wire = "GET /";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(HttpParseRequest, DeleteMethod) {
    // Regression: baseline test_http_client.cpp DeleteMethod.
    constexpr std::string_view wire =
        "DELETE /api/v1/order/123 HTTP/1.1\r\nHost: api.exchange.com\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.method, "DELETE");
    EXPECT_EQ((*r)->value.target, "/api/v1/order/123");
}

TEST(HttpParseRequest, PutMethodWithBody) {
    constexpr std::string_view wire =
        "PUT /resource HTTP/1.1\r\n"
        "Host: host.com\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 16\r\n\r\n"
        "{\"updated\":true}";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.method, "PUT");
    EXPECT_EQ((*r)->value.body.size(), 16u);
}

TEST(HttpParseRequest, PropfindMethod) {
    constexpr std::string_view wire =
        "PROPFIND / HTTP/1.1\r\nHost: host\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.method, "PROPFIND");
}

TEST(HttpParseRequest, QueryStringInTarget) {
    constexpr std::string_view wire =
        "GET /api/v3/depth?symbol=BTCUSDT&limit=5 HTTP/1.1\r\n"
        "Host: api.binance.com\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.target, "/api/v3/depth?symbol=BTCUSDT&limit=5");
}

TEST(HttpParseRequest, UrlEncodedSpaceInQuery) {
    // %20 is the proper URL encoding for a space; parser must accept.
    constexpr std::string_view wire =
        "GET /api?q=BTC%20USDT HTTP/1.1\r\nHost: host\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.target, "/api?q=BTC%20USDT");
}

TEST(HttpParseRequest, TargetWithSpecialCharsAccepted) {
    constexpr std::string_view wire =
        "GET /api/v3/depth?symbol=BTCUSDT&limit=100&extra=foo,bar;baz "
        "HTTP/1.1\r\nHost: h\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
}

TEST(HttpParseRequest, HostHeaderWithPortPreserved) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nHost: host.com:8443\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "Host"), "host.com:8443");
}

TEST(HttpParseRequest, HostHeaderIpv4) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nHost: 192.168.1.1\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
}

TEST(HttpParseRequest, HostHeaderIpv6Brackets) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nHost: [::1]:8080\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
}

TEST(HttpParseRequest, HeaderValueTrailingWhitespaceTrimmed) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nX-Val:  \t hello  \r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-Val"), "hello");
}

TEST(HttpParseRequest, HeaderEmptyValueAccepted) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nX-Empty:\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-Empty"), "");
}

TEST(HttpParseRequest, HeaderWhitespaceOnlyValueTrimmedToEmpty) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nX-Blank:   \t  \r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-Blank"), "");
}

TEST(HttpParseRequest, CaseInsensitiveContentLengthHonored) {
    // Regression: ensure CL parsing is case-insensitive.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\ncontent-length: 5\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 5u);
}

TEST(HttpParseRequest, CaseInsensitiveMixedCaseContentLength) {
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nCoNtEnT-LeNgTh: 5\r\n\r\nworld";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 5u);
}

TEST(HttpParseRequest, ContentLengthWithLeadingOwsTrimmed) {
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length:    5\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 5u);
}

TEST(HttpParseRequest, ContentLengthLeadingZerosAccepted) {
    // Pin current behavior (matches baseline IsCompleteAdv.ContentLengthLeadingZerosAccepted).
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length: 005\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 5u);
}

TEST(HttpParseRequest, DuplicateContentLengthIdenticalAccepted) {
    // RFC 7230 §3.3.2 lenient interpretation: identical duplicates OK.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 5u);
}

TEST(HttpParseRequest, DuplicateContentLengthDifferingRejected) {
    // Smuggling defense: conflicting CL values.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 99\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, ContentLengthEmbeddedSpaceRejected) {
    // Adversarial: "5 0" — proxies may interpret as 50, we reject.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length: 5 0\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, ContentLengthEmbeddedTabRejected) {
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length: 5\t0\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, ContentLengthHexValueRejected) {
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length: 0x05\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, ContentLengthLeadingPlusRejected) {
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length: +5\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, ContentLengthHugeRejected) {
    // Larger than kMaxBodySize (16 MiB) → CodecOverflow.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length: 999999999999\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

TEST(HttpParseRequest, BodyContainingCrlfCrlfBytes) {
    // Once CL-delimited, the body may contain \r\n\r\n without confusing framing.
    std::string body = "before\r\n\r\nmiddle\r\n\r\nafter";
    std::string wire =
        "POST / HTTP/1.1\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), body.size());
}

TEST(HttpParseRequest, BinaryBody) {
    // Body with null bytes and binary data.
    std::string wire =
        "POST / HTTP/1.1\r\nContent-Length: 8\r\n\r\n";
    wire.append("\x00\x01\x02\x03\x04\x05\x06\x07", 8);
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 8u);
}

TEST(HttpParseRequest, LargeBody) {
    // Baseline LargeBodyIncludesContentLength — 10 000 bytes.
    std::string body(10000, 'x');
    std::string wire =
        "POST /upload HTTP/1.1\r\n"
        "Content-Length: 10000\r\n\r\n" + body;
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 10000u);
}

TEST(HttpParseRequest, IncompleteHeadersRetry) {
    // Baseline IncompleteHeaders (adapted): no trailing \r\n\r\n yet —
    // parser must report "need more" rather than erroring.
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nContent-Length: 5\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value()); // need more
}

// ─────────────────────────────────────────────────────────────────────────────
// Positive parse cases — responses
// ─────────────────────────────────────────────────────────────────────────────

TEST(HttpParseResponse, Ok200WithContentLengthBody) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n\r\n"
        "{\"ok\":\"true\"}";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 200u);
    EXPECT_EQ((*r)->value.reason_phrase, "OK");
    EXPECT_EQ((*r)->value.version_minor, 1);
    EXPECT_EQ((*r)->value.body.size(), 13u);
    EXPECT_EQ((*r)->consumed, wire.size());
}

TEST(HttpParseResponse, NotFound404) {
    constexpr std::string_view wire =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 404u);
    EXPECT_EQ((*r)->value.reason_phrase, "Not Found");
    EXPECT_EQ((*r)->value.body.size(), 0u);
}

TEST(HttpParseResponse, InternalServerError500) {
    constexpr std::string_view wire =
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 500u);
    EXPECT_EQ((*r)->value.reason_phrase, "Internal Server Error");
}

TEST(HttpParseResponse, Created201) {
    constexpr std::string_view wire =
        "HTTP/1.1 201 Created\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 25\r\n\r\n"
        "{\"orderId\":\"12345\",\"ok\":1}";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 201u);
    EXPECT_EQ((*r)->value.body.size(), 25u);
}

TEST(HttpParseResponse, NoContent204IsBodyless) {
    // 204 is bodyless per RFC — body should be empty even without CL.
    constexpr std::string_view wire = "HTTP/1.1 204 No Content\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 204u);
    EXPECT_EQ((*r)->value.body.size(), 0u);
}

TEST(HttpParseResponse, NotModified304IsBodyless) {
    constexpr std::string_view wire = "HTTP/1.1 304 Not Modified\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 304u);
    EXPECT_EQ((*r)->value.body.size(), 0u);
}

TEST(HttpParseResponse, Informational100IsBodyless) {
    constexpr std::string_view wire = "HTTP/1.1 100 Continue\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 100u);
}

TEST(HttpParseResponse, StatusCodeOnlyNoReason) {
    // RFC 7230 allows empty reason-phrase.
    constexpr std::string_view wire =
        "HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 200u);
    EXPECT_EQ((*r)->value.reason_phrase, "");
}

TEST(HttpParseResponse, AllValidThreeDigitCodesAccepted) {
    for (int code : {100, 200, 204, 301, 404, 500, 599}) {
        std::string wire = "HTTP/1.1 " + std::to_string(code) + " OK\r\n";
        if (code != 100 && code != 204 && code != 304) {
            wire += "Content-Length: 0\r\n";
        }
        wire += "\r\n";
        HdrStorage hdrs{};
        auto r = parse_http_response(as_bytes(wire), hdrs);
        ASSERT_TRUE(r.has_value()) << "code " << code << ": " << r.error().detail;
        ASSERT_TRUE(r->has_value());
        EXPECT_EQ((*r)->value.status_code, code);
    }
}

TEST(HttpParseResponse, BodyLargerThanContentLengthTruncated) {
    // Parser honors Content-Length: consumes exactly CL bytes, caller keeps rest.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloextra";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 5u);
    EXPECT_LT((*r)->consumed, wire.size());
}

TEST(HttpParseResponse, MultipleHeaders) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "X-RateLimit-Remaining: 1199\r\n"
        "X-RateLimit-Reset: 1640000000\r\n"
        "Content-Length: 2\r\n\r\n"
        "{}";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-RateLimit-Remaining"), "1199");
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-RateLimit-Reset"), "1640000000");
}

TEST(HttpParseResponse, IncompleteHeadersReturnsNone) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value()); // need more
}

TEST(HttpParseResponse, MalformedStatusLineRejected) {
    constexpr std::string_view wire = "GARBAGE\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseResponse, NonNumericStatusCodeRejected) {
    constexpr std::string_view wire = "HTTP/1.1 XYZ Bad\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseResponse, StatusCodeTrailingGarbageRejected) {
    // RFC 7230 §3.1.2 — exactly 3 digits. "200xyz" must fail.
    constexpr std::string_view wire = "HTTP/1.1 200xyz OK\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseResponse, StatusCodeLeadingPlusRejected) {
    constexpr std::string_view wire = "HTTP/1.1 +200 OK\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseResponse, StatusCodeNegativeRejected) {
    constexpr std::string_view wire = "HTTP/1.1 -1 Bad\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseResponse, StatusCodeTwoDigitsRejected) {
    constexpr std::string_view wire = "HTTP/1.1 20 OK\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseResponse, StatusCodeFourDigitsRejected) {
    constexpr std::string_view wire = "HTTP/1.1 2000 OK\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseResponse, EmptyBufferReturnsNone) {
    HdrStorage hdrs{};
    auto r = parse_http_response(std::span<const uint8_t>{}, hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(HttpParseResponse, IncompleteBodyReturnsNone) {
    std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n";
    wire.append(10, 'x');
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value()); // need more
}

TEST(HttpParseResponse, BinaryBody) {
    std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n";
    wire.append("\x00\x01\x02\x03\x04\x05\x06\x07", 8);
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 8u);
}

TEST(HttpParseResponse, NoCLNoBodylessStatusRejected) {
    // No CL, no TE, status 200 — we can't frame, must reject.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nServer: test\r\n\r\nsome body";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseResponse, OnlyHeadersNoCLNoBody) {
    constexpr std::string_view wire = "HTTP/1.1 200 OK\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    // No CL, not bodyless (200) → reject.
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseResponse, Http10StatusLine) {
    constexpr std::string_view wire =
        "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.version_minor, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Negative parse cases — security & correctness
// ─────────────────────────────────────────────────────────────────────────────

TEST(HttpParseResponse, RejectsTransferEncodingChunked) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, RejectsAnyTransferEncoding) {
    // Per D-1 we reject Transfer-Encoding regardless of value.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, RejectsContentLengthWithNonDigit) {
    // Regression for commit 282f0e2.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length: 5x\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, RejectsInvalidHeaderNameWithCRLF) {
    // Header name must be a token; no CR/LF ever permitted inside a single
    // header line — CRLF terminates the line first, so any attempted
    // injection shows up as a malformed subsequent line.
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nHost\r\n: evil\r\n\r\n"; // CR in name position
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, RejectsBareLFInHeaders) {
    // Bare LF at header start is an injection vector — proxies that
    // accept lone LF as a line terminator see different framing than
    // proxies that require CRLF. We reject.
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nHost: a\r\n\nHeader: evil\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, TrimsTrailingOwsInFieldName) {
    // Regression for commit e108fcb: "Content-Length : 5" must be treated
    // as a Content-Length header so a downstream proxy that strips the
    // trailing SP can't disagree with us on framing.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length : 5\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 5u);
    ASSERT_EQ((*r)->value.headers.size(), 1u);
    EXPECT_EQ((*r)->value.headers[0].name, "Content-Length");
}

TEST(HttpParseRequest, ContentLengthWithTabBeforeColonTrimmed) {
    // Baseline test_http_response_complete_adv: tab is also OWS.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\nContent-Length\t: 4\r\n\r\nabcd";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 4u);
}

TEST(HttpParseResponse, RejectsNonThreeDigitStatusCode) {
    // Regression for commit 5137fee.
    constexpr std::string_view wire =
        "HTTP/1.1 20 OK\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, RejectsClTeCombo) {
    // Request smuggling vector: CL + TE combined.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\n"
        "Content-Length: 4\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "abcd";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, RejectsTeClOrder) {
    // TE before CL — same attack, different order.
    constexpr std::string_view wire =
        "POST / HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 4\r\n\r\n"
        "abcd";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseResponse, RejectsClTeCombo) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, EmptyHeaderStorageWithHeaders) {
    // No room for any headers → overflow.
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    std::array<HttpHeader, 0> hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

TEST(HttpParseRequest, EmptyStartLineRejected) {
    constexpr std::string_view wire = "\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, MissingVersionRejected) {
    constexpr std::string_view wire = "GET /\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, UnsupportedHttp2Version) {
    constexpr std::string_view wire = "GET / HTTP/2.0\r\nHost: h\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, EmptyMethodRejected) {
    constexpr std::string_view wire = " / HTTP/1.1\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, EmptyTargetRejected) {
    constexpr std::string_view wire = "GET  HTTP/1.1\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, TargetWithCtrlCharRejected) {
    // Embedded CTL in target is a defense against log-injection and
    // proxy desync.
    std::string wire = "GET /foo";
    wire.push_back('\x01');
    wire += " HTTP/1.1\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpParseRequest, InvalidMethodTokenRejected) {
    // Method must be tchar only; "GE T" is not (space inside).
    // This is caught by sp1/sp2 split though — the second "word" parses
    // as target. Test an invalid method char instead.
    std::string wire = "GE@T / HTTP/1.1\r\n\r\n"; // '@' is not tchar
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ─────────────────────────────────────────────────────────────────────────────
// Builder tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(HttpBuildRequest, SimpleGet) {
    std::array<uint8_t, 256> out{};
    HttpHeader hdrs[] = {{"Host", "example.com"}};
    auto r = build_http_request(out.data(), out.size(), "GET", "/",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value());
    std::string_view s(reinterpret_cast<const char*>(out.data()), *r);
    EXPECT_EQ(s, "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
}

TEST(HttpBuildRequest, WithHeadersAndBody) {
    std::array<uint8_t, 512> out{};
    HttpHeader hdrs[] = {
        {"Host", "api.example.com"},
        {"Content-Type", "application/json"},
        {"Content-Length", "7"},
    };
    constexpr std::string_view body_sv = "payload";
    auto r = build_http_request(out.data(), out.size(), "POST", "/order",
                                std::span<const HttpHeader>(hdrs, 3),
                                as_bytes(body_sv));
    ASSERT_TRUE(r.has_value());
    std::string_view s(reinterpret_cast<const char*>(out.data()), *r);
    EXPECT_NE(s.find("POST /order HTTP/1.1\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("Content-Length: 7\r\n"), std::string_view::npos);
    EXPECT_TRUE(s.ends_with("\r\n\r\npayload"));
}

TEST(HttpBuildRequest, RejectsCrlfInHeaderValue) {
    std::array<uint8_t, 256> out{};
    HttpHeader hdrs[] = {{"X-Evil", "foo\r\nInjected: bar"}};
    auto r = build_http_request(out.data(), out.size(), "GET", "/",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpBuildRequest, RejectsNulInMethod) {
    std::array<uint8_t, 256> out{};
    std::string evil_method("GET\0POST", 8);
    auto r = build_http_request(out.data(), out.size(), evil_method, "/", {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpBuildRequest, RejectsCrlfInTarget) {
    std::array<uint8_t, 256> out{};
    auto r = build_http_request(out.data(), out.size(), "GET",
                                "/foo\r\nInjected: bar", {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpBuildRequest, BufferTooSmallReturnsOverflow) {
    std::array<uint8_t, 8> out{}; // ridiculously small
    HttpHeader hdrs[] = {{"Host", "example.com"}};
    auto r = build_http_request(out.data(), out.size(), "GET", "/",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

TEST(HttpBuildResponse, SimpleOk200) {
    std::array<uint8_t, 256> out{};
    HttpHeader hdrs[] = {{"Content-Length", "0"}};
    auto r = build_http_response(out.data(), out.size(), 200, "OK",
                                 std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value());
    std::string_view s(reinterpret_cast<const char*>(out.data()), *r);
    EXPECT_EQ(s, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
}

TEST(HttpBuildResponse, RejectsStatusOutOfRangeLow) {
    std::array<uint8_t, 256> out{};
    auto r = build_http_response(out.data(), out.size(), 99, "x", {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpBuildResponse, RejectsStatusOutOfRangeHigh) {
    std::array<uint8_t, 256> out{};
    auto r = build_http_response(out.data(), out.size(), 600, "x", {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ─────────────────────────────────────────────────────────────────────────────
// Round-trip + compile-time sanity
// ─────────────────────────────────────────────────────────────────────────────

TEST(HttpRoundTrip, BuildThenParseRequest) {
    std::array<uint8_t, 512> out{};
    HttpHeader build_hdrs[] = {
        {"Host", "api.bin.local"},
        {"User-Agent", "eph/0.1"},
        {"Content-Length", "5"},
    };
    constexpr std::string_view body_sv = "HELLO";
    auto built = build_http_request(
        out.data(), out.size(), "POST", "/echo",
        std::span<const HttpHeader>(build_hdrs, 3), as_bytes(body_sv));
    ASSERT_TRUE(built.has_value());

    HdrStorage parsed_hdrs{};
    auto parsed = parse_http_request(
        std::span<const uint8_t>(out.data(), *built), parsed_hdrs);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->has_value());
    EXPECT_EQ((*parsed)->value.method, "POST");
    EXPECT_EQ((*parsed)->value.target, "/echo");
    EXPECT_EQ((*parsed)->value.version_minor, 1);
    ASSERT_EQ((*parsed)->value.headers.size(), 3u);
    EXPECT_EQ((*parsed)->value.headers[1].name, "User-Agent");
    EXPECT_EQ((*parsed)->value.headers[1].value, "eph/0.1");
    EXPECT_EQ((*parsed)->value.body.size(), 5u);
    EXPECT_EQ((*parsed)->consumed, *built);
}

TEST(HttpRoundTrip, BuildThenParseResponse) {
    std::array<uint8_t, 512> out{};
    HttpHeader build_hdrs[] = {
        {"Content-Type", "application/json"},
        {"Content-Length", "13"},
    };
    constexpr std::string_view body_sv = "{\"ok\":\"true\"}";
    auto built = build_http_response(
        out.data(), out.size(), 200, "OK",
        std::span<const HttpHeader>(build_hdrs, 2), as_bytes(body_sv));
    ASSERT_TRUE(built.has_value());

    HdrStorage parsed_hdrs{};
    auto parsed = parse_http_response(
        std::span<const uint8_t>(out.data(), *built), parsed_hdrs);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->has_value());
    EXPECT_EQ((*parsed)->value.status_code, 200u);
    EXPECT_EQ((*parsed)->value.reason_phrase, "OK");
    EXPECT_EQ((*parsed)->value.body.size(), 13u);
    EXPECT_EQ((*parsed)->consumed, *built);
}

TEST(HttpStaticAsserts, ParseReturnTypesMatchPlan) {
    // Compile-time validation that the parser signature matches the plan.
    using ReqRet = decltype(parse_http_request(
        std::declval<std::span<const uint8_t>>(),
        std::declval<std::span<HttpHeader>>()));
    using RspRet = decltype(parse_http_response(
        std::declval<std::span<const uint8_t>>(),
        std::declval<std::span<HttpHeader>>()));

    static_assert(std::is_same_v<
        ReqRet,
        std::expected<std::optional<ParseResult<HttpRequest>>, ErrorInfo>>);
    static_assert(std::is_same_v<
        RspRet,
        std::expected<std::optional<ParseResult<HttpResponse>>, ErrorInfo>>);
    SUCCEED();
}

TEST(HttpStaticAsserts, HotPathFunctionsAreNoexcept) {
    // Confirm the hot-path API is noexcept end-to-end.
    static_assert(noexcept(parse_http_request(
        std::declval<std::span<const uint8_t>>(),
        std::declval<std::span<HttpHeader>>())));
    static_assert(noexcept(parse_http_response(
        std::declval<std::span<const uint8_t>>(),
        std::declval<std::span<HttpHeader>>())));
    static_assert(noexcept(build_http_request(
        nullptr, 0, std::string_view{}, std::string_view{},
        std::span<const HttpHeader>{}, std::span<const uint8_t>{})));
    static_assert(noexcept(build_http_response(
        nullptr, 0, 0, std::string_view{},
        std::span<const HttpHeader>{}, std::span<const uint8_t>{})));
    SUCCEED();
}
