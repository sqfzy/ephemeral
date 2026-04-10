#pragma once

/// @file http.hpp
/// @brief HFT-pragmatic HTTP/1.x parser subset — incremental, zero-heap.
///
/// ## Why this exists
///
/// The v3.3 refactor dropped the legacy `http_message.hpp` (eager one-shot
/// parser built on `std::string`) in favor of a Tokio-aligned incremental
/// parser that:
///
///   1. **Returns `expected<optional<ParseResult<T>>>`** — "Partial" is a
///      first-class outcome, matching `httparse::Status::Partial` and the
///      v3.3 `StreamCodec::decode` contract.
///   2. **Allocates nothing** — parsed headers land in a caller-provided
///      `span<HttpHeader>`; the returned view types are
///      `string_view` / `span<const uint8_t>` into the caller's receive
///      buffer. The hot path never touches the heap.
///   3. **Is a deliberate subset** — per plan §D-1, HFT exchange REST APIs
///      (Binance / OKX / Coinbase / Kraken / Bybit) use Content-Length
///      exclusively. Chunked transfer encoding, cookies, redirect follow,
///      Expect: 100-continue, and multipart are all explicitly rejected.
///      We choose "fail loudly with a typed error" over "silently pretend
///      to parse ambiguous framing" — the latter is the root cause of
///      HTTP request-smuggling CVEs.
///
/// ## Supported
///
///   * Request methods: arbitrary token (GET, POST, CONNECT, PUT, DELETE, …)
///   * HTTP/1.0 and HTTP/1.1
///   * Content-Length body framing
///   * Up to `kMaxHeaderCount` headers (capped even if caller storage is larger)
///
/// ## Explicitly rejected (typed `Error::CodecBad`)
///
///   * Any `Transfer-Encoding` header (chunked included — see §D-1)
///   * `Content-Length + Transfer-Encoding` combination (request smuggling)
///   * Multiple `Content-Length` headers with conflicting values
///   * Non-3-digit status codes (RFC 7230 §3.1.2 — regression of 5137fee)
///   * Non-numeric `Content-Length` value (regression of 282f0e2)
///   * Bare LF (unescorted `\n`) in header lines
///   * CRLF / NUL / bare LF / whitespace injection in the builder inputs
///
/// ## Lifetime
///
/// All `string_view` and `span` fields returned by `parse_http_*` point
/// **into the caller's buffer**. They are invalidated the moment the caller
/// mutates, slides, or frees that buffer. Treat them like iterators into a
/// container you still own.
///
/// `header_storage` similarly must outlive the returned `HttpRequest` /
/// `HttpResponse` — the `headers` field is a `span` into it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/core/error.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// Public types
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A single HTTP header (name + value) as views into a caller buffer.
///
/// Both fields are views — there is no owning storage. When produced by
/// `parse_http_*`, they point into the receive buffer; when passed to
/// `build_http_*`, they are read verbatim and serialized.
struct HttpHeader {
    std::string_view name;
    std::string_view value;
};

/// @brief Parsed HTTP/1.x request.
///
/// All sub-object views (method/target/headers/body) alias the caller's
/// receive buffer. The `headers` span aliases caller-provided
/// `header_storage`. Using any of these after either buffer is mutated,
/// relocated, or freed is undefined behavior.
struct HttpRequest {
    std::string_view              method;         ///< e.g. "GET", "POST", "CONNECT"
    std::string_view              target;         ///< request-target, e.g. "/api/v3/order?x=y"
    uint8_t                       version_minor;  ///< 0 = HTTP/1.0, 1 = HTTP/1.1
    std::span<const HttpHeader>   headers;        ///< slice of header_storage
    std::span<const uint8_t>      body;           ///< empty or Content-Length sized
};

/// @brief Parsed HTTP/1.x response. Same lifetime rules as HttpRequest.
struct HttpResponse {
    uint16_t                      status_code;    ///< always 3-digit (100..599)
    std::string_view              reason_phrase;  ///< may be empty
    uint8_t                       version_minor;  ///< 0 = HTTP/1.0, 1 = HTTP/1.1
    std::span<const HttpHeader>   headers;        ///< slice of header_storage
    std::span<const uint8_t>      body;           ///< empty or Content-Length sized
};

