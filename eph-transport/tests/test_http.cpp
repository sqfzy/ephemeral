/// @file test_http.cpp
/// Tests for HTTP upgrade request/response utilities.
/// Covers: build_upgrade_request validation, parse_upgrade_response edge cases,
/// validate_ws_accept correctness, and generate_ws_key format.

#include <cstring>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/transport/detail/http.hpp"

using namespace eph::net::http;

// =======================================================================
// generate_ws_key
// =======================================================================

TEST(GenerateWsKey, ReturnsBase64EncodedString) {
    auto result = generate_ws_key();
    ASSERT_TRUE(result.has_value());
    // 16 random bytes -> 24 chars base64 (with padding)
    EXPECT_EQ(result->size(), 24u);
}

TEST(GenerateWsKey, TwoCallsProduceDifferentKeys) {
    auto k1 = generate_ws_key();
    auto k2 = generate_ws_key();
    ASSERT_TRUE(k1.has_value());
    ASSERT_TRUE(k2.has_value());
    EXPECT_NE(*k1, *k2);
}

// =======================================================================
// build_upgrade_request
// =======================================================================

TEST(BuildUpgradeRequest, ValidInputProducesCorrectRequest) {
    auto result = build_upgrade_request("example.com", "/ws", "dGVzdA==");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("GET /ws HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(result->find("Host: example.com\r\n"), std::string::npos);
    EXPECT_NE(result->find("Upgrade: websocket\r\n"), std::string::npos);
    EXPECT_NE(result->find("Connection: Upgrade\r\n"), std::string::npos);
    EXPECT_NE(result->find("Sec-WebSocket-Key: dGVzdA==\r\n"), std::string::npos);
    EXPECT_NE(result->find("Sec-WebSocket-Version: 13\r\n"), std::string::npos);
    // Ends with \r\n\r\n (blank line terminator)
    EXPECT_TRUE(result->ends_with("\r\n\r\n"));
}

TEST(BuildUpgradeRequest, EmptyHostReturnsError) {
    auto result = build_upgrade_request("", "/ws", "dGVzdA==");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("empty"), std::string::npos);
}

TEST(BuildUpgradeRequest, EmptyPathReturnsError) {
    auto result = build_upgrade_request("host", "", "dGVzdA==");
    ASSERT_FALSE(result.has_value());
}

TEST(BuildUpgradeRequest, EmptyKeyReturnsError) {
    auto result = build_upgrade_request("host", "/ws", "");
    ASSERT_FALSE(result.has_value());
}

TEST(BuildUpgradeRequest, ExtraHeadersAppended) {
    auto result = build_upgrade_request(
        "host", "/ws", "dGVzdA==", "Authorization: Bearer tok\r\n");
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(result->find("Authorization: Bearer tok\r\n"), std::string::npos);
}

TEST(BuildUpgradeRequest, EmptyExtraHeadersOk) {
    auto result = build_upgrade_request("host", "/ws", "dGVzdA==", "");
    ASSERT_TRUE(result.has_value());
}

// =======================================================================
// parse_upgrade_response — valid 101 response
// =======================================================================

TEST(ParseUpgradeResponse, Valid101Response) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 101);
    EXPECT_TRUE(result->has_upgrade);
    EXPECT_TRUE(result->has_connection_upgrade);
    EXPECT_EQ(result->sec_ws_accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    EXPECT_EQ(result->header_end_offset, response.size());
}

// =======================================================================
// parse_upgrade_response — non-101 status codes
// =======================================================================

TEST(ParseUpgradeResponse, Status403Forbidden) {
    std::string response =
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 403);
    EXPECT_FALSE(result->has_upgrade);
    EXPECT_FALSE(result->has_connection_upgrade);
}

TEST(ParseUpgradeResponse, Status200Ok) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 200);
}

// =======================================================================
// parse_upgrade_response — incomplete / malformed
// =======================================================================

TEST(ParseUpgradeResponse, IncompleteResponseReturnsError) {
    std::string partial = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n";
    auto result = parse_upgrade_response(partial.data(), partial.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Incomplete"), std::string::npos);
}

TEST(ParseUpgradeResponse, EmptyInputReturnsError) {
    auto result = parse_upgrade_response("", 0);
    ASSERT_FALSE(result.has_value());
}

TEST(ParseUpgradeResponse, NoStatusLineReturnsError) {
    // Only header terminator, no proper status line
    std::string response = "\r\n\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_FALSE(result.has_value());
}

TEST(ParseUpgradeResponse, MalformedStatusCodeReturnsError) {
    std::string response =
        "HTTP/1.1 XYZ Bad\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Malformed"), std::string::npos);
}

// =======================================================================
// parse_upgrade_response — Connection header token parsing
// =======================================================================

TEST(ParseUpgradeResponse, ConnectionHeaderMultipleTokens) {
    // "Connection: keep-alive, Upgrade" should detect "upgrade" token
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Accept: abc\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_connection_upgrade);
}

TEST(ParseUpgradeResponse, ConnectionHeaderNoUpgradeToken) {
    // "Connection: keep-alive" should NOT match
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->has_connection_upgrade);
}

