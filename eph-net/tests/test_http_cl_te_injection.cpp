/// @file test_http_cl_te_injection.cpp
/// @brief P0 security — Content-Length + Transfer-Encoding smuggling defense.
///
/// Migrated from v3.3 baseline `test_http_request_cl_te_injection.cpp`.
///
/// These cases verify that the HTTP parser **rejects** any request or
/// response that contains a `Transfer-Encoding` header (per plan §D-1,
/// TE is not supported at all in v3.3) AND any message that mixes
/// `Content-Length` with `Transfer-Encoding` (CL/TE desync — CWE-444).
///
/// The baseline builder auto-injected `Content-Length` from the body
/// argument and scanned `extra_headers` for user-supplied CL/TE. v3.3
/// deliberately does not auto-inject CL — the caller owns the header
/// list — so the defense path is:
///   1. Any TE header in the parsed byte stream → CodecBad.
///   2. Any CL+TE combination → CodecBad (same reason as #1).
///   3. Conflicting duplicate CL values → CodecBad.
///
/// The baseline attack byte sequences are preserved verbatim; the
/// assertions have been rewritten to target the parser's "parse these
/// raw bytes and confirm rejection" path rather than the legacy
/// builder's "scan extra_headers" path.

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
using eph::net::parse_http_request;

namespace {

std::span<const uint8_t> as_bytes(std::string_view s) noexcept {
    return std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

using HdrStorage = std::array<HttpHeader, 16>;

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// Content-Length supplied in the wire — accepted
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestClTeInjection, PlainContentLengthAccepted) {
    // Sanity: a request with a valid CL and matching body is accepted.
    // Baseline NoExtraHeadersAccepted / EmptyBodyNoContentLengthGenerated
    // analogue on the parser side.
    constexpr std::string_view wire =
        "POST /api HTTP/1.1\r\n"
        "Host: host.com\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 9\r\n\r\n"
        "body data";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value());
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 9u);
}

// ═══════════════════════════════════════════════════════════════════════
// Transfer-Encoding alone — rejected outright (plan §D-1)
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestClTeInjection, TransferEncodingInHeadersRejected) {
    constexpr std::string_view wire =
        "POST /api HTTP/1.1\r\n"
        "Host: host.com\r\n"
        "Content-Type: application/json\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "body";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestClTeInjection, TransferEncodingCaseInsensitiveRejected) {
    // Header-name match must be case-insensitive — lowercase variant.
    constexpr std::string_view wire =
        "POST /api HTTP/1.1\r\n"
        "Host: host.com\r\n"
        "transfer-encoding: chunked\r\n\r\n"
        "body";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Content-Length + Transfer-Encoding combined — rejected (CWE-444)
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestClTeInjection, ContentLengthPlusTransferEncodingRejected) {
    // Classic smuggling payload: both framing headers present.
    // Baseline ContentLengthInExtraHeadersRejected port.
    constexpr std::string_view wire =
        "POST /api HTTP/1.1\r\n"
        "Host: host.com\r\n"
        "Content-Length: 999\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "body";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestClTeInjection, ContentLengthAfterOtherHeaderWithTeRejected) {
    // CL appearing AFTER an unrelated header, still combined with TE.
    // Baseline ContentLengthAfterOtherHeaderRejected port.
    constexpr std::string_view wire =
        "POST /api HTTP/1.1\r\n"
        "Host: host.com\r\n"
        "X-Custom: yes\r\n"
        "Content-Length: 999\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "body";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestClTeInjection, TransferEncodingCaseMixedWithClRejected) {
    // Baseline ContentLengthCaseInsensitiveRejected + TransferEncoding
    // — TE-case and CL-case both exercised in one payload.
    constexpr std::string_view wire =
        "POST /api HTTP/1.1\r\n"
        "Host: host.com\r\n"
        "content-length: 999\r\n"
        "TRANSFER-ENCODING: chunked\r\n\r\n"
        "body";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

TEST(HttpRequestClTeInjection, ContentLengthMixedCaseWithTeRejected) {
    // Baseline ContentLengthMixedCaseRejected port.
    constexpr std::string_view wire =
        "POST /api HTTP/1.1\r\n"
        "Host: host.com\r\n"
        "CoNtEnT-LeNgTh: 999\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "body";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::CodecBad);
}

// ═══════════════════════════════════════════════════════════════════════
// Header names that contain "content-length" as a substring — OK
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpRequestClTeInjection, SimilarNamedHeaderStillAccepted) {
    // Baseline OtherHeadersStillAccepted port: "X-Content-Length-Override"
    // is a distinct header and must NOT trip the CL/TE defense.
    constexpr std::string_view wire =
        "POST /api HTTP/1.1\r\n"
        "Host: host.com\r\n"
        "Content-Type: application/json\r\n"
        "X-Content-Length-Override: 999\r\n"
        "Content-Length: 4\r\n\r\n"
        "body";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_TRUE(r->has_value());
    EXPECT_EQ((*r)->value.body.size(), 4u);
}

TEST(HttpRequestClTeInjection, SimilarNamedHeaderContainingTransferEncoding) {
    // Same principle: "X-Transfer-Encoding-Debug" is not Transfer-Encoding.
    constexpr std::string_view wire =
        "POST /api HTTP/1.1\r\n"
        "Host: host.com\r\n"
        "X-Transfer-Encoding-Debug: yes\r\n"
        "Content-Length: 4\r\n\r\n"
        "body";
    HdrStorage hdrs{};
    auto r = parse_http_request(as_bytes(wire), hdrs);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_TRUE(r->has_value());
}