/// @brief Result of a successful incremental parse — the parsed value plus
///        the number of bytes the caller should drain from the front of its
///        receive buffer.
template <class T>
struct ParseResult {
    T      value;
    size_t consumed;
};

// ─────────────────────────────────────────────────────────────────────────────
// Hard limits (enforced regardless of caller-provided storage size)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Maximum number of headers accepted by the parser.
///
/// This is a hard cap on top of whatever `header_storage.size()` the caller
/// provides. 64 is well above what any real exchange REST response uses
/// (Binance ~12, OKX ~14) while staying small enough that DoS attempts with
/// 10 000 bogus headers get rejected at a predictable cost.
inline constexpr size_t kMaxHeaderCount       = 64;
inline constexpr size_t kMaxStartLineLength   = 8192;
inline constexpr size_t kMaxHeaderLineLength  = 8192;
inline constexpr size_t kMaxBodySize          = 16 * 1024 * 1024; // 16 MiB

// ─────────────────────────────────────────────────────────────────────────────
// Internal parsing helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @brief Tag unused to key a named logger shared by the parser & builder.
[[nodiscard]] inline spdlog::logger* http_logger() noexcept {
    // Defer to default logger — we do not want to own a named sink here;
    // callers of the parser almost always run inside a module that has
    // already configured spdlog.
    return spdlog::default_logger_raw();
}

/// @brief ASCII lower-case (no locale dependence).
[[nodiscard]] constexpr uint8_t ascii_lower(uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + 32) : c;
}

/// @brief Case-insensitive ASCII equality.
[[nodiscard]] constexpr bool iequal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(static_cast<uint8_t>(a[i])) !=
            ascii_lower(static_cast<uint8_t>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// @brief RFC 7230 §3.2.6 token characters (method name, header field-name).
///
/// `tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
///         "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA`
[[nodiscard]] constexpr bool is_tchar(uint8_t c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

/// @brief RFC 7230 §3.2.3 OWS — optional whitespace (SP / HTAB only).
[[nodiscard]] constexpr bool is_ows(uint8_t c) noexcept {
    return c == ' ' || c == '\t';
}

/// @brief Target chars visible to the parser: anything except CTL / SP.
///
/// We only reject the ASCII control set and space; callers are expected
/// to have validated percent-encoding separately (we parse, we don't sanitize).
[[nodiscard]] constexpr bool is_target_char(uint8_t c) noexcept {
    return c > 0x20 && c < 0x7f;
}

/// @brief Find a CRLF starting at `pos`. Returns index of the `\r`, or
///        `end` if none before `end`.
[[nodiscard]] inline size_t find_crlf(std::span<const uint8_t> buf,
                                      size_t pos,
                                      size_t end) noexcept {
    for (size_t i = pos; i + 1 < end; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') return i;
        // Bare LF detection is left to the caller — we return "not found"
        // here and let the caller's line-length check bound the search.
    }
    return end;
}

/// @brief Parse a non-negative decimal integer into `out`. Rejects leading
///        sign, leading whitespace, empty input, and any non-digit char.
///
/// Returns true on full consumption, false otherwise.
[[nodiscard]] inline bool parse_decimal(std::string_view s, size_t& out) noexcept {
    if (s.empty()) return false;
    size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        // Overflow guard: 2^64 / 10 ~ 1.8e18; clamp at kMaxBodySize
        // equivalent safe multiply.
        if (v > (size_t{1} << 60)) return false;
        v = v * 10 + static_cast<size_t>(c - '0');
    }
    out = v;
    return true;
}

/// @brief Trim OWS from both ends of a view (RFC 7230 §3.2.4 field-value OWS).
[[nodiscard]] inline std::string_view trim_ows(std::string_view v) noexcept {
    while (!v.empty() && is_ows(static_cast<uint8_t>(v.front()))) v.remove_prefix(1);
    while (!v.empty() && is_ows(static_cast<uint8_t>(v.back())))  v.remove_suffix(1);
    return v;
}

/// @brief Trim trailing OWS from a header field-name (RFC 7230 §3.2.4).
///
/// Required because "Content-Length : 5" must match "Content-Length: 5" for
/// the smuggling defense to hold — a downstream proxy that strips trailing
/// SP before the colon would see the value; if we don't, we disagree with
/// the proxy on framing. See commit e108fcb.
[[nodiscard]] inline std::string_view trim_field_name(std::string_view name) noexcept {
    while (!name.empty() && is_ows(static_cast<uint8_t>(name.back()))) {
        name.remove_suffix(1);
    }
    return name;
}

/// @brief Validate that a parsed header field-name is a valid RFC 7230 token.
[[nodiscard]] inline bool is_valid_token(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) {
        if (!is_tchar(static_cast<uint8_t>(c))) return false;
    }
    return true;
}

