/// @file test_http_client.cpp
/// @brief Parser-level tests migrated from the baseline test_http_client.cpp.
///
/// The baseline `HttpClient` class is not part of the current API. However, a large
/// share of its baseline test suite
/// actually exercises the underlying parser against crafted byte sequences
/// rather than any connection state. Those cases carry real regression
/// value for the new `parse_http_request` / `parse_http_response`
/// subset, so they are migrated here with adapted assertions against
/// `expected<optional<ParseResult<T>>>`.
///
/// Skipped per plan §D-1: chunked / Transfer-Encoding / cookies / redirect /
/// Expect: 100-continue / multipart. Skipped out-of-scope: `HttpClient`
/// instance behavior (connect / reconnect / keep-alive), `HttpClient::Config`
/// URL parsing (9.6 territory).

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>

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

std::span<const uint8_t> as_bytes(std::string_view s) noexcept {
    return std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

using HdrStorage = std::array<HttpHeader, 16>;

std::string_view body_sv(const HttpResponse& r) noexcept {
    return std::string_view(
        reinterpret_cast<const char*>(r.body.data()), r.body.size());
}

std::string_view body_sv(const HttpRequest& r) noexcept {
    return std::string_view(
        reinterpret_cast<const char*>(r.body.data()), r.body.size());
}

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

// =============================================================================
// build_http_request — GET (migrated from HttpClientRequest baseline)
// =============================================================================

TEST(HttpClientBuildRequest, GetBasic) {
    std::array<uint8_t, 512> out{};
    HttpHeader hdrs[] = {{"Host", "api.binance.com"}, {"Connection", "close"}};
    auto r = build_http_request(out.data(), out.size(), "GET",
                                "/api/v3/ticker/price",
                                std::span<const HttpHeader>(hdrs, 2));
    ASSERT_TRUE(r.has_value());
    std::string_view s(reinterpret_cast<const char*>(out.data()), *r);
    EXPECT_NE(s.find("GET /api/v3/ticker/price HTTP/1.1\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("Host: api.binance.com\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("Connection: close\r\n"), std::string_view::npos);
    EXPECT_TRUE(s.ends_with("\r\n\r\n"));
}

TEST(HttpClientBuildRequest, GetWithExtraHeaders) {
    std::array<uint8_t, 512> out{};
    HttpHeader hdrs[] = {
        {"Host", "api.exchange.com"},
        {"Authorization", "Bearer tok123"},
        {"X-MBX-APIKEY", "key456"},
    };
    auto r = build_http_request(out.data(), out.size(), "GET", "/v1/balance",
                                std::span<const HttpHeader>(hdrs, 3));
    ASSERT_TRUE(r.has_value());
    std::string_view s(reinterpret_cast<const char*>(out.data()), *r);
    EXPECT_NE(s.find("Authorization: Bearer tok123\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("X-MBX-APIKEY: key456\r\n"), std::string_view::npos);
}

TEST(HttpClientBuildRequest, GetWithQueryString) {
    std::array<uint8_t, 512> out{};
    HttpHeader hdrs[] = {{"Host", "api.binance.com"}};
    auto r = build_http_request(out.data(), out.size(), "GET",
                                "/api/v3/depth?symbol=BTCUSDT&limit=5",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value());
    std::string_view s(reinterpret_cast<const char*>(out.data()), *r);
    EXPECT_NE(s.find("GET /api/v3/depth?symbol=BTCUSDT&limit=5 HTTP/1.1"),
              std::string_view::npos);
}

TEST(HttpClientBuildRequest, PostWithJsonBody) {
    std::array<uint8_t, 512> out{};
    std::string body = R"({"symbol":"BTCUSDT","side":"BUY","quantity":"0.01"})";
    std::string cl = std::to_string(body.size());
    HttpHeader hdrs[] = {
        {"Host", "api.exchange.com"},
        {"Content-Type", "application/json"},
        {"Content-Length", cl},
    };
    auto r = build_http_request(out.data(), out.size(), "POST", "/api/v1/order",
                                std::span<const HttpHeader>(hdrs, 3),
                                as_bytes(body));
    ASSERT_TRUE(r.has_value());
    std::string_view s(reinterpret_cast<const char*>(out.data()), *r);
    EXPECT_NE(s.find("POST /api/v1/order HTTP/1.1\r\n"), std::string_view::npos);
    EXPECT_NE(s.find("Content-Type: application/json\r\n"), std::string_view::npos);
    EXPECT_TRUE(s.ends_with(body));
}

TEST(HttpClientBuildRequest, PostWithFormBody) {
    std::array<uint8_t, 512> out{};
    std::string body = "symbol=BTCUSDT&side=BUY&quantity=0.01";
    std::string cl = std::to_string(body.size());
    HttpHeader hdrs[] = {
        {"Host", "api.exchange.com"},
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"Content-Length", cl},
    };
    auto r = build_http_request(out.data(), out.size(), "POST", "/order",
                                std::span<const HttpHeader>(hdrs, 3),
                                as_bytes(body));
    ASSERT_TRUE(r.has_value());
    std::string_view s(reinterpret_cast<const char*>(out.data()), *r);
    EXPECT_NE(s.find("Content-Type: application/x-www-form-urlencoded\r\n"),
              std::string_view::npos);
}

TEST(HttpClientBuildRequest, EmptyMethodReturnsError) {
    std::array<uint8_t, 256> out{};
    auto r = build_http_request(out.data(), out.size(), "", "/path", {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientBuildRequest, EmptyTargetReturnsError) {
    std::array<uint8_t, 256> out{};
    auto r = build_http_request(out.data(), out.size(), "GET", "", {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientBuildRequest, LargeBodyRoundTrip) {
    // 10 000-byte body should serialize and re-parse cleanly.
    std::vector<uint8_t> out(64 * 1024);
    std::string body(10000, 'x');
    std::string cl = std::to_string(body.size());
    HttpHeader hdrs[] = {
        {"Host", "host.com"},
        {"Content-Type", "application/octet-stream"},
        {"Content-Length", cl},
    };
    auto built = build_http_request(out.data(), out.size(), "POST", "/upload",
                                    std::span<const HttpHeader>(hdrs, 3),
                                    as_bytes(body));
    ASSERT_TRUE(built.has_value());

    HdrStorage parsed_hdrs{};
    auto parsed = parse_http_request(
        std::span<const uint8_t>(out.data(), *built), parsed_hdrs);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->has_value());
    EXPECT_EQ((*parsed)->value.body.size(), 10000u);
}

TEST(HttpClientBuildRequest, DeleteMethod) {
    std::array<uint8_t, 256> out{};
    HttpHeader hdrs[] = {{"Host", "api.exchange.com"}};
    auto r = build_http_request(out.data(), out.size(),
                                "DELETE", "/api/v1/order/123",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value());
    std::string_view s(reinterpret_cast<const char*>(out.data()), *r);
    EXPECT_NE(s.find("DELETE /api/v1/order/123 HTTP/1.1\r\n"),
              std::string_view::npos);
}

TEST(HttpClientBuildRequest, PutMethodWithBody) {
    std::array<uint8_t, 512> out{};
    std::string body = R"({"updated":true})";
    std::string cl = std::to_string(body.size());
    HttpHeader hdrs[] = {
        {"Host", "host.com"},
        {"Content-Type", "application/json"},
        {"Content-Length", cl},
    };
    auto r = build_http_request(out.data(), out.size(), "PUT", "/resource",
                                std::span<const HttpHeader>(hdrs, 3),
                                as_bytes(body));
    ASSERT_TRUE(r.has_value());
    std::string_view s(reinterpret_cast<const char*>(out.data()), *r);
    EXPECT_NE(s.find("PUT /resource HTTP/1.1\r\n"), std::string_view::npos);
}

TEST(HttpClientBuildRequest, PatchMethod) {
    std::array<uint8_t, 256> out{};
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    auto r = build_http_request(out.data(), out.size(), "PATCH", "/resource",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value());
}

TEST(HttpClientBuildRequest, HeadMethod) {
    std::array<uint8_t, 256> out{};
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    auto r = build_http_request(out.data(), out.size(), "HEAD", "/",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value());
}

TEST(HttpClientBuildRequest, OptionsMethod) {
    std::array<uint8_t, 256> out{};
    HttpHeader hdrs[] = {{"Host", "host.com"}};
    auto r = build_http_request(out.data(), out.size(), "OPTIONS", "*",
                                std::span<const HttpHeader>(hdrs, 1));
    ASSERT_TRUE(r.has_value());
}

// =============================================================================
// parse_http_response — success paths (migrated from HttpClientResponse)
// =============================================================================

TEST(HttpClientParseResponse, Parse200Ok) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 28\r\n"
        "\r\n"
        R"({"price":"50000.00","qty":1})";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 200u);
    EXPECT_EQ(body_sv((*r)->value), R"({"price":"50000.00","qty":1})");
    EXPECT_EQ(find_hdr((*r)->value.headers, "Content-Type"), "application/json");
}

TEST(HttpClientParseResponse, Parse404NotFound) {
    constexpr std::string_view wire =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 404u);
    EXPECT_EQ(body_sv((*r)->value), "Not Found");
}

TEST(HttpClientParseResponse, Parse500InternalServerError) {
    constexpr std::string_view wire =
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 500u);
    EXPECT_TRUE((*r)->value.body.empty());
}

TEST(HttpClientParseResponse, Parse204EmptyBody) {
    constexpr std::string_view wire = "HTTP/1.1 204 No Content\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 204u);
    EXPECT_TRUE((*r)->value.body.empty());
}

TEST(HttpClientParseResponse, ParseBodyLargerThanContentLengthConsumesCL) {
    // Parser consumes exactly CL bytes, returning "extra" via shorter consumed.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloextra";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(body_sv((*r)->value), "hello");
    EXPECT_EQ((*r)->consumed, wire.size() - 5); // "extra" stays in buffer
}

TEST(HttpClientParseResponse, ParseMultipleHeaders) {
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
    EXPECT_EQ((*r)->value.status_code, 200u);
    EXPECT_EQ(body_sv((*r)->value), "{}");
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-RateLimit-Remaining"), "1199");
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-RateLimit-Reset"), "1640000000");
}

TEST(HttpClientParseResponse, ParseIncompleteNoHeaderEnd) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value()); // need more
}

