/// @file test_http_te_edge.cpp
/// @brief P0 security — Transfer-Encoding edge-case rejection (plan §D-1).
///
/// Migrated from baseline `test_http_te_edge_cases.cpp`, **inverted**.
///
/// The baseline parser tolerated chunked and a handful of comma-list TE
/// patterns (`gzip, chunked`, `deflate, chunked`, mixed-case, leading SP,
/// multiple TE header lines, etc.). The parser rejects **any**
/// Transfer-Encoding header outright per plan §D-1 — the rationale is
/// that HFT REST APIs all use Content-Length and every proxy-compat
/// path through TE is a smuggling surface we refuse to carry.
///
/// So every baseline case is migrated here with the assertion flipped:
/// anything that previously *accepted* chunked now *rejects* it with
/// `Error::CodecBad`. The original byte sequences are preserved
/// verbatim to pin the rejection path on exactly those attack patterns.

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
using eph::net::parse_http_response;

namespace {

std::span<const uint8_t> as_bytes(std::string_view s) noexcept {
    return std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

using HdrStorage = std::array<HttpHeader, 16>;

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Comma-separated transfer codings — all rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpTeEdge, GzipChunkedRejected) {
    // Baseline accepted "gzip, chunked" (chunked last, valid per RFC).
    // The parser rejects any TE header.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpTeEdge, GzipOnlyRejected) {
    // Baseline: "gzip" only → returned incomplete (read until close).
    // TE header present → reject.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: gzip\r\n\r\n"
        "some gzipped bytes here";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpTeEdge, DeflateChunkedRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: deflate, chunked\r\n\r\n"
        "0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpTeEdge, ChunkedGzipRejected) {
    // Baseline: "chunked, gzip" — RFC violation (chunked must be LAST).
    // Either way, the parser rejects because TE header is present.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked, gzip\r\n\r\n"
        "garbage that is not hex chunk size";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Multiple Transfer-Encoding header lines — all rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpTeEdge, MultipleTeHeadersFirstChunkedRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Transfer-Encoding: gzip\r\n\r\n"
        "0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpTeEdge, MultipleTeHeadersFirstGzipRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: gzip\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Case variations in coding name — all rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpTeEdge, ChunkedUppercaseRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: CHUNKED\r\n\r\n"
        "0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpTeEdge, ChunkedMixedCaseRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: ChUnKeD\r\n\r\n"
        "0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpTeEdge, ChunkedLowercaseRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Whitespace tolerance in TE value — still rejected because the header
// name is still Transfer-Encoding, whatever the value looks like
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpTeEdge, TeValueWithLeadingSpacesRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding:    chunked\r\n\r\n"
        "0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpTeEdge, TeValueWithTrailingSpaceRejected) {
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked   \r\n\r\n"
        "0\r\n\r\n";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Empty / corrupt / identity values — all rejected
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpTeEdge, EmptyTeValueRejected) {
    // Header name is still Transfer-Encoding.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: \r\n\r\n"
        "some body";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpTeEdge, TeIdentityRejected) {
    // "identity" was a no-op coding. The parser rejects anyway — TE header present.
    constexpr std::string_view wire =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: identity\r\n\r\n"
        "body";
    HdrStorage hdrs{};
    auto r = parse_http_response(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}