/// @brief Validate a header field-value — reject CTL chars except HTAB.
///        CRLF was already consumed as the line terminator, so a `\r` or
///        `\n` inside the value is by definition an injection attempt.
[[nodiscard]] inline bool is_valid_field_value(std::string_view s) noexcept {
    for (char c : s) {
        auto u = static_cast<uint8_t>(c);
        // Allow: printable ASCII + HTAB; reject CR/LF/NUL/other CTLs.
        if (u == '\t') continue;
        if (u < 0x20 || u == 0x7f) return false;
    }
    return true;
}

/// @brief Shared header-block parser used by request & response paths.
///
/// On success writes up to `header_storage.size()` HttpHeader records and
/// returns the count via `out_count` + the position *past* the terminating
/// blank-line CRLF via `out_header_end`. Returns:
///
///   * `Ok(true)`  — header block fully parsed, `out_*` set
///   * `Ok(false)` — need more bytes
///   * `Err(_)`    — protocol violation
///
/// Performs CL/TE conflict detection, Transfer-Encoding rejection, and
/// Content-Length numeric validation inline — these three checks are the
/// smuggling-defense core and must run regardless of whether the caller
/// later inspects the headers.
///
/// `out_content_length` is set to the parsed Content-Length value, or 0
/// if no Content-Length header is present. `out_has_content_length` tells
/// the caller which of those two states it's in.
struct HeaderParseState {
    size_t count             = 0;       ///< headers written to storage
    size_t header_block_end  = 0;       ///< position past the final CRLF CRLF
    size_t content_length    = 0;       ///< valid only if has_content_length
    bool   has_content_length = false;
};