TEST(HttpClientParseResponse, ParseMalformedStatusLine) {
    constexpr std::string_view wire = "GARBAGE\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, ParseInvalidStatusCode) {
    constexpr std::string_view wire = "HTTP/1.1 XYZ Bad\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, ParseEmptyInput) {
    HdrStorage hdrs{};
    auto r = parse_http_response(std::span<const uint8_t>{}, hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

// RFC 7230 §3.1.2 status-code rejection regression cases.
TEST(HttpClientParseResponse, ParseStatusCodeTrailingGarbageRejected) {
    constexpr std::string_view wire = "HTTP/1.1 200xyz OK\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, ParseStatusCodeLeadingPlusRejected) {
    constexpr std::string_view wire = "HTTP/1.1 +200 OK\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, ParseStatusCodeNegativeRejected) {
    constexpr std::string_view wire = "HTTP/1.1 -1 Bad\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, ParseStatusCodeTwoDigitsRejected) {
    constexpr std::string_view wire = "HTTP/1.1 20 OK\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, ParseStatusCodeFourDigitsRejected) {
    constexpr std::string_view wire = "HTTP/1.1 2000 OK\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, AllValidThreeDigitCodesAccepted) {
    for (int code : {100, 101, 200, 201, 204, 301, 302, 400, 401,
                     403, 404, 500, 502, 503, 599}) {
        std::string wire = "HTTP/1.1 " + std::to_string(code) + " OK\r\n";
        if (code >= 200 && code != 204 && code != 304) {
            wire += "Content-Length: 0\r\n";
        }
        wire += "\r\n";
        HdrStorage hdrs{};
        auto r = parse_http_response(as_bytes(wire), hdrs);
        ASSERT_TRUE(r.has_value()) << "code=" << code;
        ASSERT_TRUE(r->has_value()) << "code=" << code;
        EXPECT_EQ((*r)->value.status_code, code);
    }
}

TEST(HttpClientParseResponse, ParseStatusCodeOnlyNoReason) {
    // Empty reason phrase permitted per RFC.
    constexpr std::string_view wire =
        "HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 200u);
    EXPECT_EQ((*r)->value.reason_phrase, "");
}

TEST(HttpClientParseResponse, ParseBinaryBody) {
    std::string wire = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n";
    wire.append("\x00\x01\x02\x03\x04\x05\x06\x07", 8);
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 8u);
    EXPECT_EQ((*r)->value.body[0], 0x00);
    EXPECT_EQ((*r)->value.body[7], 0x07);
}

TEST(HttpClientParseResponse, ParseLongReasonPhrase) {
    constexpr std::string_view wire =
        "HTTP/1.1 503 Service Temporarily Unavailable Due To Maintenance\r\n"
        "Content-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.status_code, 503u);
    EXPECT_EQ((*r)->value.reason_phrase,
              "Service Temporarily Unavailable Due To Maintenance");
}

TEST(HttpClientParseResponse, ParseReasonPhraseWithSpaces) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 O K\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.reason_phrase, "O K");
}

