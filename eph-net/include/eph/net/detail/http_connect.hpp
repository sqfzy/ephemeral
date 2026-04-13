#pragma once

/// @file http_connect.hpp
/// Client-side HTTP CONNECT proxy handshake over a generic ByteSink.
///
/// Mirrors the template/adapter pattern established by
/// `eph/net/detail/ws_handshake.hpp`: the helper takes any
/// object satisfying the duck-typed ByteSink contract
///
///     struct ByteSink {
///         std::expected<size_t, core::ErrorInfo>
///         send(std::span<const uint8_t>) noexcept;
///
///         // Returns bytes read. WouldBlock is retried until the deadline
///         // trips. Disconnected / other errors bubble up as handshake
///         // failure.
///         std::expected<size_t, core::ErrorInfo>
///         recv(uint8_t* buf, size_t cap) noexcept;
///     };
///
/// and drives the following RFC 7231 §4.3.6 CONNECT flow:
///
///   1. Build a CONNECT request via `build_http_request`:
///
///        CONNECT target_host:target_port HTTP/1.1
///        Host: target_host:target_port
///        Proxy-Connection: Keep-Alive
///        [Proxy-Authorization: Basic <base64(user:pass)>]
///
///   2. Send the request (looping on WouldBlock until the deadline).
///   3. Recv the response incrementally — loop `parse_http_response` until
///      it returns `Complete` or the deadline expires.
///   4. On 200 (any reason phrase) → success.
///   5. On 407 → `Error::ProxyAuthRequired`.
///   6. On any other status → `Error::ProxyHandshakeFailed`.
///   7. On parse error → `Error::ProxyHandshakeFailed`.
///   8. On deadline expiry → `Error::Timeout`.
///
/// The helper is invoked *before* the TLS handshake: any bytes that arrive
/// in the same `recv()` call as the 200 response but beyond the response
/// headers belong to the upstream application layer (almost always a TLS
/// ServerHello that started back-to-back with the proxy's 200 — rare but
/// legal, and common when the proxy and the origin are on the same LAN).
/// Those bytes are returned via the optional `leftover` out-parameter so
/// the caller can push them into its TLS ciphertext accumulator before
/// kicking off the TLS state machine.
///
/// All error paths return `ErrorInfo` with static-literal `detail` — the
/// hot path (success) does no heap allocation other than the small
/// `basic_auth_token` std::string (which fits SSO on every libstdc++ we
/// target even for a 254-char colon-joined user:pass).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/detail/base64.hpp"
#include "eph/core/error.hpp"
#include "eph/net/http.hpp"
#include "eph/net/proxy.hpp"

namespace eph::net::detail {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Format `host:port` into a caller-provided buffer.
///
/// Used both for the `CONNECT` request-target and for the `Host` header.
/// Returns a string_view into `scratch`. The scratch buffer must outlive
/// the returned view.
[[nodiscard]] inline std::string_view
format_host_port(char (&scratch)[320],
                 std::string_view target_host,
                 uint16_t         target_port) noexcept {
    // Max host length in practice is 253 chars (DNS label limit).
    // Reserving 320 (= 253 + 1 + 5 for port + '\0') is generous.
    const size_t host_len = std::min<size_t>(target_host.size(), 253);
    std::memcpy(scratch, target_host.data(), host_len);
    size_t off = host_len;
    scratch[off++] = ':';
    // Decimal-encode the port without going through std::to_string to
    // stay allocation-free on this path.
    char port_digits[6];
    size_t pd = 0;
    uint16_t p = target_port;
    if (p == 0) {
        port_digits[pd++] = '0';
    } else {
        while (p > 0) {
            port_digits[pd++] = static_cast<char>('0' + (p % 10));
            p /= 10;
        }
    }
    // Reverse into scratch.
    while (pd > 0) {
        scratch[off++] = port_digits[--pd];
    }
    return std::string_view{scratch, off};
}

// ---------------------------------------------------------------------------
// CONNECT-response fallback parser
// ---------------------------------------------------------------------------
//
// Our shared `parse_http_response` rejects any non-1xx/204/304 response
// without a `Content-Length` header. That's the safe default for exchange
// REST traffic, but RFC 7231 §4.3.6 explicitly says a successful CONNECT
// response is framed as if the body has length zero — many real proxies
// omit Content-Length on the 200. We fall back to a tiny hand-rolled
// status-line extractor for the CONNECT response case.
//
// The fallback only runs if the main parser rejected the response with
// CodecBad; it is deliberately forgiving about CL framing and only needs
// to pull out the status code and the offset of the "\r\n\r\n" terminator.

/// @brief Parsed result of the CONNECT fallback.
struct ConnectFallback {
    uint16_t status_code;
    size_t   consumed;  ///< byte offset after the terminating CRLF CRLF
};

/// @brief Try to extract a CONNECT response status code + header-block
///        length from a raw byte buffer. Returns nullopt on malformed
///        input or if the \r\n\r\n terminator hasn't arrived yet.
[[nodiscard]] inline std::optional<ConnectFallback>
try_parse_connect_response(std::span<const uint8_t> buf) noexcept {
    // Need at least "HTTP/1.x DDD \r\n\r\n" (~16 bytes).
    if (buf.size() < 16) return std::nullopt;
    // Status line must start with "HTTP/1."
    if (std::memcmp(buf.data(), "HTTP/1.", 7) != 0) return std::nullopt;
    // Find end-of-headers (\r\n\r\n).
    size_t term = std::string_view::npos;
    for (size_t i = 0; i + 3 < buf.size(); ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            term = i + 4;
            break;
        }
    }
    if (term == std::string_view::npos) return std::nullopt;

