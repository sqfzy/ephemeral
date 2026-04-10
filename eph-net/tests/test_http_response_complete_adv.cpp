/// @file test_http_response_complete_adv.cpp
/// Adversarial tests for is_http_response_complete().
///
/// is_http_response_complete is the termination predicate for the
/// HTTP client recv loop.  A bug here either hangs the client
/// (under-detection: never reports complete) or returns truncated
/// responses (over-detection: reports complete too early).
///
/// The pre-existing test_http_client.cpp covers the happy path:
/// Content-Length cases, basic chunked-encoding cases, malformed
/// Content-Length.  This file targets adversarial inputs that could
/// trip false positives or false negatives:
///
/// * Chunk data that incidentally contains the "0\r\n\r\n" terminator
///   byte sequence (THE classic chunked-parsing pitfall)
/// * Multiple Content-Length headers (RFC 7230 §3.3.3 ambiguity)
/// * Content-Length and Transfer-Encoding both present
/// * Whitespace and case variations in header values
/// * Numeric edge cases: 0, leading-zero, very large

#include <string>

#include <gtest/gtest.h>

#include "eph/net/http_message.hpp"

using namespace eph::net;

// ═══════════════════════════════════════════════════════════════════════
// Content-Length edge cases
// ═══════════════════════════════════════════════════════════════════════

TEST(IsCompleteAdv, ContentLengthZeroEmptyBodyComplete) {
    std::string resp =
        "HTTP/1.1 204 No Content\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthZeroExtraBodyStillComplete) {
    // body_len (4) >= content_length (0), so complete.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "\r\n"
        "abcd";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthLeadingPlusRejected) {
    // std::from_chars treats "+5" as invalid for unsigned types.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: +5\r\n"
        "\r\n"
        "hello";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthHugeRejected) {
    // 256 MiB cap rejects values larger than this.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 999999999999\r\n"
        "\r\n";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthCaseInsensitive) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "content-length: 5\r\n"
        "\r\n"
        "hello";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthMixedCase) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "CoNtEnT-LeNgTh: 5\r\n"
        "\r\n"
        "world";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthLeadingSpaceAccepted) {
    // The header value is trimmed before parsing.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length:    5\r\n"
        "\r\n"
        "hello";
    EXPECT_TRUE(is_http_response_complete(resp));
}

// ═══════════════════════════════════════════════════════════════════════
// Chunked transfer edge cases
// ═══════════════════════════════════════════════════════════════════════

TEST(IsCompleteAdv, ChunkedSinglePayloadComplete) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "C\r\nhello, world\r\n0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ChunkedMultiPayloadComplete) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ChunkedNoTerminatorIncomplete) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n6\r\n world\r\n";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ChunkedTransferEncodingCaseInsensitive) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "transfer-encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

// ═══════════════════════════════════════════════════════════════════════
// Adversarial chunk data — false positive risk
// ═══════════════════════════════════════════════════════════════════════
//
// The current chunked detector substring-searches for "0\r\n\r\n" or
// "\r\n0\r\n\r\n" in the body.  It does NOT parse chunk sizes — so
// any byte pattern in chunk DATA that incidentally matches the
// terminator will cause false-positive completion.
//
// These tests pin the current behavior so any future tightening
// (e.g., proper chunk-size walking) is intentional.

TEST(IsCompleteAdv, ChunkedDataContainingZeroCrlfCrlfNotConfused) {
    // Adversarial: an 8-byte chunk whose DATA bytes happen to be
    // "0\r\n\r\nXYZ".  A naïve substring search for "0\r\n\r\n"
    // mid-body would falsely report completion mid-stream.  The
    // fixed parser anchors the match to "\r\n0\r\n\r\n" (chunk
    // boundary) so the embedded sequence is ignored.
    //
    // Mid-stream form: real terminator NOT yet present.  Buggy
    // parser would report complete; correct parser must report
    // incomplete.
    std::string mid =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "8\r\n0\r\n\r\nXYZ\r\n5\r\nhello\r\n";
    EXPECT_FALSE(is_http_response_complete(mid))
        << "embedded \"0\\r\\n\\r\\n\" inside chunk data must NOT trip "
           "false-positive completion";

    // Full form: same embedded sequence, plus the real terminator
    // at the end.  Correct parser must report complete.
    std::string full =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "8\r\n0\r\n\r\nXYZ\r\n5\r\nhello\r\n0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(full));
}

TEST(IsCompleteAdv, ChunkedExtensionsOnSizeLine) {
    // RFC 7230 §4.1.1 allows chunk-extensions on the size line:
    //   chunk-size [ ; ext-name [ = ext-value ] ]
    // The proper chunked walker strips at ';' before parsing the
    // size, so the final chunk "0;ext=foo\r\n\r\n" is recognized.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0;ext=foo\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ChunkedHexSizeLargerThan9) {
    // Chunk size in hex: 0x10 = 16 bytes.  The lazy substring matcher
    // wouldn't notice; the proper walker reads it correctly.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "10\r\n0123456789abcdef\r\n0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ChunkedTrailersWalkedToEmptyLine) {
    // The terminator chunk "0" can be followed by trailer headers
    // before the final empty line.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\nX-Trailer: value\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ChunkedTrailersIncompleteIsIncomplete) {
    // Same as above but missing the final empty line.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\nX-Trailer: value\r\n";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ChunkedMalformedSizeLineRejected) {
    // Non-hex characters in the size line.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "ZZZ\r\nhello\r\n";
    EXPECT_FALSE(is_http_response_complete(resp));
}

