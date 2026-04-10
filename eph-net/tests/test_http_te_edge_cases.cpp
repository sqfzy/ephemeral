/// @file test_http_te_edge_cases.cpp
/// RFC 7230 §3.3.1 Transfer-Encoding edge cases.
///
/// RFC 7230 §3.3.1: "If any transfer coding other than chunked is
/// applied to a response payload body, the sender MUST either apply
/// chunked as the final transfer coding or terminate the message by
/// closing the connection.  If any transfer coding other than chunked
/// is applied to a request payload body, the sender MUST apply
/// chunked as the final transfer coding..."
///
/// And §3.3.3: multiple transfer codings are listed comma-separated;
/// chunked must be the LAST coding when present.

#include <string>

#include <gtest/gtest.h>

#include "eph/net/http_message.hpp"

using namespace eph::net;

// ═══════════════════════════════════════════════════════════════════════
// Comma-separated transfer codings
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpTeEdge, GzipChunkedAcceptedAsChunked) {
    // "gzip, chunked" — chunked is last, valid per RFC.
    // The parser's substring matcher finds "chunked" and walks
    // the chunk stream.  The body is presumably gzipped chunks
    // (gzipped content delivered in chunked frames).
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: gzip, chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(HttpTeEdge, GzipOnlyDetectedAsNoChunked) {
    // "gzip" — no chunked.  Parser falls back to connection-close
    // semantics (returns false until close).
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: gzip\r\n"
        "\r\n"
        "some gzipped bytes here";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(HttpTeEdge, ChunkedAtLastPositionTwoCodings) {
    // "deflate, chunked" — also valid per RFC (chunked is last).
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: deflate, chunked\r\n"
        "\r\n"
        "0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(HttpTeEdge, ChunkedNotLastIsRfcInvalidButSubstringMatches) {
    // "chunked, gzip" — RFC 7230 §3.3.1 violation: chunked must be
    // LAST.  The current substring matcher doesn't enforce position
    // and will try to walk chunks anyway.
    //
    // The body bytes will be interpreted as chunk-size, which will
    // either parse as a hex number (false complete/incomplete) or
    // fail parsing (return false).
    //
    // Pin the current behavior: substring match → walks chunks →
    // body must look like chunked.  If the body happens to NOT
    // look like chunked, the walker rejects it.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked, gzip\r\n"
        "\r\n"
        "garbage that is not hex chunk size";
    // The body starts with "garbage" which is not a valid hex chunk
    // size, so the walker rejects it.
    EXPECT_FALSE(is_http_response_complete(resp));
}

// ═══════════════════════════════════════════════════════════════════════
// Multiple Transfer-Encoding header lines
// ═══════════════════════════════════════════════════════════════════════
//
// RFC 7230 §3.2.2: Multiple message-header fields with the same field
// name MAY be combined into one "field-name: field-value" pair, where
// each subsequent field value is appended to the combined field value
// in order, separated by a comma.
//
// The current parser uses find_header which returns the FIRST
// occurrence — so multiple TE headers with chunked in a later line
// will be missed.

TEST(HttpTeEdge, MultipleTeHeadersFirstIsChunked) {
    // First header is chunked → parser detects chunked → walks → complete.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Transfer-Encoding: gzip\r\n"
        "\r\n"
        "0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(HttpTeEdge, MultipleTeHeadersWalkedForChunked) {
    // First header is gzip, second is chunked.  Per RFC 7230 §3.2.2,
    // these are equivalent to "gzip, chunked" — making the body
    // chunked-encoded.  The fixed parser walks ALL Transfer-Encoding
    // header lines and detects chunked.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: gzip\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

// ═══════════════════════════════════════════════════════════════════════
// Case-insensitive coding name matching
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpTeEdge, ChunkedCaseInsensitive) {
    // RFC 7230 §3.3.1: transfer codings are case-insensitive.
    // The fixed parser does case-insensitive substring matching.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: CHUNKED\r\n"
        "\r\n"
        "0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(HttpTeEdge, ChunkedMixedCaseAccepted) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: ChUnKeD\r\n"
        "\r\n"
        "0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(HttpTeEdge, ChunkedLowercaseAccepted) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

// ═══════════════════════════════════════════════════════════════════════
// Whitespace tolerance in Transfer-Encoding value
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpTeEdge, TeValueWithLeadingSpaces) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding:    chunked\r\n"
        "\r\n"
        "0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(HttpTeEdge, TeValueWithTrailingSpaceBeforeCrlf) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked   \r\n"
        "\r\n"
        "0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

// ═══════════════════════════════════════════════════════════════════════
// Empty / corrupt TE value
// ═══════════════════════════════════════════════════════════════════════

TEST(HttpTeEdge, EmptyTeValueFallsThroughToConnectionClose) {
    // Header is present but value is empty.  No "chunked" substring
    // → falls through to connection-close handling.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: \r\n"
        "\r\n"
        "some body";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(HttpTeEdge, TeIdentityCodingFallsThroughToConnectionClose) {
    // "identity" is the no-op coding.  Without chunked, the parser
    // can't determine completion.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: identity\r\n"
        "\r\n"
        "body";
    EXPECT_FALSE(is_http_response_complete(resp));
}