    // Parse status code: skip "HTTP/1.x " (9 chars).
    if (buf.size() < 12) return std::nullopt;
    size_t p = 9;
    uint32_t code = 0;
    size_t digits = 0;
    while (p < buf.size() && buf[p] >= '0' && buf[p] <= '9') {
        code = code * 10 + (buf[p] - '0');
        ++p; ++digits;
        if (digits > 3) return std::nullopt;
    }
    if (digits != 3) return std::nullopt;
    if (code < 100 || code > 599) return std::nullopt;
    return ConnectFallback{static_cast<uint16_t>(code), term};
}

/// @brief Build the `Proxy-Authorization: Basic ...` header value from
///        an already-validated ProxyConfig.
///
/// Per RFC 7617 §2 the credentials are serialized as `user:pass`, then
/// base64-encoded (with '=' padding). We prepend "Basic " so the return
/// value can be dropped directly into an HttpHeader value slot.
///
/// @return the full header value (e.g. "Basic dXNlcjpwYXNz"), or an empty
///         string if the proxy has no auth fields set.
[[nodiscard]] inline std::string
build_proxy_auth_value(const ProxyConfig& proxy) {
    if (!proxy.basic_auth_user.has_value() ||
        !proxy.basic_auth_pass.has_value()) {
        return {};
    }
    std::string joined;
    joined.reserve(proxy.basic_auth_user->size() + 1 +
                   proxy.basic_auth_pass->size());
    joined.append(*proxy.basic_auth_user);
    joined.push_back(':');
    joined.append(*proxy.basic_auth_pass);
    std::string encoded = ::eph::core::detail::base64_encode(
        reinterpret_cast<const uint8_t*>(joined.data()), joined.size());
    std::string out;
    out.reserve(6 + encoded.size());  // "Basic " + digits
    out.append("Basic ");
    out.append(encoded);
    return out;
}

// ---------------------------------------------------------------------------
// perform_http_connect
// ---------------------------------------------------------------------------