// ═══════════════════════════════════════════════════════════════════════
// Connection-close (no Content-Length, no chunked)
// ═══════════════════════════════════════════════════════════════════════

TEST(IsCompleteAdv, NoCLNoChunkedReturnsFalse) {
    // Without Content-Length or chunked, the parser cannot know if the
    // response is complete — must wait for connection close (caller
    // responsibility).  Always returns false until cap.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Server: test\r\n"
        "\r\n"
        "some body";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, NoCLNoChunkedExceedingCapReturnsTrue) {
    // The parser's safety cap (kMaxNoClBuffer = 16 MiB) treats a
    // 17 MiB no-CL-no-chunked buffer as complete to prevent OOM.
    std::string resp;
    resp.reserve(17 * 1024 * 1024);
    resp = "HTTP/1.1 200 OK\r\nServer: t\r\n\r\n";
    resp.append(17 * 1024 * 1024, 'A');
    EXPECT_TRUE(is_http_response_complete(resp));
}

// ═══════════════════════════════════════════════════════════════════════
// Content-Length + Transfer-Encoding: chunked together (RFC 7230 §3.3.3)
// ═══════════════════════════════════════════════════════════════════════
//
// RFC 7230 §3.3.3:
//   "If a message is received with both a Transfer-Encoding and a
//    Content-Length header field, the Transfer-Encoding overrides the
//    Content-Length.  Such a message might indicate an attempt to
//    perform request smuggling (Section 9.5) or response splitting
//    (Section 9.4)..."
//
// We honor TE precedence so a malicious or buggy server cannot trick
// the client by setting an inconsistent CL.

TEST(IsCompleteAdv, BothCLAndChunkedUsesChunked_Complete) {
    // Content-Length lies (says 100), but the chunked stream is
    // properly terminated.  RFC 7230: TE wins → complete.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 100\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, BothCLAndChunkedUsesChunked_Incomplete) {
    // Content-Length says 5 (matches the body bytes if we used CL).
    // But the chunked terminator is NOT present yet — TE precedence
    // means we report incomplete despite the CL match.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello"; // missing trailing \r\n + 0\r\n\r\n
    EXPECT_FALSE(is_http_response_complete(resp))
        << "Transfer-Encoding overrides Content-Length per RFC 7230 §3.3.3";
}

TEST(IsCompleteAdv, BothCLAndChunkedHeaderOrderIndependent) {
    // Same as above but Transfer-Encoding appears BEFORE Content-Length.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

// ═══════════════════════════════════════════════════════════════════════
// Multiple Content-Length headers
// ═══════════════════════════════════════════════════════════════════════

TEST(IsCompleteAdv, DuplicateContentLengthUsesFirst) {
    // RFC 7230 §3.3.3 requires duplicate Content-Length to be rejected
    // (or all values must agree).  The current parser uses find_header
    // which returns the FIRST occurrence — pin this behavior.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 99\r\n"
        "\r\n"
        "hello";
    // First CL=5, body has 5 bytes — complete by first value.
    EXPECT_TRUE(is_http_response_complete(resp));
}

// ═══════════════════════════════════════════════════════════════════════
// Boundary: header buffer exactly at \r\n\r\n
// ═══════════════════════════════════════════════════════════════════════

TEST(IsCompleteAdv, OnlyHeadersNoCLNoBody) {
    // No body whatsoever, no Content-Length declared.
    std::string resp = "HTTP/1.1 200 OK\r\n\r\n";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, EmptyBufferReturnsFalse) {
    EXPECT_FALSE(is_http_response_complete(""));
}

TEST(IsCompleteAdv, SingleByteBufferReturnsFalse) {
    EXPECT_FALSE(is_http_response_complete("H"));
}

TEST(IsCompleteAdv, NoHeaderTerminatorReturnsFalse) {
    EXPECT_FALSE(is_http_response_complete("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"));
}

// ═══════════════════════════════════════════════════════════════════════
// Body containing \r\n\r\n inside it
// ═══════════════════════════════════════════════════════════════════════

TEST(IsCompleteAdv, BodyContainingCrlfCrlfBytes) {
    // The header parser uses the FIRST occurrence of \r\n\r\n to
    // delimit headers from body — once delimited, body bytes can
    // include further \r\n\r\n without confusing completion.
    std::string body = "before\r\n\r\nmiddle\r\n\r\nafter";
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;
    EXPECT_TRUE(is_http_response_complete(resp));
}
