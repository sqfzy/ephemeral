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

TEST(IsCompleteAdv, DuplicateContentLengthDifferentValuesRejected) {
    // RFC 7230 §3.3.2: multiple Content-Length headers with
    // conflicting values MUST be rejected (smuggling defense).
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 99\r\n"
        "\r\n"
        "hello";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, DuplicateContentLengthIdenticalValuesAccepted) {
    // RFC 7230 §3.3.2: identical duplicates MAY be accepted as if
    // a single header (lenient interpretation).
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthCommaListIdenticalValuesAccepted) {
    // RFC 7230 §3.3.2: a single header with comma-separated identical
    // values is equivalent to multiple identical headers.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5, 5\r\n"
        "\r\n"
        "hello";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthCommaListDifferentValuesRejected) {
    // The classic CL/CL desync attack: "5, 99" — proxy and server
    // disagree on which value to honor.  Reject.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5, 99\r\n"
        "\r\n"
        "hello";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthEmbeddedWhitespaceRejected) {
    // Adversarial: "5 0" with embedded space.  RFC 7230 §3.2 disallows
    // whitespace inside field values (only leading/trailing OWS).  A
    // proxy might accept it as 50 while we accept it as something
    // else — desync.  Reject.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5 0\r\n"
        "\r\n"
        "hello";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthEmbeddedTabRejected) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\t0\r\n"
        "\r\n"
        "hello";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthHexValueRejected) {
    // "0x05" is not a valid decimal CL.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0x05\r\n"
        "\r\n"
        "hello";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthLeadingZerosAccepted) {
    // Leading zeros are syntactically a decimal number.
    // Pin current behavior so any future tightening is intentional.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 005\r\n"
        "\r\n"
        "hello";
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

// ═══════════════════════════════════════════════════════════════════════
// RFC 7230 §3.2.4 — whitespace between header field-name and colon
// ═══════════════════════════════════════════════════════════════════════
//
// RFC 7230 §3.2.4:
//   "No whitespace is allowed between the header field-name and colon.
//    In the past, differences in the handling of such whitespace have
//    led to security vulnerabilities in request routing and response
//    handling.  A server MUST reject [requests] with [whitespace
//    between field-name and colon].  A proxy MUST remove any such
//    whitespace from a response message before forwarding the message
//    downstream."
//
// is_http_response_complete previously matched header names with an
// EXACT byte-length comparison (`hdr_name.size() != kClName.size()`),
// which means a header line `Content-Length : 5\r\n` (with SP before
// the colon) was silently ignored.  Combined with a downstream proxy
// that strips the whitespace and DOES use the value, this is a clean
// smuggling primitive: we and the proxy disagree on framing.
//
// Defense: trim trailing OWS (SP / HTAB) from header names so we see
// what an RFC-compliant proxy would forward.

TEST(IsCompleteAdv, ContentLengthWithSpaceBeforeColonStillHonored) {
    // 5-byte body announced via "Content-Length : 5" — must be
    // recognized as complete, NOT silently ignored.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length : 5\r\n"
        "\r\n"
        "12345";
    EXPECT_TRUE(is_http_response_complete(resp))
        << "header name with trailing SP must be honored (RFC 7230 §3.2.4)";
}

TEST(IsCompleteAdv, ContentLengthWithSpaceBeforeColonShortBodyIncomplete) {
    // Confirm completion logic still uses the announced length:
    // declared 10 bytes, only 3 received → incomplete.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length : 10\r\n"
        "\r\n"
        "abc";
    EXPECT_FALSE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthWithTabBeforeColonStillHonored) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length\t: 4\r\n"
        "\r\n"
        "abcd";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, TransferEncodingWithSpaceBeforeColonStillTriggersChunkedPath) {
    // The chunked walker must also see "Transfer-Encoding : chunked"
    // — otherwise we'd ignore TE, also ignore CL (if any), and fall
    // back to read-until-close while a downstream proxy correctly
    // frames each chunk.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding : chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}

TEST(IsCompleteAdv, ContentLengthAndTeBothSpacedTeWins) {
    // Per RFC 7230 §3.3.3: TE wins over CL.  This must hold even
    // when both header NAMES carry whitespace before the colon.
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length : 100\r\n"
        "Transfer-Encoding : chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    EXPECT_TRUE(is_http_response_complete(resp));
}