/// @brief Drive a client-side HTTP CONNECT handshake over a byte sink.
///
/// @tparam ByteSink      Duck-typed — see the file-level comment.
///
/// @param io             The already-connected byte sink to the proxy.
/// @param proxy          Validated ProxyConfig. Caller is responsible for
///                       having called `proxy.validate()` beforehand.
/// @param target_host    Upstream host the caller wants the proxy to tunnel
///                       to (used both as CONNECT target and Host header).
/// @param target_port    Upstream port.
/// @param leftover       Optional out-parameter: any bytes received in the
///                       same `recv()` call as the 200 response but beyond
///                       the HTTP body boundary. These belong to the
///                       upstream layer (e.g. the start of a TLS
///                       ClientHelloReply or raw app data) and must be
///                       fed into the next protocol's reassembly buffer
///                       before I/O resumes. Pass `nullptr` to drop them
///                       (logged as a WARN).
///
/// @return `{}` on a clean 200 response; otherwise an ErrorInfo mapped per
///         the file-level comment.
template <class ByteSink>
[[nodiscard]] inline std::expected<void, ::eph::core::ErrorInfo>
perform_http_connect(
    ByteSink&            io,
    const ProxyConfig&   proxy,
    std::string_view     target_host,
    uint16_t             target_port,
    std::vector<uint8_t>* leftover = nullptr) noexcept
{
    SPDLOG_DEBUG("http_connect: begin proxy={}:{} target={}:{} auth={} timeout={}ms",
                 proxy.host, proxy.port,
                 std::string(target_host), target_port,
                 proxy.basic_auth_user.has_value(),
                 proxy.timeout.count());

    // ── 1. Build host:port literal (used as request-target + Host header) ──
    char hp_scratch[320];
    const std::string_view target_authority =
        format_host_port(hp_scratch, target_host, target_port);

    // ── 2. Optionally compute Proxy-Authorization value ───────────────────
    //
    // We must keep the returned string alive for the duration of the
    // `HttpHeader` view, so it lives on the stack here and the builder
    // copies its contents into the request buffer.
    std::string proxy_auth_value;
    try {
        proxy_auth_value = build_proxy_auth_value(proxy);
    } catch (...) {
        // base64_encode uses std::string and can theoretically throw
        // bad_alloc. We intentionally suppress to keep the helper noexcept.
        SPDLOG_ERROR("http_connect: build_proxy_auth_value threw; bailing out");
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::ProxyHandshakeFailed,
            "http_connect: proxy auth encoding failed"});
    }

    // ── 3. Assemble headers ───────────────────────────────────────────────
    //
    // RFC 7231 §4.3.6 requires only the Host header for CONNECT, but in
    // practice every proxy we care about also expects Proxy-Connection
    // to keep the socket around for the tunneled traffic.
    std::array<HttpHeader, 3> hdrs{};
    size_t nhdrs = 0;
    hdrs[nhdrs++] = HttpHeader{"Host",             target_authority};
    hdrs[nhdrs++] = HttpHeader{"Proxy-Connection", "Keep-Alive"};
    if (!proxy_auth_value.empty()) {
        hdrs[nhdrs++] = HttpHeader{"Proxy-Authorization", proxy_auth_value};
    }

    uint8_t req_buf[2048];
    auto built = build_http_request(
        req_buf, sizeof(req_buf),
        "CONNECT", target_authority,
        std::span<const HttpHeader>(hdrs.data(), nhdrs));
    if (!built) {
        SPDLOG_WARN("http_connect: build_http_request failed: {}",
                    built.error().detail);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::ProxyHandshakeFailed,
            "http_connect: failed to build CONNECT request"});
    }
    const size_t req_len = *built;

    // ── 4. Send the request (loop until fully drained) ────────────────────
    const auto deadline = std::chrono::steady_clock::now() + proxy.timeout;
    size_t sent = 0;
    while (sent < req_len) {
        if (std::chrono::steady_clock::now() >= deadline) {
            SPDLOG_WARN("http_connect: send deadline exceeded at {}/{} bytes",
                        sent, req_len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::Timeout,
                "http_connect: send timeout"});
        }
        auto sr = io.send(std::span<const uint8_t>(req_buf + sent,
                                                     req_len - sent));
        if (!sr) {
            if (sr.error().code == ::eph::core::Error::WouldBlock) continue;
            SPDLOG_WARN("http_connect: ByteSink::send err={}",
                        sr.error().detail);
            return std::unexpected(sr.error());
        }
        sent += *sr;
    }
    SPDLOG_DEBUG("http_connect: sent {}B CONNECT request", req_len);

    // ── 5. Recv response and parse incrementally ──────────────────────────
    //
    // 4 KiB is far more than any realistic proxy response (200 / 407 / 502
    // all fit in <300B on Squid + nginx + HAProxy). The extra headroom
    // guards against proxies that pad with long banner / server headers.
    uint8_t  rx_buf[4096];
    size_t   rx_len = 0;
    HttpHeader header_storage[kMaxHeaderCount];
    std::optional<ParseResult<HttpResponse>> parsed;
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
            SPDLOG_WARN("http_connect: read deadline exceeded at {}B buffered",
                        rx_len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::Timeout,
                "http_connect: read timeout"});
        }
        if (rx_len == sizeof(rx_buf)) {
            SPDLOG_WARN("http_connect: response exceeds {}B buffer",
                        sizeof(rx_buf));
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::ProxyHandshakeFailed,
                "http_connect: response too large"});
        }
        auto rr = io.recv(rx_buf + rx_len, sizeof(rx_buf) - rx_len);
        if (!rr) {
            if (rr.error().code == ::eph::core::Error::WouldBlock) continue;
            SPDLOG_WARN("http_connect: ByteSink::recv err={}",
                        rr.error().detail);
            return std::unexpected(rr.error());
        }
        if (*rr == 0) {
            // Treat 0 as WouldBlock (FakeByteSink convention). Real sinks
            // would return Disconnected, which we already propagate above.
            continue;
        }
        rx_len += *rr;

        auto pr = parse_http_response(
            std::span<const uint8_t>(rx_buf, rx_len),
            std::span<HttpHeader>(header_storage, kMaxHeaderCount));
        if (!pr) {
            // RFC 7231 §4.3.6 allows a CONNECT 2xx to omit Content-Length.
            // The shared parser rejects that ("response has no
            // Content-Length and is not bodyless") because its contract
            // targets exchange REST traffic — here we fall back to a
            // permissive status-line-only extractor so Squid / nginx /
            // HAProxy 200 Connection Established interops cleanly.
            auto fb = try_parse_connect_response(
                std::span<const uint8_t>(rx_buf, rx_len));
            if (fb.has_value()) {
                parsed = ParseResult<HttpResponse>{
                    HttpResponse{
                        .status_code   = fb->status_code,
                        .reason_phrase = std::string_view{},
                        .version_minor = 1,
                        .headers       = std::span<const HttpHeader>{},
                        .body          = std::span<const uint8_t>{},
                    },
                    fb->consumed,
                };
                break;
            }
            SPDLOG_WARN("http_connect: parse_http_response err={}",
                        pr.error().detail);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::ProxyHandshakeFailed,
                "http_connect: malformed response"});
        }
        if (pr->has_value()) {
            parsed = **pr;
            break;
        }
        // Incomplete — loop and recv more.
    }

    const auto& resp = parsed->value;

    // ── 6. Status code → Error mapping ────────────────────────────────────
    //
    // Any 2xx is in principle acceptable per RFC 7231, but real proxies
    // only ever return 200 ("Connection established"). We accept the full
    // 200..299 band for robustness but log non-200 success as a WARN so
    // operators notice.
    if (resp.status_code == 407) {
        SPDLOG_WARN("http_connect: proxy returned 407 Proxy Auth Required");
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::ProxyAuthRequired,
            "http_connect: proxy requires authentication (407)"});
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        SPDLOG_WARN("http_connect: non-2xx status {} (reason='{}')",
                    resp.status_code, std::string(resp.reason_phrase));
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::ProxyHandshakeFailed,
            "http_connect: proxy returned non-2xx status"});
    }
    if (resp.status_code != 200) {
        SPDLOG_WARN("http_connect: unexpected 2xx {} (reason='{}')",
                    resp.status_code, std::string(resp.reason_phrase));
    }

    // ── 7. Stash any over-read bytes for the caller's reasm buffer ────────
    //
    // `parsed->consumed` is the number of bytes the parser treated as
    // "part of the response". Anything beyond that in rx_buf belongs to
    // the next layer (TLS ClientHelloReply, WS Upgrade response, raw app).
    const size_t consumed = parsed->consumed;
    if (leftover != nullptr && consumed < rx_len) {
        const size_t tail = rx_len - consumed;
        leftover->insert(leftover->end(),
                          rx_buf + consumed,
                          rx_buf + consumed + tail);
        SPDLOG_DEBUG("http_connect: stashed {}B of over-read post-CONNECT bytes",
                     tail);
    } else if (leftover == nullptr && consumed < rx_len) {
        SPDLOG_WARN("http_connect: {}B of over-read bytes dropped "
                    "(no leftover sink provided)", rx_len - consumed);
    }

    SPDLOG_INFO("http_connect: OK target={}:{} via {}:{} ({}B req, {}B resp)",
                std::string(target_host), target_port,
                proxy.host, proxy.port, req_len, consumed);
    return {};
}

} // namespace eph::net::detail