[[nodiscard]] inline std::expected<std::optional<HeaderParseState>, core::ErrorInfo>
parse_header_block(std::span<const uint8_t> buf,
                   size_t                   start,
                   std::span<HttpHeader>    header_storage) noexcept {
    HeaderParseState st;
    size_t pos = start;
    const size_t end = buf.size();

    // Cap headers at min(caller storage, kMaxHeaderCount).
    const size_t cap = std::min(header_storage.size(), kMaxHeaderCount);

    bool seen_transfer_encoding = false;
    size_t cl_parsed_count = 0;
    size_t first_cl_value  = 0;

    while (true) {
        if (pos >= end) return std::optional<HeaderParseState>{}; // need more

        // End of header block?  A bare CRLF on its own line.
        if (buf[pos] == '\r') {
            if (pos + 1 >= end) return std::optional<HeaderParseState>{}; // need more
            if (buf[pos + 1] != '\n') {
                SPDLOG_WARN("http: header block: bare CR (offset {})", pos);
                return std::unexpected(core::ErrorInfo{
                    core::Error::CodecBad, "bare CR in header block"});
            }
            pos += 2;
            break; // end of headers
        }
        // Bare LF at line start is an injection attempt (some proxies accept
        // lone LF as a line terminator; we do not).
        if (buf[pos] == '\n') {
            SPDLOG_WARN("http: header block: bare LF (offset {})", pos);
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad, "bare LF at header start"});
        }

        // Find the end of this header line (CRLF).
        const size_t line_start = pos;
        size_t scan_end = std::min(end, line_start + kMaxHeaderLineLength + 2);
        size_t crlf = find_crlf(buf, line_start, scan_end);
        if (crlf == scan_end) {
            if (scan_end < end || (end - line_start) > kMaxHeaderLineLength) {
                // Either we ran out the kMaxHeaderLineLength window — abuse —
                // or the bound was the actual buffer end but the line is
                // already over the cap. Either way: reject.
                if ((end - line_start) > kMaxHeaderLineLength) {
                    SPDLOG_WARN("http: header line too long ({} > {})",
                                end - line_start, kMaxHeaderLineLength);
                    return std::unexpected(core::ErrorInfo{
                        core::Error::CodecOverflow, "header line exceeds max length"});
                }
            }
            // Not enough data yet.
            return std::optional<HeaderParseState>{};
        }

        // Scan for bare LF inside the line (would be an injection).
        for (size_t i = line_start; i < crlf; ++i) {
            if (buf[i] == '\n') {
                SPDLOG_WARN("http: header line: bare LF (offset {})", i);
                return std::unexpected(core::ErrorInfo{
                    core::Error::CodecBad, "bare LF inside header line"});
            }
        }

        std::string_view line(
            reinterpret_cast<const char*>(buf.data() + line_start),
            crlf - line_start);
        pos = crlf + 2; // advance past CRLF

        // Split on the first ':'.
        auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            SPDLOG_WARN("http: header line missing ':' (len={})", line.size());
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad, "header line missing ':'"});
        }
        auto raw_name = line.substr(0, colon);
        auto name     = trim_field_name(raw_name);
        if (!is_valid_token(name)) {
            SPDLOG_WARN("http: invalid header field-name");
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad, "invalid header field-name"});
        }
        auto value = trim_ows(line.substr(colon + 1));
        if (!is_valid_field_value(value)) {
            SPDLOG_WARN("http: invalid header field-value (CTL char)");
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad, "invalid header field-value"});
        }

        // ── Smuggling defenses ──────────────────────────────────────────
        // 1. Any Transfer-Encoding header → rejected outright (D-1).
        if (iequal(name, "Transfer-Encoding")) {
            SPDLOG_WARN("http: Transfer-Encoding header present — rejecting (D-1)");
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad,
                "Transfer-Encoding not supported (see D-1)"});
        }
        // 2. Content-Length: strict digit-only, all values must agree.
        if (iequal(name, "Content-Length")) {
            size_t cl = 0;
            if (!parse_decimal(value, cl)) {
                SPDLOG_WARN("http: Content-Length non-numeric ('{}')",
                            std::string(value));
                return std::unexpected(core::ErrorInfo{
                    core::Error::CodecBad, "Content-Length not numeric"});
            }
            if (cl > kMaxBodySize) {
                SPDLOG_WARN("http: Content-Length {} exceeds max {}",
                            cl, kMaxBodySize);
                return std::unexpected(core::ErrorInfo{
                    core::Error::CodecOverflow, "Content-Length exceeds max body size"});
            }
            if (cl_parsed_count == 0) {
                first_cl_value = cl;
            } else if (cl != first_cl_value) {
                SPDLOG_WARN("http: conflicting Content-Length headers ({} vs {})",
                            first_cl_value, cl);
                return std::unexpected(core::ErrorInfo{
                    core::Error::CodecBad, "conflicting Content-Length values"});
            }
            ++cl_parsed_count;
        }

        // Store header (bounded).
        if (st.count >= cap) {
            SPDLOG_WARN("http: header count exceeds storage/limit ({})", cap);
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecOverflow, "too many headers"});
        }
        header_storage[st.count++] = HttpHeader{name, value};
    }

    (void)seen_transfer_encoding; // reserved (future: allow explicit whitelist)
    if (cl_parsed_count > 0) {
        st.has_content_length = true;
        st.content_length     = first_cl_value;
    }
    st.header_block_end = pos;
    return std::optional<HeaderParseState>{st};
}

/// @brief Shared builder helper: validate that a token-ish field has no
///        CR/LF/NUL/space/tab. Used for method/target/reason/header-name.
[[nodiscard]] inline bool builder_reject_unsafe_token(std::string_view s) noexcept {
    for (char c : s) {
        auto u = static_cast<uint8_t>(c);
        if (u == '\r' || u == '\n' || u == '\0' || u == ' ' || u == '\t') {
            return true;
        }
    }
    return false;
}

/// @brief Relaxed field-value check: reject only CR/LF/NUL. OWS is permitted
///        inside values (RFC 7230 allows SP/HTAB, just not CR/LF).
[[nodiscard]] inline bool builder_reject_unsafe_value(std::string_view s) noexcept {
    for (char c : s) {
        auto u = static_cast<uint8_t>(c);
        if (u == '\r' || u == '\n' || u == '\0') return true;
    }
    return false;
}

/// @brief Append raw bytes to `buf`, bounds-checked against `cap`.
[[nodiscard]] inline bool buf_append(uint8_t* buf, size_t cap, size_t& off,
                                     const void* src, size_t n) noexcept {
    if (off > cap || cap - off < n) return false;
    std::memcpy(buf + off, src, n);
    off += n;
    return true;
}

