/// @file test_http_request_cl_te_injection.cpp
/// Test that build_http_request rejects user-supplied Content-Length
/// and Transfer-Encoding in extra_headers.
///
/// build_http_request manages Content-Length itself (auto-derived from
/// body.size()).  Allowing the caller to also pass Content-Length via
/// extra_headers creates a CL/CL desync attack vector on the REQUEST
/// side, mirroring the smuggling defenses on the response side.
///
/// Same defense applies to Transfer-Encoding: build_http_request does
/// not support chunked request bodies, so any TE header in
/// extra_headers would mismatch the actual on-wire body framing.

#include <string>

#include <gtest/gtest.h>

#include "eph/net/http_message.hpp"

using namespace eph::net;

TEST(HttpRequestClTeInjection, ContentLengthInExtraHeadersRejected) {
    auto r = build_http_request("POST", "host.com", "/api", "body data",
                                 "application/json",
                                 "Content-Length: 999\r\n");
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("Content-Length"), std::string::npos);
}

TEST(HttpRequestClTeInjection, ContentLengthCaseInsensitiveRejected) {
    auto r = build_http_request("POST", "host.com", "/api", "body",
                                 "application/json",
                                 "content-length: 999\r\n");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestClTeInjection, ContentLengthMixedCaseRejected) {
    auto r = build_http_request("POST", "host.com", "/api", "body",
                                 "application/json",
                                 "CoNtEnT-LeNgTh: 999\r\n");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestClTeInjection, TransferEncodingInExtraHeadersRejected) {
    auto r = build_http_request("POST", "host.com", "/api", "body",
                                 "application/json",
                                 "Transfer-Encoding: chunked\r\n");
    EXPECT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("Transfer-Encoding"), std::string::npos);
}

TEST(HttpRequestClTeInjection, TransferEncodingCaseInsensitiveRejected) {
    auto r = build_http_request("POST", "host.com", "/api", "body",
                                 "application/json",
                                 "transfer-encoding: chunked\r\n");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestClTeInjection, ContentLengthAfterOtherHeaderRejected) {
    // Content-Length on the SECOND line (after a legitimate header).
    auto r = build_http_request("POST", "host.com", "/api", "body",
                                 "application/json",
                                 "X-Custom: yes\r\nContent-Length: 999\r\n");
    EXPECT_FALSE(r.has_value());
}

TEST(HttpRequestClTeInjection, OtherHeadersStillAccepted) {
    // Verify the validation doesn't false-positive on header names that
    // contain "content-length" or "transfer-encoding" as substrings.
    auto r = build_http_request("POST", "host.com", "/api", "body",
                                 "application/json",
                                 "X-Content-Length-Override: 999\r\n");
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
}

TEST(HttpRequestClTeInjection, NoExtraHeadersAccepted) {
    auto r = build_http_request("POST", "host.com", "/api", "body",
                                 "application/json");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_NE(r->find("Content-Length: 4\r\n"), std::string::npos);
}

TEST(HttpRequestClTeInjection, EmptyBodyNoContentLengthGenerated) {
    // Sanity: no body → no Content-Length.  User can't sneak one in via
    // extra_headers either.
    auto r = build_http_request("GET", "host.com", "/api", {}, {},
                                 "Content-Length: 0\r\n");
    EXPECT_FALSE(r.has_value());
}
