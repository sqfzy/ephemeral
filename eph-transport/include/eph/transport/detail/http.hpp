#pragma once

/// @file http.hpp
/// Minimal HTTP/1.1 implementation — only WebSocket Upgrade support.
///
/// Builds HTTP Upgrade requests and parses 101 Switching Protocols responses.
/// Not a general-purpose HTTP library — intentionally minimal for low latency.

#include <charconv>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "eph/core/detail/base64.hpp"

namespace eph::net::http {

// ─────────────────────────────────────────────────────────────────────────────
// Base64 encoding (for WebSocket key)
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

// Delegate to shared base64 implementation to avoid duplication
// with proxy.hpp and other modules that need base64 encoding.
using eph::core::detail::base64_encode;

/// @return Pointer to the "net.http" spdlog logger.
inline spdlog::logger* http_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.http");
        if (!lg) lg = spdlog::stdout_color_mt("net.http");
        return lg;
    }();
    return l.get();
}

/// Case-insensitive string comparison for HTTP header matching.
inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket Upgrade request builder
// ─────────────────────────────────────────────────────────────────────────────

/// Generate a random 16-byte WebSocket key, base64-encoded.
[[nodiscard]] inline std::expected<std::string, std::string> generate_ws_key() {
    uint8_t raw[16];
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        SPDLOG_LOGGER_ERROR(detail::http_logger(),
            "RAND_bytes failed for WebSocket key generation");
        return std::unexpected("RAND_bytes failed for WebSocket key generation");
    }
    return detail::base64_encode(raw, sizeof(raw));
}