[[nodiscard]] inline bool buf_append_sv(uint8_t* buf, size_t cap, size_t& off,
                                        std::string_view s) noexcept {
    return buf_append(buf, cap, off, s.data(), s.size());
}

[[nodiscard]] inline bool buf_append_cstr(uint8_t* buf, size_t cap, size_t& off,
                                          const char* s) noexcept {
    return buf_append(buf, cap, off, s, std::strlen(s));
}

/// @brief Append a non-negative decimal integer (no padding).
[[nodiscard]] inline bool buf_append_decimal(uint8_t* buf, size_t cap, size_t& off,
                                             size_t v) noexcept {
    // 20 digits is enough for 2^64; we reverse in place.
    char tmp[20];
    size_t n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < sizeof(tmp)) {
            tmp[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        // reverse
        for (size_t i = 0, j = n - 1; i < j; ++i, --j) {
            std::swap(tmp[i], tmp[j]);
        }
    }
    return buf_append(buf, cap, off, tmp, n);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// parse_http_request
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Incremental HTTP request parser.
///
/// @param buffer         raw bytes received so far
/// @param header_storage caller-owned slot for parsed headers (≤ kMaxHeaderCount)
///
/// @return
///   * `Ok(Some(ParseResult<HttpRequest>))` — complete request parsed;
///     the caller should drain `consumed` bytes and reuse the tail.
///   * `Ok(None)` — need more bytes (incomplete start line, headers, or body).
///   * `Err(ErrorInfo)` — unrecoverable protocol violation; the caller must
///     close the connection.
///
/// All string_view / span fields in the returned HttpRequest point into
/// `buffer` and `header_storage`; they are invalidated the moment either
/// underlying storage is mutated, relocated, or freed.
[[nodiscard]] inline
std::expected<std::optional<ParseResult<HttpRequest>>, core::ErrorInfo>
parse_http_request(
    std::span<const uint8_t> buffer,
    std::span<HttpHeader>    header_storage
) noexcept {
    const size_t end = buffer.size();
    if (end == 0) return std::optional<ParseResult<HttpRequest>>{};

    // ── Start line: METHOD SP TARGET SP HTTP/1.x CRLF ──────────────────
    const size_t sl_search_end = std::min(end, kMaxStartLineLength + 2);
    size_t sl_crlf = detail::find_crlf(buffer, 0, sl_search_end);
    if (sl_crlf == sl_search_end) {
        if (sl_search_end < end) {
            // Reached kMaxStartLineLength without a CRLF — oversize.
            SPDLOG_WARN("http: start line exceeds {} bytes", kMaxStartLineLength);
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecOverflow, "request start line too long"});
        }
        return std::optional<ParseResult<HttpRequest>>{}; // need more
    }
    if (sl_crlf == 0) {
        SPDLOG_WARN("http: empty start line");
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "empty start line"});
    }

    std::string_view start_line(
        reinterpret_cast<const char*>(buffer.data()), sl_crlf);

    // Split on the first two spaces.
    auto sp1 = start_line.find(' ');
    if (sp1 == std::string_view::npos) {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "request start line missing SP after method"});
    }
    auto sp2 = start_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "request start line missing SP after target"});
    }

    auto method  = start_line.substr(0, sp1);
    auto target  = start_line.substr(sp1 + 1, sp2 - sp1 - 1);
    auto version = start_line.substr(sp2 + 1);

    if (method.empty() || !detail::is_valid_token(method)) {
        SPDLOG_WARN("http: invalid method token");
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "invalid method token"});
    }
    // Target: non-empty, no CTL, no bare whitespace. We already split on
    // spaces so any embedded space would appear as extra tokens.
    if (target.empty()) {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "empty request-target"});
    }
    for (char c : target) {
        if (!detail::is_target_char(static_cast<uint8_t>(c))) {
            SPDLOG_WARN("http: CTL/SP in request-target");
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad, "CTL or SP in request-target"});
        }
    }
    // HTTP version: "HTTP/1.0" or "HTTP/1.1"
    if (version.size() != 8 || version.substr(0, 7) != "HTTP/1." ||
        (version[7] != '0' && version[7] != '1')) {
        SPDLOG_WARN("http: unsupported version '{}'", std::string(version));
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "unsupported HTTP version (need 1.0/1.1)"});
    }
    uint8_t version_minor = static_cast<uint8_t>(version[7] - '0');

    // ── Headers ───────────────────────────────────────────────────────
    auto hdr_res = detail::parse_header_block(buffer, sl_crlf + 2, header_storage);
    if (!hdr_res.has_value()) return std::unexpected(hdr_res.error());
    if (!hdr_res->has_value()) return std::optional<ParseResult<HttpRequest>>{};
    const auto& st = **hdr_res;

    // ── Body (Content-Length or empty for requests) ───────────────────
    std::span<const uint8_t> body_span;
    size_t consumed = st.header_block_end;
    if (st.has_content_length) {
        const size_t body_end = st.header_block_end + st.content_length;
        if (body_end > end) {
            // Need more bytes — return None (caller will retry).
            return std::optional<ParseResult<HttpRequest>>{};
        }
        body_span = buffer.subspan(st.header_block_end, st.content_length);
        consumed  = body_end;
    }
    // No Content-Length on a request = empty body (RFC 7230 §3.3.3 rule 6
    // for requests: absent CL and absent TE means length = 0).

    HttpRequest req{
        .method        = method,
        .target        = target,
        .version_minor = version_minor,
        .headers       = header_storage.subspan(0, st.count),
        .body          = body_span,
    };
    SPDLOG_DEBUG("http: parsed request method='{}' target='{}' hdrs={} body={}B",
                 std::string(method), std::string(target), st.count,
                 body_span.size());
    return std::optional<ParseResult<HttpRequest>>{
        ParseResult<HttpRequest>{req, consumed}};
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_http_response
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Incremental HTTP response parser. Same contract as
///        `parse_http_request`, but for the response side.
///
/// Body framing rules (§RFC 7230 §3.3.3, simplified for our subset):
///   * 1xx / 204 / 304 → body is always empty regardless of Content-Length
///   * otherwise, if Content-Length present → read that many bytes
///   * otherwise → `Error::CodecBad` (we don't support read-until-close;
///     there is no way for an incremental parser to know "done" without
///     either CL or TE, and we reject TE)
[[nodiscard]] inline
std::expected<std::optional<ParseResult<HttpResponse>>, core::ErrorInfo>
parse_http_response(
    std::span<const uint8_t> buffer,
    std::span<HttpHeader>    header_storage
) noexcept {
    const size_t end = buffer.size();
    if (end == 0) return std::optional<ParseResult<HttpResponse>>{};

    // ── Status line: HTTP/1.x SP CODE SP REASON CRLF ──────────────────
    const size_t sl_search_end = std::min(end, kMaxStartLineLength + 2);
    size_t sl_crlf = detail::find_crlf(buffer, 0, sl_search_end);
    if (sl_crlf == sl_search_end) {
        if (sl_search_end < end) {
            SPDLOG_WARN("http: status line exceeds {} bytes", kMaxStartLineLength);
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecOverflow, "response status line too long"});
        }
        return std::optional<ParseResult<HttpResponse>>{};
    }
    if (sl_crlf == 0) {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "empty status line"});
    }

    std::string_view status_line(
        reinterpret_cast<const char*>(buffer.data()), sl_crlf);

    // version
    if (status_line.size() < 12 ||
        status_line.substr(0, 7) != "HTTP/1." ||
        (status_line[7] != '0' && status_line[7] != '1') ||
        status_line[8] != ' ') {
        SPDLOG_WARN("http: unsupported response version");
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "unsupported HTTP version in response"});
    }
    uint8_t version_minor = static_cast<uint8_t>(status_line[7] - '0');

    // status code: exactly 3 ASCII digits
    auto code_str = status_line.substr(9, 3);
    if (code_str.size() != 3 ||
        code_str[0] < '0' || code_str[0] > '9' ||
        code_str[1] < '0' || code_str[1] > '9' ||
        code_str[2] < '0' || code_str[2] > '9') {
        SPDLOG_WARN("http: non-3-digit status code");
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "status code must be 3 digits (RFC 7230 §3.1.2)"});
    }
    uint16_t status_code = static_cast<uint16_t>(
        (code_str[0] - '0') * 100 + (code_str[1] - '0') * 10 + (code_str[2] - '0'));

    // After the code we expect either CRLF (no reason phrase) or SP reason CRLF.
    std::string_view reason_phrase;
    if (status_line.size() == 12) {
        // Exact "HTTP/1.x SP DDD" — no SP reason. Accept as empty reason.
        reason_phrase = {};
    } else if (status_line.size() >= 13 && status_line[12] == ' ') {
        reason_phrase = status_line.substr(13);
        // Validate reason phrase: printable ASCII + HTAB, no CTL.
        if (!detail::is_valid_field_value(reason_phrase)) {
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad, "invalid character in reason phrase"});
        }
    } else {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "malformed status line after code"});
    }

    // ── Headers ───────────────────────────────────────────────────────
    auto hdr_res = detail::parse_header_block(buffer, sl_crlf + 2, header_storage);
    if (!hdr_res.has_value()) return std::unexpected(hdr_res.error());
    if (!hdr_res->has_value()) return std::optional<ParseResult<HttpResponse>>{};
    const auto& st = **hdr_res;

    // ── Body framing ──────────────────────────────────────────────────
    const bool bodyless_status =
        (status_code >= 100 && status_code < 200) ||
        status_code == 204 || status_code == 304;

    std::span<const uint8_t> body_span;
    size_t consumed = st.header_block_end;

    if (bodyless_status) {
        // Body is guaranteed empty per RFC. We ignore any Content-Length.
    } else if (st.has_content_length) {
        const size_t body_end = st.header_block_end + st.content_length;
        if (body_end > end) {
            return std::optional<ParseResult<HttpResponse>>{};
        }
        body_span = buffer.subspan(st.header_block_end, st.content_length);
        consumed  = body_end;
    } else {
        // No CL, no chunked (we reject TE earlier), and status isn't bodyless.
        // We cannot safely frame this — reject. Exchange REST servers
        // always set Content-Length, so hitting this is a bug or a proxy
        // rewriting the response.
        SPDLOG_WARN("http: response without Content-Length (status {})", status_code);
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad,
            "response has no Content-Length and is not bodyless"});
    }

    HttpResponse resp{
        .status_code   = status_code,
        .reason_phrase = reason_phrase,
        .version_minor = version_minor,
        .headers       = header_storage.subspan(0, st.count),
        .body          = body_span,
    };
    SPDLOG_DEBUG("http: parsed response status={} reason='{}' hdrs={} body={}B",
                 status_code, std::string(reason_phrase), st.count,
                 body_span.size());
    return std::optional<ParseResult<HttpResponse>>{
        ParseResult<HttpResponse>{resp, consumed}};
}