TEST(HttpClientParseResponse, RejectReasonPhraseWithBareCr) {
    std::string wire = "HTTP/1.1 200 B\rOK\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    // Bare CR in the reason phrase splits the start line early — parser
    // will see either a short line or a codec error; either way no value.
    ASSERT_FALSE(r.has_value() && r->has_value());
}

// =============================================================================
// header lookup — migrated from HttpClientFindHeader
// =============================================================================
//
// The parser returns a span of HttpHeader; the baseline find_header()
// helper is not part of the public API but the same semantics are tested
// here against a local helper that mimics it.

TEST(HttpClientFindHeader, BasicLookup) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 3\r\n"
        "X-Custom: value123\r\n\r\n"
        "abc";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "Content-Type"), "application/json");
    EXPECT_EQ(find_hdr((*r)->value.headers, "Content-Length"), "3");
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-Custom"), "value123");
}

TEST(HttpClientFindHeader, CaseInsensitive) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\ncontent-type: text/html\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "Content-Type"), "text/html");
    EXPECT_EQ(find_hdr((*r)->value.headers, "CONTENT-TYPE"), "text/html");
}

TEST(HttpClientFindHeader, TrimsWhitespace) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Type:   application/json  \r\n"
        "Content-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "Content-Type"), "application/json");
}

