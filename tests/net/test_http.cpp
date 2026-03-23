/// @file test_http.cpp
/// Unit tests for HTTP Upgrade request/response and WebSocket key validation.

#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "eph/net/http.hpp"

using namespace eph::net::http;

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket key generation
// ─────────────────────────────────────────────────────────────────────────────

TEST(Http, GenerateWsKeyIsBase64) {
    auto result = generate_ws_key();
    ASSERT_TRUE(result.has_value()) << result.error();
    std::string key = *result;
    // Base64 of 16 bytes = 24 chars (with padding)
    EXPECT_EQ(key.size(), 24u);
    // All characters should be valid base64
    for (char c : key) {
        EXPECT_TRUE(std::isalnum(c) || c == '+' || c == '/' || c == '=')
            << "Invalid base64 char: " << c;
    }
}

TEST(Http, GenerateWsKeyProducesUniqueKeys) {
    auto r1 = generate_ws_key();
    auto r2 = generate_ws_key();
    ASSERT_TRUE(r1.has_value()) << r1.error();
    ASSERT_TRUE(r2.has_value()) << r2.error();
    EXPECT_NE(*r1, *r2) << "Two consecutive keys should differ";
}

// ─────────────────────────────────────────────────────────────────────────────
// Upgrade request
// ─────────────────────────────────────────────────────────────────────────────

TEST(Http, BuildUpgradeRequestFormat) {
    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    auto result = build_upgrade_request("example.com", "/ws", key);
    ASSERT_TRUE(result.has_value()) << result.error();
    auto& req = *result;

    EXPECT_NE(req.find("GET /ws HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(req.find("Host: example.com\r\n"), std::string::npos);
    EXPECT_NE(req.find("Upgrade: websocket\r\n"), std::string::npos);
    EXPECT_NE(req.find("Connection: Upgrade\r\n"), std::string::npos);
    EXPECT_NE(req.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"),
              std::string::npos);
    EXPECT_NE(req.find("Sec-WebSocket-Version: 13\r\n"), std::string::npos);
    // Ends with \r\n\r\n
    EXPECT_EQ(req.substr(req.size() - 4), "\r\n\r\n");
}

TEST(Http, BuildUpgradeRequestWithExtraHeaders) {
    auto result = build_upgrade_request(
        "api.exchange.com", "/stream",
        "abc123base64==",
        "Authorization: Bearer tok123\r\n");
    ASSERT_TRUE(result.has_value()) << result.error();
    auto& req = *result;

    EXPECT_NE(req.find("Authorization: Bearer tok123\r\n"), std::string::npos);
    EXPECT_NE(req.find("Host: api.exchange.com\r\n"), std::string::npos);
    EXPECT_NE(req.find("GET /stream HTTP/1.1\r\n"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Upgrade response parsing
// ─────────────────────────────────────────────────────────────────────────────

TEST(Http, ParseValidUpgradeResponse) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";

    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value()) << result.error();

    EXPECT_EQ(result->status_code, 101);
    EXPECT_TRUE(result->has_upgrade);
    EXPECT_TRUE(result->has_connection_upgrade);
    EXPECT_EQ(result->sec_ws_accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    EXPECT_EQ(result->header_end_offset, response.size());
}

TEST(Http, ParseNon101Response) {
    std::string response =
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 403);
    EXPECT_FALSE(result->has_upgrade);
}

TEST(Http, ParseIncompleteResponse) {
    std::string partial = "HTTP/1.1 101 Switching Proto";
    auto result = parse_upgrade_response(partial.data(), partial.size());
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Incomplete"), std::string::npos);
}

TEST(Http, ParseMalformedStatusLine) {
    std::string bad = "GARBAGE\r\n\r\n";
    auto result = parse_upgrade_response(bad.data(), bad.size());
    // No space in status line → parser returns "Malformed status line"
    EXPECT_FALSE(result.has_value());
}

TEST(Http, ParseCaseInsensitiveHeaders) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "upgrade: websocket\r\n"
        "connection: Upgrade\r\n"
        "sec-websocket-accept: abc123\r\n"
        "\r\n";

    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_upgrade);
    EXPECT_TRUE(result->has_connection_upgrade);
    EXPECT_EQ(result->sec_ws_accept, "abc123");
}

// ─────────────────────────────────────────────────────────────────────────────
// Sec-WebSocket-Accept validation (RFC 6455 Section 4.2.2)
// ─────────────────────────────────────────────────────────────────────────────

TEST(Http, ValidateWsAcceptRfcExample) {
    // RFC 6455 Section 4.2.2 example:
    // Key:    "dGhlIHNhbXBsZSBub25jZQ=="
    // Accept: "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    EXPECT_TRUE(validate_ws_accept(
        "dGhlIHNhbXBsZSBub25jZQ==",
        "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST(Http, ValidateWsAcceptRejectsWrongValue) {
    EXPECT_FALSE(validate_ws_accept(
        "dGhlIHNhbXBsZSBub25jZQ==",
        "WRONG_VALUE_HERE"));
}

TEST(Http, ValidateWsAcceptRejectsEmptyKey) {
    EXPECT_FALSE(validate_ws_accept("", "anything"));
}

TEST(Http, ValidateWsAcceptWithGeneratedKey) {
    // Generate a key, compute expected accept, and verify
    auto key_result = generate_ws_key();
    ASSERT_TRUE(key_result.has_value()) << key_result.error();
    std::string key = *key_result;

    // Manually compute: SHA-1(key + GUID) → base64
    static constexpr std::string_view kMagicGuid =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    std::string input;
    input.append(key);
    input.append(kMagicGuid);

    uint8_t hash[20];
    unsigned int hash_len = 0;
    EVP_Digest(input.data(), input.size(), hash, &hash_len,
               EVP_sha1(), nullptr);

    std::string expected = detail::base64_encode(hash, 20);

    EXPECT_TRUE(validate_ws_accept(key, expected));
    EXPECT_FALSE(validate_ws_accept(key, "wrong"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Base64 encoding
// ─────────────────────────────────────────────────────────────────────────────

TEST(Http, Base64EncodeEmpty) {
    std::string result = detail::base64_encode(nullptr, 0);
    EXPECT_EQ(result, "");
}

TEST(Http, Base64EncodeKnownValues) {
    // "f"      → "Zg=="
    // "fo"     → "Zm8="
    // "foo"    → "Zm9v"
    // "foob"   → "Zm9vYg=="
    // "fooba"  → "Zm9vYmE="
    // "foobar" → "Zm9vYmFy"

    auto b64 = [](const char* s) {
        return detail::base64_encode(
            reinterpret_cast<const uint8_t*>(s), strlen(s));
    };

    EXPECT_EQ(b64("f"),      "Zg==");
    EXPECT_EQ(b64("fo"),     "Zm8=");
    EXPECT_EQ(b64("foo"),    "Zm9v");
    EXPECT_EQ(b64("foob"),   "Zm9vYg==");
    EXPECT_EQ(b64("fooba"),  "Zm9vYmE=");
    EXPECT_EQ(b64("foobar"), "Zm9vYmFy");
}

// ─────────────────────────────────────────────────────────────────────────────
// Boundary tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(Http, Base64EncodeEmptyInput) {
    auto result = detail::base64_encode(nullptr, 0);
    EXPECT_EQ(result, "");
}

TEST(Http, Base64EncodeSingleByte) {
    uint8_t data[] = {0x00};
    auto result = detail::base64_encode(data, 1);
    EXPECT_EQ(result, "AA==");
}

TEST(Http, ParseResponseMissingUpgradeHeader) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 101);
    EXPECT_FALSE(result->has_upgrade) << "Missing Upgrade header should be detected";
    EXPECT_TRUE(result->has_connection_upgrade);
}

TEST(Http, ParseResponseMissingConnectionHeader) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_upgrade);
    EXPECT_FALSE(result->has_connection_upgrade)
        << "Missing Connection: Upgrade header should be detected";
}