// ─────────────────────────────────────────────────────────────────────────────
// build_http_request
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Serialize an HTTP/1.1 request into a caller-owned buffer.
///
/// @return number of bytes written, or `CodecOverflow` if `cap` is too small,
///         or `CodecBad` if any input contains CR/LF/NUL/whitespace-in-token.
///
/// The caller is responsible for also supplying a `Content-Length` header in
/// `headers` when `body` is non-empty. We do **not** auto-inject CL — the
/// plan's contract is "zero heap, zero hidden behavior"; builder symmetry
/// with parse is valuable for tests and for CONNECT tunnels (where the body
/// is empty and no CL is desired). If you want auto-CL, build it into your
/// own wrapper.
[[nodiscard]] inline
std::expected<size_t, core::ErrorInfo>
build_http_request(
    uint8_t*                      buf,
    size_t                        cap,
    std::string_view              method,
    std::string_view              target,
    std::span<const HttpHeader>   headers,
    std::span<const uint8_t>      body = {}
) noexcept {
    // ── Input validation (injection defense) ──────────────────────────
    if (method.empty() || !detail::is_valid_token(method)) {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "invalid method token (builder)"});
    }
    if (target.empty() || detail::builder_reject_unsafe_token(target)) {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "invalid target (CRLF/NUL/whitespace)"});
    }
    if (headers.size() > kMaxHeaderCount) {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecOverflow, "too many headers (builder)"});
    }
    for (const auto& h : headers) {
        if (h.name.empty() || !detail::is_valid_token(h.name)) {
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad, "invalid header name (builder)"});
        }
        if (detail::builder_reject_unsafe_value(h.value)) {
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad, "invalid header value (CRLF/NUL)"});
        }
    }

    size_t off = 0;
    // "METHOD SP TARGET SP HTTP/1.1\r\n"
    if (!detail::buf_append_sv(buf, cap, off, method))            goto overflow;
    if (!detail::buf_append_cstr(buf, cap, off, " "))             goto overflow;
    if (!detail::buf_append_sv(buf, cap, off, target))            goto overflow;
    if (!detail::buf_append_cstr(buf, cap, off, " HTTP/1.1\r\n")) goto overflow;
    for (const auto& h : headers) {
        if (!detail::buf_append_sv(buf, cap, off, h.name))     goto overflow;
        if (!detail::buf_append_cstr(buf, cap, off, ": "))     goto overflow;
        if (!detail::buf_append_sv(buf, cap, off, h.value))    goto overflow;
        if (!detail::buf_append_cstr(buf, cap, off, "\r\n"))   goto overflow;
    }
    if (!detail::buf_append_cstr(buf, cap, off, "\r\n"))      goto overflow;
    if (!body.empty()) {
        if (!detail::buf_append(buf, cap, off, body.data(), body.size()))
            goto overflow;
    }
    SPDLOG_DEBUG("http: built request {} {} bytes={}",
                 std::string(method), std::string(target), off);
    return off;