TEST(HttpClientFindHeader, NotFoundReturnsEmpty) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-Missing"), "");
}

TEST(HttpClientFindHeader, HeaderWithColonInValue) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Location: https://example.com:8080/path\r\n"
        "Content-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "Location"),
              "https://example.com:8080/path");
}

TEST(HttpClientFindHeader, EmptyValue) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nX-Empty:\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-Empty"), "");
}

TEST(HttpClientFindHeader, WhitespaceOnlyValueTrimmedToEmpty) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nX-Blank:   \t  \r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-Blank"), "");
}

TEST(HttpClientFindHeader, ValueWithLeadingAndTrailingOws) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nX-Val:  \t hello  \r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "X-Val"), "hello");
}

TEST(HttpClientFindHeader, PartialNameDoesNotFalsePositive) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Content-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(find_hdr((*r)->value.headers, "Content-Typ"), "");
}

// =============================================================================
// is_response_complete equivalent — pure parser completion
// =============================================================================
//
// The current API replaces the baseline `is_response_complete(buf)` predicate with the
// parser's incremental "need more / complete / error" tri-state. Each
// baseline case maps onto a parse call and checks which of those three
// outcomes fires.

TEST(HttpClientComplete, CompleteWithContentLength) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->consumed, wire.size());
}

TEST(HttpClientComplete, IncompleteBody) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(HttpClientComplete, NoContentLengthRejected) {
    // Without CL and not bodyless, parser rejects — we cannot frame.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nsome data";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientComplete, IncompleteHeaders) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(HttpClientComplete, ZeroContentLength) {
    constexpr std::string_view wire =
        "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 0u);
}