/// Build an HTTP/1.1 WebSocket Upgrade request.
///
/// @param host     Server hostname (for Host header)
/// @param path     Request URI path (e.g. "/ws" or "/stream")
/// @param ws_key   Base64-encoded 16-byte random key
/// @param extra_headers  Additional headers (e.g. "Authorization: Bearer xxx\r\n")
/// @return Complete HTTP request string, or error describing which param is invalid
[[nodiscard]] inline std::expected<std::string, std::string> build_upgrade_request(
    std::string_view host,
    std::string_view path,
    std::string_view ws_key,
    std::string_view extra_headers = {}) {

    if (host.empty() || path.empty() || ws_key.empty()) [[unlikely]] {
        auto msg = std::format(
            "build_upgrade_request: invalid params: host={}, path={}, key={}",
            host.empty() ? "(empty)" : host,
            path.empty() ? "(empty)" : path,
            ws_key.empty() ? "(empty)" : ws_key);
        SPDLOG_LOGGER_ERROR(detail::http_logger(), "{}", msg);
        return std::unexpected(std::move(msg));
    }

    return std::format(
        "GET {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "{}"
        "\r\n",
        path, host, ws_key, extra_headers);
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP response parser (minimal — only handles 101 Switching Protocols)
// ─────────────────────────────────────────────────────────────────────────────

/// Parsed HTTP response fields relevant to the WebSocket Upgrade handshake.
///
/// Only the fields needed to validate a 101 Switching Protocols response
/// are extracted; all other headers are ignored.
struct UpgradeResponse {
    int         status_code = 0;          ///< HTTP status code (101 expected)
    std::string sec_ws_accept{};          ///< Sec-WebSocket-Accept header value
    std::string sec_ws_protocol{};        ///< Negotiated subprotocol (may be empty)
    bool        has_upgrade = false;      ///< True if "Upgrade: websocket" header present
    bool        has_connection_upgrade = false; ///< True if "Connection" header contains "upgrade" token
    size_t      header_end_offset = 0;    ///< Byte position after the "\\r\\n\\r\\n" header terminator
};

/// Parse an HTTP response looking for 101 Switching Protocols.
/// @param data  Raw HTTP response data
/// @param len   Data length
/// @return Parsed response, or error if incomplete/malformed
[[nodiscard]] inline std::expected<UpgradeResponse, std::string>
parse_upgrade_response(const char* data, size_t len) {
    [[maybe_unused]] auto log = detail::http_logger();

    std::string_view response(data, len);

    // Find end of headers
    auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        return std::unexpected("Incomplete HTTP response (no \\r\\n\\r\\n)");
    }

    UpgradeResponse result;
    result.header_end_offset = header_end + 4;

    // Parse status line: "HTTP/1.1 101 Switching Protocols\r\n"
    auto first_line_end = response.find("\r\n");
    if (first_line_end == std::string_view::npos) {
        return std::unexpected("Malformed HTTP response");
    }

    auto status_line = response.substr(0, first_line_end);

    // Find status code (after first space)
    auto space1 = status_line.find(' ');
    if (space1 == std::string_view::npos) {
        return std::unexpected("Malformed status line");
    }

    auto code_start = space1 + 1;
    auto space2 = status_line.find(' ', code_start);
    auto code_str = status_line.substr(code_start,
        (space2 != std::string_view::npos) ? space2 - code_start : std::string_view::npos);

    result.status_code = 0;
    auto [ptr, ec] = std::from_chars(
        code_str.data(), code_str.data() + code_str.size(),
        result.status_code);
    if (ec != std::errc{}) {
        SPDLOG_LOGGER_WARN(log,
            "Failed to parse HTTP status code from '{}'",
            std::string(code_str));
        return std::unexpected(std::format(
            "Malformed HTTP status code: '{}'", code_str));
    }

    SPDLOG_LOGGER_DEBUG(log, "HTTP response status: {}", result.status_code);

    // Parse headers (case-insensitive).
    // Include the trailing \r\n before \r\n\r\n so the last header line
    // is terminated and found by the line-by-line parser.
    auto headers = response.substr(first_line_end + 2,
                                    header_end - first_line_end);

    // Simple header parser — line by line
    size_t pos = 0;
    while (pos < headers.size()) {
        auto line_end = headers.find("\r\n", pos);
        if (line_end == std::string_view::npos) break;

        auto line = headers.substr(pos, line_end - pos);
        pos = line_end + 2;

        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;

        auto name = line.substr(0, colon);
        auto value = line.substr(colon + 1);

        // Trim leading and trailing OWS (RFC 7230 §3.2.6: SP / HTAB)
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.remove_suffix(1);
        }

        // Reject header values containing bare CR or LF (header injection defense)
        if (name.find('\r') != std::string_view::npos ||
            name.find('\n') != std::string_view::npos ||
            value.find('\r') != std::string_view::npos ||
            value.find('\n') != std::string_view::npos) {
            SPDLOG_LOGGER_WARN(log,
                "Header injection attempt detected: header name or value contains embedded CR/LF");
            continue;
        }

        if (detail::iequals(name, "Upgrade")) {
            result.has_upgrade = detail::iequals(value, "websocket");
        } else if (detail::iequals(name, "Connection")) {
            // Connection header is a comma-separated list of tokens (RFC 7230 §6.1).
            // Match whole tokens case-insensitively to avoid "noupgrade" false positives.
            result.has_connection_upgrade = [&] {
                size_t tok_pos = 0;
                while (tok_pos < value.size()) {
                    // Skip leading whitespace
                    while (tok_pos < value.size() && (value[tok_pos] == ' ' || value[tok_pos] == '\t'))
                        ++tok_pos;
                    // Find token end (comma or end of string)
                    size_t end = value.find(',', tok_pos);
                    if (end == std::string_view::npos) end = value.size();
                    // Trim trailing whitespace from token
                    size_t tok_end = end;
                    while (tok_end > tok_pos && (value[tok_end - 1] == ' ' || value[tok_end - 1] == '\t'))
                        --tok_end;
                    if (detail::iequals(value.substr(tok_pos, tok_end - tok_pos), "upgrade"))
                        return true;
                    tok_pos = (end < value.size()) ? end + 1 : end;
                }
                return false;
            }();
        } else if (detail::iequals(name, "Sec-WebSocket-Accept")) {
            result.sec_ws_accept = std::string(value);
        } else if (detail::iequals(name, "Sec-WebSocket-Protocol")) {
            result.sec_ws_protocol = std::string(value);
        }
    }

    return result;
}

/// Validate the Sec-WebSocket-Accept header against the client key.
///
/// Computes Base64(SHA-1(ws_key + GUID)) per RFC 6455 section 4.2.2 and
/// compares it with the server's Accept header value.
///
/// @param ws_key        The Sec-WebSocket-Key sent in the Upgrade request
/// @param accept_value  The Sec-WebSocket-Accept value from the server's response
/// @return true if the accept value matches the expected hash
inline bool validate_ws_accept(std::string_view ws_key,
                                std::string_view accept_value) {
    static constexpr std::string_view kMagicGuid =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    // Concatenate key + GUID
    std::string input;
    input.reserve(ws_key.size() + kMagicGuid.size());
    input.append(ws_key);
    input.append(kMagicGuid);

    // SHA-1 hash (required by RFC 6455, not for cryptographic security)
    uint8_t hash[20];
    unsigned int hash_len = 0;
    if (!EVP_Digest(input.data(), input.size(), hash, &hash_len,
                    EVP_sha1(), nullptr)) {
        SPDLOG_LOGGER_ERROR(detail::http_logger(),
            "EVP_Digest(SHA-1) failed during WebSocket accept validation");
        return false;
    }

    // Base64 encode
    std::string expected = detail::base64_encode(hash, hash_len);

    return expected == accept_value;
}

} // namespace eph::net::http