overflow:
    return std::unexpected(core::ErrorInfo{
        core::Error::CodecOverflow, "buffer too small for request"});
}

// ─────────────────────────────────────────────────────────────────────────────
// build_http_response
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Serialize an HTTP/1.1 response into a caller-owned buffer.
///
/// Same contract / caveats as `build_http_request`: no auto Content-Length,
/// no hidden headers, zero heap. Primarily used by tests and by the mock
/// echo server; production flow is always "parse", not "build".
[[nodiscard]] inline
std::expected<size_t, core::ErrorInfo>
build_http_response(
    uint8_t*                      buf,
    size_t                        cap,
    uint16_t                      status_code,
    std::string_view              reason_phrase,
    std::span<const HttpHeader>   headers,
    std::span<const uint8_t>      body = {}
) noexcept {
    if (status_code < 100 || status_code > 599) {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "status code out of 100..599"});
    }
    if (detail::builder_reject_unsafe_value(reason_phrase)) {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecBad, "invalid reason phrase (CRLF/NUL)"});
    }
    if (headers.size() > kMaxHeaderCount) {
        return std::unexpected(core::ErrorInfo{
            core::Error::CodecOverflow, "too many headers (builder)"});
    }
    for (const auto& h : headers) {
        if (h.name.empty() || !detail::is_valid_token(h.name)) {
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad, "invalid header name (builder)"});
        }
        if (detail::builder_reject_unsafe_value(h.value)) {
            return std::unexpected(core::ErrorInfo{
                core::Error::CodecBad, "invalid header value (CRLF/NUL)"});
        }
    }

    size_t off = 0;
    // "HTTP/1.1 DDD REASON\r\n"
    if (!detail::buf_append_cstr(buf, cap, off, "HTTP/1.1 "))  goto overflow;
    if (!detail::buf_append_decimal(buf, cap, off, status_code)) goto overflow;
    if (!detail::buf_append_cstr(buf, cap, off, " "))          goto overflow;
    if (!detail::buf_append_sv(buf, cap, off, reason_phrase))  goto overflow;
    if (!detail::buf_append_cstr(buf, cap, off, "\r\n"))       goto overflow;
    for (const auto& h : headers) {
        if (!detail::buf_append_sv(buf, cap, off, h.name))    goto overflow;
        if (!detail::buf_append_cstr(buf, cap, off, ": "))    goto overflow;
        if (!detail::buf_append_sv(buf, cap, off, h.value))   goto overflow;
        if (!detail::buf_append_cstr(buf, cap, off, "\r\n"))  goto overflow;
    }
    if (!detail::buf_append_cstr(buf, cap, off, "\r\n"))      goto overflow;
    if (!body.empty()) {
        if (!detail::buf_append(buf, cap, off, body.data(), body.size()))
            goto overflow;
    }
    SPDLOG_DEBUG("http: built response status={} bytes={}", status_code, off);
    return off;

overflow:
    return std::unexpected(core::ErrorInfo{
        core::Error::CodecOverflow, "buffer too small for response"});
}

} // namespace eph::net
