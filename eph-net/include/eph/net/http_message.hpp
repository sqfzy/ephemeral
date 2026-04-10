#pragma once

/// @file http_message.hpp
/// HTTP/1.1 message types, request builder, and response parser.
///
/// Pure value types and stateless functions — no network I/O, no TLS,
/// no POSIX headers. Suitable for unit testing without linking OpenSSL
/// or socket libraries.
///
/// For the synchronous HTTP client that uses these types, see http_client.hpp.

#include <charconv>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/core/detail/json_escape.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations (used by HttpResponse convenience methods)
// ─────────────────────────────────────────────────────────────────────────────

[[nodiscard]] inline std::string
find_header(std::string_view headers_raw, std::string_view name) noexcept;

[[nodiscard]] inline std::optional<std::string>
find_header_opt(std::string_view headers_raw, std::string_view name) noexcept;

// ─────────────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Parsed HTTP response.
///
/// Contains the status code, raw header block, and body text
/// from a completed HTTP/1.1 response.
struct HttpResponse {
    int         status_code = 0;  ///< HTTP status code (e.g. 200, 404, 500).
    std::string body;             ///< Response body text.
    std::string headers_raw;      ///< Raw header block (excluding status line) for optional parsing.

    /// Check if the response is informational (1xx status code).
    [[nodiscard]] constexpr bool is_informational() const noexcept {
        return status_code >= 100 && status_code < 200;
    }

    /// Check if the response indicates success (2xx status code).
    [[nodiscard]] constexpr bool is_success() const noexcept {
        return status_code >= 200 && status_code < 300;
    }

    /// Check if the response indicates a client error (4xx status code).
    [[nodiscard]] constexpr bool is_client_error() const noexcept {
        return status_code >= 400 && status_code < 500;
    }

    /// Check if the response indicates a server error (5xx status code).
    [[nodiscard]] constexpr bool is_server_error() const noexcept {
        return status_code >= 500 && status_code < 600;
    }

    /// Check if the response indicates a redirect (3xx status code).
    ///
    /// Exchange REST APIs occasionally return redirects (e.g., Binance EU
    /// migration). Callers can check this to decide whether to follow.
    [[nodiscard]] constexpr bool is_redirect() const noexcept {
        return status_code >= 300 && status_code < 400;
    }

    /// Check if the response indicates any error (4xx or 5xx).
    [[nodiscard]] constexpr bool is_error() const noexcept {
        return status_code >= 400 && status_code < 600;
    }

    /// @brief JSON-formatted summary for monitoring system integration.
    ///
    /// Body is truncated to prevent massive JSON blobs. Use .body directly
    /// for the full response content.
    [[nodiscard]] std::string to_json() const {
        // Truncate body at 256 chars to keep JSON manageable
        std::string_view body_preview = body;
        bool truncated = false;
        if (body_preview.size() > 256) {
            body_preview = body_preview.substr(0, 256);
            truncated = true;
        }
        // Escape body_preview per RFC 8259 to prevent malformed JSON
        // from response bodies containing quotes, backslashes, or control chars.
        auto escaped = eph::core::detail::json_escape(body_preview);
        return std::format(
            "{{\"status_code\":{},\"body_size\":{},\"is_success\":{}"
            ",\"body_preview\":\"{}{}\"}}",
            status_code, body.size(),
            is_success() ? "true" : "false",
            escaped, truncated ? "..." : "");
    }

    /// @brief Look up a response header by name (case-insensitive).
    /// @param name  Header name to search for.
    /// @return Header value (trimmed), or empty string if not found.
    [[nodiscard]] std::string header(std::string_view name) const noexcept {
        return find_header(headers_raw, name);
    }

    /// @brief Check if a response header exists (case-insensitive).
    /// @param name  Header name to check for.
    /// @return true if the header is present (even with empty value).
    [[nodiscard]] bool has_header(std::string_view name) const noexcept {
        return find_header_opt(headers_raw, name).has_value();
    }