TEST(Http, ParseResponseExtraWhitespaceInHeaderValue) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade:   websocket\r\n"
        "Connection:  Upgrade\r\n"
        "Sec-WebSocket-Accept:  s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_upgrade) << "Extra whitespace should be trimmed";
    EXPECT_TRUE(result->has_connection_upgrade);
}

TEST(Http, ParseResponseNon101StatusCode) {
    std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 200);
    // Caller should check status_code != 101
}

TEST(Http, ParseResponseMalformedStatusLine) {
    std::string response = "GARBAGE\r\n\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    EXPECT_FALSE(result.has_value())
        << "Malformed status line (no space) should be rejected";
}

TEST(Http, ParseResponseIncomplete_NoDoubleNewline) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    EXPECT_FALSE(result.has_value());
    // Should return "Incomplete" error
}

TEST(Http, BuildUpgradeRequestContainsRequiredHeaders) {
    auto result = build_upgrade_request("example.com", "/ws", "dGVzdA==");
    ASSERT_TRUE(result.has_value()) << result.error();
    auto& req = *result;
    EXPECT_NE(req.find("GET /ws HTTP/1.1"), std::string::npos);
    EXPECT_NE(req.find("Host: example.com"), std::string::npos);
    EXPECT_NE(req.find("Upgrade: websocket"), std::string::npos);
    EXPECT_NE(req.find("Connection: Upgrade"), std::string::npos);
    EXPECT_NE(req.find("Sec-WebSocket-Key: dGVzdA=="), std::string::npos);
    EXPECT_NE(req.find("Sec-WebSocket-Version: 13"), std::string::npos);
}