TEST(HttpClientComplete, LowercaseContentLength) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\ncontent-length: 3\r\n\r\nabc";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(body_sv((*r)->value), "abc");
}

TEST(HttpClientComplete, InvalidContentLengthRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\ndata";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientComplete, ExcessBodyBeyondContentLength) {
    // Parser takes exactly CL bytes from the body; the rest stays in buf.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcdef";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(body_sv((*r)->value), "abc");
    EXPECT_EQ((*r)->consumed, wire.size() - 3);
}

TEST(HttpClientComplete, EmptyStringReturnsNone) {
    HdrStorage hdrs{};
    auto r = parse_http_response(std::span<const uint8_t>{}, hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

TEST(HttpClientComplete, IncompleteWithoutHeaderTerminator) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
}

// =============================================================================
// Round-trip exchange scenarios (migrated from HttpClientRoundTrip)
// =============================================================================

TEST(HttpClientRoundTrip, BuildAndParseSimulatedBinance) {
    std::array<uint8_t, 1024> out{};
    HttpHeader req_hdrs[] = {{"Host", "api.binance.com"}};
    auto req = build_http_request(out.data(), out.size(), "GET",
                                  "/api/v3/ticker/price?symbol=BTCUSDT",
                                  std::span<const HttpHeader>(req_hdrs, 1));
    ASSERT_TRUE(req.has_value());

    // Simulated server response.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 39\r\n"
        "X-MBX-USED-WEIGHT-1M: 1\r\n\r\n"
        R"({"symbol":"BTCUSDT","price":"50000.00"})";
    HdrStorage hdrs{};
    auto resp = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(resp.has_value());
    ASSERT_TRUE(resp->has_value());
    EXPECT_EQ((*resp)->value.status_code, 200u);
    EXPECT_EQ(body_sv((*resp)->value), R"({"symbol":"BTCUSDT","price":"50000.00")" "}");
    EXPECT_EQ(find_hdr((*resp)->value.headers, "Content-Type"), "application/json");
    EXPECT_EQ(find_hdr((*resp)->value.headers, "X-MBX-USED-WEIGHT-1M"), "1");
}

TEST(HttpClientRoundTrip, PostOrderAndParseResponse) {
    std::array<uint8_t, 1024> out{};
    std::string body =
        R"({"symbol":"ETHUSDT","side":"BUY","type":"LIMIT","price":"3000","quantity":"1"})";
    std::string cl = std::to_string(body.size());
    HttpHeader req_hdrs[] = {
        {"Host", "api.exchange.com"},
        {"Content-Type", "application/json"},
        {"Content-Length", cl},
        {"X-API-KEY", "mykey123"},
    };
    auto req = build_http_request(out.data(), out.size(), "POST", "/api/v1/order",
                                  std::span<const HttpHeader>(req_hdrs, 4),
                                  as_bytes(body));
    ASSERT_TRUE(req.has_value());

    HdrStorage parse_hdrs{};
    auto parsed_req = parse_http_request(
        std::span<const uint8_t>(out.data(), *req), parse_hdrs);
    ASSERT_TRUE(parsed_req.has_value());
    ASSERT_TRUE(parsed_req->has_value());
    EXPECT_EQ((*parsed_req)->value.method, "POST");
    EXPECT_EQ((*parsed_req)->value.target, "/api/v1/order");
    EXPECT_EQ(body_sv((*parsed_req)->value), body);
    EXPECT_EQ(find_hdr((*parsed_req)->value.headers, "X-API-KEY"), "mykey123");

    // Simulated response.
    constexpr std::string_view resp_wire =
        "HTTP/1.1 201 Created\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 26\r\n\r\n"
        R"({"orderId":"12345","ok":1})";
    HdrStorage rsp_hdrs{};
    auto resp = parse_http_response(as_bytes(resp_wire), rsp_hdrs);
    ASSERT_TRUE(resp.has_value());
    ASSERT_TRUE(resp->has_value());
    EXPECT_EQ((*resp)->value.status_code, 201u);
    EXPECT_EQ(body_sv((*resp)->value), R"({"orderId":"12345","ok":1})");
}

// =============================================================================
// Status category predicates (constexpr bit-twiddles, not parser — but
// mandated by the baseline tests; preserved as plain field checks)
// =============================================================================

TEST(HttpStatusCategory, Is1xxInformational) {
    for (int c : {100, 101, 199}) {
        HttpResponse r{}; r.status_code = static_cast<uint16_t>(c);
        EXPECT_TRUE(r.status_code >= 100 && r.status_code < 200);
    }
}

TEST(HttpStatusCategory, Is2xxSuccess) {
    for (int c : {200, 201, 299}) {
        HttpResponse r{}; r.status_code = static_cast<uint16_t>(c);
        EXPECT_TRUE(r.status_code >= 200 && r.status_code < 300);
    }
}

TEST(HttpStatusCategory, Is3xxRedirect) {
    for (int c : {301, 302, 307, 399}) {
        HttpResponse r{}; r.status_code = static_cast<uint16_t>(c);
        EXPECT_TRUE(r.status_code >= 300 && r.status_code < 400);
    }
}

TEST(HttpStatusCategory, Is4xxClientError) {
    for (int c : {400, 404, 499}) {
        HttpResponse r{}; r.status_code = static_cast<uint16_t>(c);
        EXPECT_TRUE(r.status_code >= 400 && r.status_code < 500);
    }
}

TEST(HttpStatusCategory, Is5xxServerError) {
    for (int c : {500, 503, 599}) {
        HttpResponse r{}; r.status_code = static_cast<uint16_t>(c);
        EXPECT_TRUE(r.status_code >= 500 && r.status_code < 600);
    }
}

// =============================================================================
// Additional Content-Length edge cases (baseline IsCompleteAdv)
// =============================================================================

TEST(HttpClientParseResponse, ContentLengthZeroEmptyBodyComplete) {
    constexpr std::string_view wire =
        "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
}

TEST(HttpClientParseResponse, ContentLengthZeroExtraBodyConsumesNoExtra) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\nabcd";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 0u);
    // Four "abcd" bytes remain in buffer beyond the header terminator.
    EXPECT_EQ((*r)->consumed, wire.size() - 4);
}