    /// Defaulted equality -- all fields must match exactly.
    [[nodiscard]] friend bool operator==(const HttpResponse&,
                                         const HttpResponse&) = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger (shared with http_client.hpp — same logger name for unified output)
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @brief Lazily-initialized logger for the HTTP message subsystem.
/// @return Pointer to the "net.http_client" spdlog logger.
///
/// Uses the same logger name as HttpClient so all HTTP-related log output
/// appears under a single channel.
inline spdlog::logger* http_client_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.http_client");
        if (!lg) lg = spdlog::stdout_color_mt("net.http_client");
        return lg;
    }();
    return l.get();
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// HTTP request builder (pure, testable)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Build an HTTP/1.1 request string.
///
/// @param method          HTTP method (e.g. "GET", "POST")
/// @param host            Host header value
/// @param path            Request URI path (e.g. "/api/v1/order")
/// @param body            Request body (empty for GET)
/// @param content_type    Content-Type header value (only added if body is non-empty)
/// @param extra_headers   Additional headers, each terminated with \r\n
/// @return Complete HTTP/1.1 request string, or error if params are invalid
[[nodiscard]] inline std::expected<std::string, std::string>
build_http_request(std::string_view method,
                   std::string_view host,
                   std::string_view path,
                   std::string_view body = {},
                   std::string_view content_type = {},
                   std::string_view extra_headers = {}) noexcept {
    if (method.empty()) {
        SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
            "build_http_request: method is empty");
        return std::unexpected(std::string("HTTP method must not be empty"));
    }
    if (host.empty()) {
        SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
            "build_http_request: host is empty");
        return std::unexpected(std::string("Host must not be empty"));
    }
    if (path.empty()) {
        SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
            "build_http_request: path is empty");
        return std::unexpected(std::string("Path must not be empty"));
    }

    // Reject CR/LF in method/host/path. Per the same rationale that
    // extra_headers are scrubbed below: a caller passing untrusted bytes
    // through any of these fields could otherwise inject arbitrary
    // request lines or headers.
    auto has_crlf = [](std::string_view s) noexcept {
        return s.find('\r') != std::string_view::npos
            || s.find('\n') != std::string_view::npos;
    };
    if (has_crlf(method) || has_crlf(host) || has_crlf(path)) {
        SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
            "build_http_request: method/host/path contains CR/LF "
            "(potential header injection)");
        return std::unexpected(std::string(
            "method/host/path must not contain CR or LF (header injection risk)"));
    }

    std::string req;
    // Pre-allocate a reasonable size to avoid reallocations
    req.reserve(256 + body.size() + extra_headers.size());

    // Request line
    req.append(method);
    req.append(" ");
    req.append(path);
    req.append(" HTTP/1.1\r\n");

    // Mandatory headers
    req.append("Host: ");
    req.append(host);
    req.append("\r\n");

    // Close connection after response (no keep-alive)
    req.append("Connection: close\r\n");

    // Body-related headers
    if (!body.empty()) {
        if (!content_type.empty()) {
            req.append("Content-Type: ");
            req.append(content_type);
            req.append("\r\n");
        }
        req.append(std::format("Content-Length: {}\r\n", body.size()));
    }

    // User-supplied extra headers (must already be \r\n terminated per header).
    // Reject if it contains a blank line (\r\n\r\n) which would terminate
    // headers early — this prevents HTTP request smuggling via header injection.
    if (!extra_headers.empty()) {
        if (extra_headers.find("\r\n\r\n") != std::string_view::npos) {
            SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
                "build_http_request: extra_headers contains blank line (potential injection)");
            return std::unexpected(std::string(
                "extra_headers must not contain \\r\\n\\r\\n (header injection risk)"));
        }
        // Reject bare newlines (\n without preceding \r) — some proxies treat
        // lone \n as a line terminator, enabling header injection.
        for (size_t i = 0; i < extra_headers.size(); ++i) {
            if (extra_headers[i] == '\n' && (i == 0 || extra_headers[i - 1] != '\r')) {
                SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
                    "build_http_request: extra_headers contains bare LF (potential injection)");
                return std::unexpected(std::string(
                    "extra_headers must not contain bare \\n (header injection risk)"));
            }
        }
        // Reject user-supplied Content-Length and Transfer-Encoding in
        // extra_headers.  Both are managed by build_http_request itself
        // (Content-Length is auto-generated from body.size() above; this
        // function does NOT support chunked request bodies).  Allowing
        // either via extra_headers creates a CL/CL or CL/TE desync
        // vector on the request side, mirroring the smuggling attacks
        // we defend against on the response side via
        // is_http_response_complete.
        //
        // The check is case-insensitive (RFC 7230 §3.2 — header names
        // are case-insensitive) and matches at the start of any line
        // inside extra_headers.
        auto starts_with_ci = [](std::string_view haystack, size_t pos,
                                  std::string_view needle) {
            if (pos + needle.size() > haystack.size()) return false;
            for (size_t i = 0; i < needle.size(); ++i) {
                char a = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[pos + i])));
                char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[i])));
                if (a != b) return false;
            }
            return true;
        };
        size_t scan = 0;
        while (scan < extra_headers.size()) {
            // We are at the start of a header line.
            if (starts_with_ci(extra_headers, scan, "content-length:") ||
                starts_with_ci(extra_headers, scan, "transfer-encoding:")) {
                SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
                    "build_http_request: extra_headers contains "
                    "Content-Length or Transfer-Encoding (managed by "
                    "build_http_request itself — supplying them via "
                    "extra_headers creates a CL/TE desync risk)");
                return std::unexpected(std::string(
                    "extra_headers must not contain Content-Length or "
                    "Transfer-Encoding (managed internally — desync risk)"));
            }
            auto eol = extra_headers.find("\r\n", scan);
            if (eol == std::string_view::npos) break;
            scan = eol + 2;
        }
        req.append(extra_headers);
    }

    // End of headers
    req.append("\r\n");

    // Body
    if (!body.empty()) {
        req.append(body);
    }

    SPDLOG_LOGGER_DEBUG(detail::http_client_logger(),
        "Built HTTP request: {} {} ({} bytes total)",
        method, path, req.size());

    return req;
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP response parser (pure, testable)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Parse an HTTP/1.1 response from raw bytes.
///
/// Supports Content-Length and connection-close semantics.
/// Does NOT support chunked transfer encoding.
///
/// @param data  Complete raw HTTP response (headers + body)
/// @return Parsed response, or error string
[[nodiscard]] inline std::expected<HttpResponse, std::string>
parse_http_response(std::string_view data) noexcept {
    [[maybe_unused]] auto* log = detail::http_client_logger();

    // Find end of headers
    auto header_end = data.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        SPDLOG_LOGGER_WARN(log,
            "parse_http_response: no header terminator found in {} bytes",
            data.size());
        return std::unexpected(
            std::string("Incomplete HTTP response: no \\r\\n\\r\\n found"));
    }

    size_t body_start = header_end + 4;

    // Parse status line: "HTTP/1.1 200 OK\r\n"
    auto first_line_end = data.find("\r\n");
    if (first_line_end == std::string_view::npos) {
        return std::unexpected(std::string("Malformed HTTP response: no status line"));
    }

    auto status_line = data.substr(0, first_line_end);

    // Find status code (after first space)
    auto space1 = status_line.find(' ');
    if (space1 == std::string_view::npos) {
        SPDLOG_LOGGER_WARN(log,
            "parse_http_response: malformed status line: '{}'",
            std::string(status_line));
        return std::unexpected(std::format(
            "Malformed HTTP status line: '{}'", status_line));
    }

    auto code_start = space1 + 1;
    auto space2 = status_line.find(' ', code_start);
    auto code_str = status_line.substr(code_start,
        (space2 != std::string_view::npos)
            ? space2 - code_start
            : std::string_view::npos);

    HttpResponse resp;
    auto [ptr, ec] = std::from_chars(
        code_str.data(), code_str.data() + code_str.size(),
        resp.status_code);
    if (ec != std::errc{}) {
        SPDLOG_LOGGER_WARN(log,
            "parse_http_response: invalid status code: '{}'",
            std::string(code_str));
        return std::unexpected(std::format(
            "Invalid HTTP status code: '{}'", code_str));
    }

    // Extract raw headers (between status line and the blank line)
    resp.headers_raw = std::string(
        data.substr(first_line_end + 2, header_end - (first_line_end + 2)));

    // Body is everything after \r\n\r\n
    resp.body = std::string(data.substr(body_start));

    SPDLOG_LOGGER_DEBUG(log,
        "Parsed HTTP response: status={}, headers_len={}, body_len={}",
        resp.status_code, resp.headers_raw.size(), resp.body.size());

    return resp;
}

