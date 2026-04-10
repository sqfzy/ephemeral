/// @file test_http.cpp
/// @brief Smoke tests for the HFT-pragmatic HTTP/1.x parser subset.
///
/// This is sub-phase 9.3's minimal test set (~30 cases). The full P0 security
/// migration (~214 cases) lives in sub-phase 9.4 and replaces this file.
/// For now we want "enough tests to prove the incremental parser works and
/// the D-1 rejection path fires" — no more, no less.

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

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Positive parse cases — requests
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
    // The stored header's name should be the trimmed form.
    ASSERT_EQ((*r)->value.headers.size(), 1u);
    EXPECT_EQ((*r)->value.headers[0].name, "Content-Length");
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

TEST(HttpParseRequest, EmptyHeaderStorageWithHeaders) {
    // No room for any headers → overflow.
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    std::array<HttpHeader, 0> hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
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