TEST(HttpClientParseResponse, ContentLengthLeadingPlusRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, ContentLengthHugeRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 999999999999\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

TEST(HttpClientParseResponse, ContentLengthCaseInsensitive) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(body_sv((*r)->value), "hello");
}

TEST(HttpClientParseResponse, ContentLengthMixedCase) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nCoNtEnT-LeNgTh: 5\r\n\r\nworld";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(body_sv((*r)->value), "world");
}

TEST(HttpClientParseResponse, ContentLengthLeadingSpaceAccepted) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length:    5\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(body_sv((*r)->value), "hello");
}

TEST(HttpClientParseResponse, ContentLengthEmbeddedSpaceRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 5 0\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, ContentLengthEmbeddedTabRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\t0\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, ContentLengthHexValueRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 0x05\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, ContentLengthLeadingZerosAccepted) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 005\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 5u);
}

// =============================================================================
// Multiple Content-Length headers (smuggling defense)
// =============================================================================

TEST(HttpClientParseResponse, DuplicateContentLengthDifferentValuesRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\nContent-Length: 99\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParseResponse, DuplicateContentLengthIdenticalValuesAccepted) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\nContent-Length: 5\r\n\r\nhello";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ(body_sv((*r)->value), "hello");
}

// =============================================================================
// Header-name whitespace regression (RFC 7230 §3.2.4 — commit e108fcb)
// =============================================================================

TEST(HttpClientParseResponse, ContentLengthWithSpaceBeforeColonHonored) {
    // Baseline IsCompleteAdv.ContentLengthWithSpaceBeforeColonStillHonored.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length : 5\r\n\r\n12345";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 5u);
}

TEST(HttpClientParseResponse, ContentLengthWithSpaceBeforeColonShortBody) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length : 10\r\n\r\nabc";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value()); // need more
}