// ─────────────────────────────────────────────────────────────────────────────
// Header extraction
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Extract a header value by name, distinguishing "not found" from "empty value".
///
/// Unlike find_header() which returns empty string for both missing and empty-value
/// headers, this variant returns std::nullopt when the header is absent, allowing
/// callers to distinguish the two cases.
///
/// This is the canonical implementation; find_header() delegates to this.
///
/// @param headers_raw  Raw header block (lines separated by \r\n)
/// @param name         Header name to search for (case-insensitive)
/// @return Header value (trimmed), or std::nullopt if header is not present
[[nodiscard]] inline std::optional<std::string>
find_header_opt(std::string_view headers_raw, std::string_view name) noexcept {
    auto to_lower = [](unsigned char c) -> unsigned char {
        return static_cast<unsigned char>(std::tolower(c));
    };

    size_t pos = 0;
    while (pos < headers_raw.size()) {
        auto line_end = headers_raw.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            line_end = headers_raw.size();
        }

        auto line = headers_raw.substr(pos, line_end - pos);
        pos = (line_end == headers_raw.size()) ? line_end : line_end + 2;

        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;

        auto hdr_name = line.substr(0, colon);
        if (!std::ranges::equal(hdr_name, name, {}, to_lower, to_lower))
            continue;

        // Trim OWS (optional whitespace) from value per RFC 7230 section 3.2.6
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.remove_suffix(1);

        return std::string(value);
    }
    return std::nullopt;
}