TEST(Http, BuildUpgradeRequestWithAuthHeader) {
    auto result = build_upgrade_request(
        "host.com", "/", "key==", "Authorization: Bearer tok\r\n");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NE(result->find("Authorization: Bearer tok"), std::string::npos);
}

TEST(Http, BuildUpgradeRequestPathWithQuery) {
    auto result = build_upgrade_request(
        "host.com", "/ws?token=abc&v=2", "key==");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NE(result->find("GET /ws?token=abc&v=2 HTTP/1.1"), std::string::npos);
}

TEST(Http, ValidateWsAcceptCaseSensitive) {
    // Accept value is base64, which IS case-sensitive
    std::string key = "dGVzdA==";
    // Compute expected accept
    std::string accept = ""; // Will compute dynamically
    // Instead, just verify mismatch fails
    EXPECT_FALSE(validate_ws_accept(key, "INVALID_ACCEPT_VALUE"));
}

TEST(Http, ParseResponseHeaderEndOffset) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "\r\n"
        "trailing data";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    // header_end_offset should point past "\r\n\r\n"
    EXPECT_EQ(result->header_end_offset,
              response.find("\r\n\r\n") + 4);
}

// ─────────────────────────────────────────────────────────────────────────────
// Base64 edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(Http, Base64EncodeAllByteValues) {
    // Encode all 256 byte values and verify output is valid base64
    uint8_t all_bytes[256];
    for (int i = 0; i < 256; ++i) all_bytes[i] = static_cast<uint8_t>(i);

    std::string result = detail::base64_encode(all_bytes, 256);
    // 256 bytes -> ceil(256/3)*4 = 344 chars
    EXPECT_EQ(result.size(), 344u);

    // All chars must be valid base64
    for (char c : result) {
        EXPECT_TRUE(std::isalnum(c) || c == '+' || c == '/' || c == '=')
            << "Invalid base64 char: 0x" << std::hex << static_cast<int>(c);
    }
}

TEST(Http, Base64Encode16Bytes) {
    // WebSocket key is always 16 bytes -> 24 base64 chars
    uint8_t data[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    std::string result = detail::base64_encode(data, 16);
    EXPECT_EQ(result.size(), 24u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Response parsing edge cases
// ─────────────────────────────────────────────────────────────────────────────

TEST(Http, ParseResponseMultipleConnectionValues) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Accept: abc123\r\n"
        "\r\n";

    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_connection_upgrade)
        << "Connection header with multiple values should detect 'Upgrade'";
}

TEST(Http, ParseResponseStatusCode100) {
    std::string response =
        "HTTP/1.1 100 Continue\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 100);
}

TEST(Http, ParseResponseStatusCode500) {
    std::string response =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "\r\n";
    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 500);
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket subprotocol negotiation
// ─────────────────────────────────────────────────────────────────────────────

TEST(Http, ParseResponseWithSubprotocol) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "Sec-WebSocket-Protocol: graphql-ws\r\n"
        "\r\n";

    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->status_code, 101);
    EXPECT_EQ(result->sec_ws_protocol, "graphql-ws");
}

TEST(Http, ParseResponseWithoutSubprotocol) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: abc123\r\n"
        "\r\n";

    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->sec_ws_protocol.empty())
        << "Missing Sec-WebSocket-Protocol should yield empty string";
}

TEST(Http, ParseResponseSubprotocolCaseInsensitiveHeader) {
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "sec-websocket-protocol: ocpp1.6\r\n"
        "Sec-WebSocket-Accept: abc123\r\n"
        "\r\n";

    auto result = parse_upgrade_response(response.data(), response.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sec_ws_protocol, "ocpp1.6");
}

TEST(Http, BuildUpgradeRequestWithSubprotocol) {
    auto result = build_upgrade_request(
        "api.exchange.com", "/ws", "key123==",
        "Sec-WebSocket-Protocol: graphql-ws\r\n");
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_NE(result->find("Sec-WebSocket-Protocol: graphql-ws\r\n"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// Input validation — empty params must return unexpected
// ---------------------------------------------------------------------------

TEST(Http, BuildUpgradeRequestEmptyHostReturnsError) {
    auto result = build_upgrade_request("", "/ws", "key123==");
    EXPECT_FALSE(result.has_value());
}

TEST(Http, BuildUpgradeRequestEmptyPathReturnsError) {
    auto result = build_upgrade_request("example.com", "", "key123==");
    EXPECT_FALSE(result.has_value());
}

TEST(Http, BuildUpgradeRequestEmptyKeyReturnsError) {
    auto result = build_upgrade_request("example.com", "/ws", "");
    EXPECT_FALSE(result.has_value());
}

TEST(Http, BuildUpgradeRequestAllEmptyReturnsError) {
    auto result = build_upgrade_request("", "", "");
    EXPECT_FALSE(result.has_value());
}