TEST(HttpClientParseResponse, ContentLengthWithTabBeforeColonHonored) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\nContent-Length\t: 4\r\n\r\nabcd";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 4u);
}

// =============================================================================
// Status code mutual-exclusivity / boundary cases
// =============================================================================

TEST(HttpStatusCategory, BoundariesDoNotOverlap) {
    struct Case { int code; int expect_bucket; };
    // 1=info, 2=success, 3=redirect, 4=client, 5=server
    Case cases[] = {
        {100, 1}, {199, 1}, {200, 2}, {299, 2}, {301, 3},
        {399, 3}, {400, 4}, {499, 4}, {500, 5}, {599, 5},
    };
    for (auto& c : cases) {
        int got = c.code / 100;
        EXPECT_EQ(got, c.expect_bucket) << "code=" << c.code;
    }
}

// =============================================================================
// Sanity: the baseline file also covered method / target / version presence
// =============================================================================

TEST(HttpClientParseRequest, VersionMinor0Returned) {
    constexpr std::string_view wire = "GET / HTTP/1.0\r\nHost: h\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.version_minor, 0);
}

TEST(HttpClientParseRequest, VersionMinor1Returned) {
    constexpr std::string_view wire = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.version_minor, 1);
}

TEST(HttpClientParseRequest, MultipleEdgeHeaders) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\n"
        "Host: h\r\n"
        "A: 1\r\n"
        "B: 2\r\n"
        "C: 3\r\n"
        "D: 4\r\n"
        "E: 5\r\n"
        "F: 6\r\n"
        "G: 7\r\n"
        "H: 8\r\n"
        "\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.headers.size(), 9u);
}

TEST(HttpClientParseRequest, ConnectTarget) {
    constexpr std::string_view wire =
        "CONNECT api.binance.com:443 HTTP/1.1\r\n"
        "Host: api.binance.com:443\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.method, "CONNECT");
    EXPECT_EQ((*r)->value.target, "api.binance.com:443");
}

TEST(HttpClientParseRequest, HttpVersion10WithBody) {
    constexpr std::string_view wire =
        "POST /legacy HTTP/1.0\r\nContent-Length: 4\r\n\r\nbody";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.version_minor, 0);
    EXPECT_EQ(body_sv((*r)->value), "body");
}

// =============================================================================
// Parser overflow / storage boundary cases
// =============================================================================

TEST(HttpClientParser, ExceedsHeaderStorageCap) {
    // Build a request with more headers than storage can hold.
    std::string wire = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 20; ++i) {
        wire += "X-H" + std::to_string(i) + ": v\r\n";
    }
    wire += "\r\n";
    std::array<HttpHeader, 4> small_storage{}; // undersized on purpose
    auto r = parse_http_request(as_bytes(wire), small_storage);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecOverflow);
}

TEST(HttpClientParser, ExactlyFillsHeaderStorage) {
    // 16 headers fits in the default HdrStorage.
    std::string wire = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 16; ++i) {
        wire += "X-H" + std::to_string(i) + ": v\r\n";
    }
    wire += "\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.headers.size(), 16u);
}

TEST(HttpClientParser, EmptyHeadersRequestAccepted) {
    // Not strictly valid HTTP (Host is MUST for 1.1) but parser only checks
    // structure, not semantic constraints.
    constexpr std::string_view wire = "GET / HTTP/1.1\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.headers.size(), 0u);
}

// =============================================================================
// Obsolete line-folding (continuation lines) — RFC 7230 §3.2.4 forbids; some
// proxies still tolerate them. We parse the leading SP/HTAB as part of the
// field-name slot, which then fails is_valid_token() → CodecBad.
// =============================================================================

TEST(HttpClientParser, FoldedHeaderLeadingSpaceRejected) {
    // Continuation line: "X-Long-Header: part1\r\n part2\r\n\r\n"
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\n"
        "X-Long-Header: part1\r\n"
        " part2\r\n"
        "\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpClientParser, FoldedHeaderLeadingTabRejected) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\n"
        "X-Long-Header: part1\r\n"
        "\tpart2\r\n"
        "\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}