/// @brief Extract a header value by name (case-insensitive) from raw headers.
///
/// Delegates to find_header_opt() and converts std::nullopt to empty string.
/// Use find_header_opt() when you need to distinguish "header absent" from
/// "header present with empty value".
///
/// @param headers_raw  Raw header block (lines separated by \r\n)
/// @param name         Header name to search for (case-insensitive)
/// @return Header value (trimmed), or empty string if not found
[[nodiscard]] inline std::string
find_header(std::string_view headers_raw, std::string_view name) noexcept {
    return find_header_opt(headers_raw, name).value_or(std::string{});
}

// ─────────────────────────────────────────────────────────────────────────────
// Response completeness detection
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Check if a raw HTTP response buffer contains a complete response.
///
/// Handles Content-Length, chunked Transfer-Encoding, and connection-close semantics.
///
/// @param buf  Raw response data received so far.
/// @return true if complete, false if more data needed.
[[nodiscard]] inline bool is_http_response_complete(std::string_view buf) noexcept {
    auto header_end = buf.find("\r\n\r\n");
    if (header_end == std::string_view::npos) return false;

    size_t body_start = header_end + 4;
    size_t body_len = buf.size() - body_start;

    // Extract raw headers (between status line end and blank line)
    auto headers_raw = buf.substr(0, header_end);

    // RFC 7230 §3.3.3: if BOTH Content-Length and Transfer-Encoding
    // are present, the Transfer-Encoding overrides Content-Length and
    // such a message MAY indicate request smuggling.  We honor RFC
    // semantics: Transfer-Encoding wins, Content-Length is ignored.
    // (A stricter interpretation would reject the response entirely;
    // we choose the lenient form so legitimate servers that
    // accidentally include both still parse.)
    //
    // RFC 7230 §3.3.1: transfer codings are case-insensitive ("chunked"
    // and "CHUNKED" are equivalent).  We also walk ALL header lines
    // (RFC §3.2.2: multiple headers with the same name combine as if
    // comma-separated) so a response that splits Transfer-Encoding
    // across two lines like "Transfer-Encoding: gzip\r\nTransfer-
    // Encoding: chunked" is still detected.
    // RFC 7230 §3.3.2: multiple Content-Length headers (or a single
    // header with comma-separated values) MUST be rejected unless all
    // values are identical.  Silently picking the first opens
    // smuggling attacks where a downstream proxy uses a different
    // value than the client.  Walk all headers and confirm uniqueness.
    std::string cl_value;
    {
        size_t scan = 0;
        bool first_cl = true;
        bool conflict = false;
        while (scan < headers_raw.size()) {
            auto line_end = headers_raw.find("\r\n", scan);
            if (line_end == std::string_view::npos) line_end = headers_raw.size();
            auto line = headers_raw.substr(scan, line_end - scan);
            scan = (line_end == headers_raw.size()) ? line_end : line_end + 2;

            auto colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            auto hdr_name = line.substr(0, colon);
            constexpr std::string_view kClName = "Content-Length";
            if (hdr_name.size() != kClName.size()) continue;
            bool name_match = true;
            for (size_t i = 0; i < kClName.size(); ++i) {
                char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hdr_name[i])));
                char b = static_cast<char>(std::tolower(static_cast<unsigned char>(kClName[i])));
                if (a != b) { name_match = false; break; }
            }
            if (!name_match) continue;

            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.remove_suffix(1);
            // Comma-separated list (e.g. "42, 42") inside ONE header.
            // Per RFC 7230 §3.3.2 these are equivalent to multiple
            // headers and must be checked for identity.
            std::string normalized;
            normalized.reserve(value.size());
            for (size_t i = 0; i < value.size(); ++i) {
                if (value[i] == ' ' || value[i] == '\t') continue;
                normalized.push_back(value[i]);
            }
            // Walk comma-separated values inside this single header.
            size_t prev = 0;
            while (prev < normalized.size()) {
                auto comma = normalized.find(',', prev);
                std::string one = normalized.substr(prev,
                    comma == std::string::npos ? std::string::npos : comma - prev);
                if (first_cl) {
                    cl_value = one;
                    first_cl = false;
                } else if (one != cl_value) {
                    conflict = true;
                    break;
                }
                if (comma == std::string::npos) break;
                prev = comma + 1;
            }
            if (conflict) break;
        }
        if (conflict) {
            SPDLOG_LOGGER_WARN(detail::http_client_logger(),
                "Multiple Content-Length headers with conflicting values "
                "(RFC 7230 §3.3.2 — possible request smuggling)");
            return false;
        }
    }
    bool te_chunked = false;
    {
        // Walk all Transfer-Encoding headers, case-insensitive on
        // both name and value, looking for the "chunked" coding token.
        size_t scan = 0;
        while (scan < headers_raw.size()) {
            auto line_end = headers_raw.find("\r\n", scan);
            if (line_end == std::string_view::npos) line_end = headers_raw.size();
            auto line = headers_raw.substr(scan, line_end - scan);
            scan = (line_end == headers_raw.size()) ? line_end : line_end + 2;

            auto colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            auto hdr_name = line.substr(0, colon);
            // Case-insensitive name match against "Transfer-Encoding".
            constexpr std::string_view kTeName = "Transfer-Encoding";
            if (hdr_name.size() != kTeName.size()) continue;
            bool name_match = true;
            for (size_t i = 0; i < kTeName.size(); ++i) {
                char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hdr_name[i])));
                char b = static_cast<char>(std::tolower(static_cast<unsigned char>(kTeName[i])));
                if (a != b) { name_match = false; break; }
            }
            if (!name_match) continue;

            // Case-insensitive substring search for "chunked" in value.
            auto value = line.substr(colon + 1);
            for (size_t i = 0; i + 7 <= value.size(); ++i) {
                bool match = true;
                static constexpr char k[] = "chunked";
                for (size_t j = 0; j < 7; ++j) {
                    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i + j])));
                    if (c != k[j]) { match = false; break; }
                }
                if (match) { te_chunked = true; break; }
            }
            if (te_chunked) break;
        }
    }
    if (te_chunked) {
        // Force chunked path even when Content-Length is also set.
        cl_value = std::string{};
    }
    if (cl_value.empty()) {
        // No Content-Length — check for chunked transfer encoding.
        if (te_chunked) {
            // RFC 7230 §4.1: properly walk the chunk stream rather
            // than substring-matching for "0\r\n\r\n".  A naïve
            // substring search produces false positives whenever
            // chunk DATA happens to contain those bytes — and a
            // matching "\r\n0\r\n\r\n" can ALSO occur inside data
            // (a chunk-size line ending in "\r\n" followed by chunk
            // data starting with "0\r\n\r\n" looks identical to a
            // legitimate boundary).  The only safe path is parsing
            // chunk sizes.
            size_t pos = body_start;
            while (pos < buf.size()) {
                auto crlf = buf.find("\r\n", pos);
                if (crlf == std::string_view::npos) return false;
                // chunk-size [ ; chunk-extensions ]
                auto size_str = buf.substr(pos, crlf - pos);
                auto semi = size_str.find(';');
                if (semi != std::string_view::npos) {
                    size_str = size_str.substr(0, semi);
                }
                // Trim trailing whitespace per RFC 7230 §4.1.1
                while (!size_str.empty() &&
                       (size_str.back() == ' ' || size_str.back() == '\t')) {
                    size_str.remove_suffix(1);
                }
                if (size_str.empty()) return false;
                size_t chunk_size = 0;
                auto [pp, ec] = std::from_chars(
                    size_str.data(),
                    size_str.data() + size_str.size(),
                    chunk_size, /*base=*/16);
                if (ec != std::errc{} || pp != size_str.data() + size_str.size()) {
                    return false;
                }
                pos = crlf + 2; // past size-line CRLF
                if (chunk_size == 0) {
                    // Last chunk: walk optional trailer headers until
                    // we hit an empty line (the final CRLF terminator).
                    while (pos < buf.size()) {
                        auto tend = buf.find("\r\n", pos);
                        if (tend == std::string_view::npos) return false;
                        if (tend == pos) return true; // empty line → done
                        pos = tend + 2;
                    }
                    return false;
                }
                // Skip chunk data + its trailing CRLF.
                if (pos + chunk_size + 2 > buf.size()) return false;
                pos += chunk_size + 2;
            }
            return false;
        }
        // Neither Content-Length nor chunked — rely on connection close.
        // Cap total buffer to prevent unbounded growth.
        constexpr size_t kMaxNoClBuffer = 16 * 1024 * 1024; // 16 MiB
        if (buf.size() > kMaxNoClBuffer) {
            SPDLOG_LOGGER_WARN(detail::http_client_logger(),
                "Response without Content-Length exceeds {} bytes, "
                "treating as complete", kMaxNoClBuffer);
            return true;
        }
        return false;
    }

    size_t content_length = 0;
    auto [ptr, ec] = std::from_chars(
        cl_value.data(),
        cl_value.data() + cl_value.size(),
        content_length);
    // Require from_chars to consume the ENTIRE value — partial
    // consumption (e.g. "0x05" parses as 0 with ptr at 'x') means
    // the value contains non-digit garbage and a downstream proxy
    // may interpret it differently.  Reject as desync defense.
    if (ec != std::errc{} ||
        ptr != cl_value.data() + cl_value.size()) {
        return false;
    }

    // Reject absurdly large Content-Length to prevent OOM (256 MiB limit for REST responses)
    constexpr size_t kMaxContentLength = 256 * 1024 * 1024;
    if (content_length > kMaxContentLength) {
        SPDLOG_LOGGER_WARN(detail::http_client_logger(),
            "Content-Length {} exceeds maximum allowed {} bytes",
            content_length, kMaxContentLength);
        return false;
    }

    return body_len >= content_length;
}

} // namespace eph::net

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for HttpResponse
// ─────────────────────────────────────────────────────────────────────────────

/// @brief std::formatter for HttpResponse -- compact one-line summary.
///
/// Shows status code, body size, and a truncated body preview.
/// Full body is omitted to keep log output manageable.
template <>
struct std::formatter<eph::net::HttpResponse> : std::formatter<std::string> {
    auto format(const eph::net::HttpResponse& r, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("HttpResponse(status={}, body={}B)",
                r.status_code, r.body.size()),
            ctx);
    }
};