TEST(ParseUpgradeResponse, ConnectionHeaderCaseInsensitive) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: WebSocket\r\n"
        "connection: upgrade\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_upgrade);
    EXPECT_TRUE(result->has_connection_upgrade);
}

// =======================================================================
// parse_upgrade_response — header value whitespace trimming
// =======================================================================

TEST(ParseUpgradeResponse, HeaderValueLeadingTrailingWhitespace) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade:  \twebsocket \t\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept:  abc123  \r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_upgrade);
    EXPECT_EQ(result->sec_ws_accept, "abc123");
}

// =======================================================================
// parse_upgrade_response — subprotocol extraction
// =======================================================================

TEST(ParseUpgradeResponse, SubprotocolExtracted) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Protocol: graphql-ws\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sec_ws_protocol, "graphql-ws");
}

TEST(ParseUpgradeResponse, NoSubprotocolIsEmpty) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->sec_ws_protocol.empty());
}

// =======================================================================
// parse_upgrade_response — header_end_offset
// =======================================================================

TEST(ParseUpgradeResponse, HeaderEndOffsetPointsAfterTerminator) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "\r\n"
        "extra body data";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    // header_end_offset should point right after \r\n\r\n
    auto expected_end = response.find("\r\n\r\n") + 4;
    EXPECT_EQ(result->header_end_offset, expected_end);
}

// =======================================================================
// parse_upgrade_response — header injection defense
// =======================================================================

TEST(ParseUpgradeResponse, HeaderWithEmbeddedCRLFAborts) {
    // A header containing embedded CR or LF must fail-fast the entire parse
    // (not silently skip) to prevent header injection attacks.
    // Construct a response where a header value contains a bare \n:
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n";
    // Inject a header with embedded \n in the value
    response += "X-Evil: foo\nbar\r\n";
    response += "Sec-WebSocket-Accept: validvalue\r\n";
    response += "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("injection"), std::string::npos);
}

TEST(ParseUpgradeResponse, CleanHeadersStillWork) {
    // Verify clean headers without embedded CR/LF parse correctly.
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: validvalue\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sec_ws_accept, "validvalue");
}

// =======================================================================
// validate_ws_accept
// =======================================================================

TEST(ValidateWsAccept, CorrectKeyProducesMatch) {
    // RFC 6455 test vector:
    // Key: "dGhlIHNhbXBsZSBub25jZQ=="
    // Expected accept: "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    EXPECT_TRUE(validate_ws_accept(
        "dGhlIHNhbXBsZSBub25jZQ==", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST(ValidateWsAccept, WrongAcceptValueRejectsMismatch) {
    EXPECT_FALSE(validate_ws_accept(
        "dGhlIHNhbXBsZSBub25jZQ==", "WRONGVALUE"));
}

TEST(ValidateWsAccept, EmptyAcceptRejects) {
    EXPECT_FALSE(validate_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", ""));
}

TEST(ValidateWsAccept, EmptyKeyProducesValidHash) {
    // Even an empty key is hashed with the GUID — should not crash
    // and should produce a deterministic result
    auto result1 = validate_ws_accept("", "");
    // Just verify it doesn't crash; the exact result depends on SHA1("")
    (void)result1;
}

// =======================================================================
// detail::iequals
// =======================================================================

TEST(IEquals, SameStringReturnsTrue) {
    EXPECT_TRUE(detail::iequals("websocket", "websocket"));
}

TEST(IEquals, CaseInsensitiveMatch) {
    EXPECT_TRUE(detail::iequals("WebSocket", "websocket"));
    EXPECT_TRUE(detail::iequals("UPGRADE", "upgrade"));
    EXPECT_TRUE(detail::iequals("Connection", "connection"));
}

TEST(IEquals, DifferentStringsReturnFalse) {
    EXPECT_FALSE(detail::iequals("websocket", "webSocket2"));
    EXPECT_FALSE(detail::iequals("a", "b"));
}

TEST(IEquals, DifferentLengthsReturnFalse) {
    EXPECT_FALSE(detail::iequals("short", "shorter"));
    EXPECT_FALSE(detail::iequals("longer", "long"));
}

TEST(IEquals, EmptyStringsMatch) {
    EXPECT_TRUE(detail::iequals("", ""));
}

TEST(IEquals, EmptyVsNonEmptyReturnFalse) {
    EXPECT_FALSE(detail::iequals("", "a"));
    EXPECT_FALSE(detail::iequals("a", ""));
}

// =======================================================================
// parse_upgrade_response — status line with no reason phrase
// =======================================================================

TEST(ParseUpgradeResponse, StatusLineWithNoReasonPhrase) {
    // Some servers send "HTTP/1.1 101\r\n" without a reason phrase
    std::string response =
        "HTTP/1.1 101\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 101);
}

// =======================================================================
// parse_upgrade_response — headers without colon are skipped
// =======================================================================

TEST(ParseUpgradeResponse, LineWithoutColonIsSkipped) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "InvalidHeaderNoColon\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_upgrade);
    EXPECT_TRUE(result->has_connection_upgrade);
}
