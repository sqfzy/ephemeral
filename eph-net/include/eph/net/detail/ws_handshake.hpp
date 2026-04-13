#pragma once

/// @file ws_handshake.hpp
/// Client-side WebSocket HTTP Upgrade handshake over a generic ByteSink.
///
/// This helper performs steps 1-10 of RFC 6455 §4.1 over an already-
/// established byte-level transport (plaintext TCP *or* TLS-wrapped TCP,
/// decided by the caller's `ByteSink` instantiation — see §D-2 of the
/// plan for why the design is config-driven rather than a separate
/// `connect_websocket()` function):
///
///   1. Generate 16 random bytes → base64-encode as `Sec-WebSocket-Key`
///   2. Build the HTTP/1.1 Upgrade request via `build_http_request`
///   3. Send the request via `io.send` (looping until fully drained)
///   4. Recv the response via `io.recv` until `parse_http_response`
///      returns `Complete` or the deadline expires
///   5. Verify the status line is `101 Switching Protocols`
///   6. Verify `Upgrade: websocket` (case-insensitive per RFC 6455 §4.1)
///   7. Verify `Connection: Upgrade` (case-insensitive, token match)
///   8. Verify `Sec-WebSocket-Accept == base64(SHA1(key + magic_guid))`
///
/// Error mapping (all return `core::ErrorInfo` with static-literal detail):
///   * Parser errors      → `Error::WsHandshakeFailed`
///   * Status != 101      → `Error::WsHandshakeFailed`
///   * Missing/bad headers → `Error::WsHandshakeFailed`
///   * Wrong Accept digest → `Error::WsHandshakeFailed`
///   * Deadline exceeded  → `Error::Timeout`
///   * Any I/O error from the ByteSink bubbles up verbatim.
///
/// ByteSink contract (duck-typed):
///
///   struct ByteSink {
///       std::expected<size_t, core::ErrorInfo>
///       send(std::span<const uint8_t>) noexcept;
///
///       // Returns bytes read. WouldBlock is treated as "retry";
///       // Disconnected / other errors bubble up as handshake failure.
///       std::expected<size_t, core::ErrorInfo>
///       recv(uint8_t* buf, size_t cap) noexcept;
///   };
///
/// The helper is zero-heap *after* the small std::string holding the
/// Sec-WebSocket-Accept digest (compile-time 29 bytes — fits SSO on every
/// libstdc++/libc++ we target). Request build + response parse run on
/// stack-allocated scratch buffers sized for the handshake.

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

#include <sys/random.h>  // getrandom(2)

#include <spdlog/spdlog.h>

#include "eph/core/detail/base64.hpp"
#include "eph/core/error.hpp"
#include "eph/net/http.hpp"

namespace eph::net::detail {

// ---------------------------------------------------------------------------
// SHA-1 (RFC 3174)
// ---------------------------------------------------------------------------
//
// A tiny, self-contained SHA-1 implementation dedicated to computing the
// WS Sec-WebSocket-Accept digest. We deliberately do NOT reuse aws-lc's
// `SHA1()` function here because this helper also has to compile inside
// eph-net (which is header-only and does not link aws-lc transitively via
// its public consumers — e.g. tests that only exercise the handshake
// against a fake byte sink). Aligning with the bench mock's implementation
// in `benchmarks/latency/core/ws_handshake.hpp` also keeps the two sides
// byte-for-byte comparable if a regression ever surfaces.
//
// 80-ish lines of plain bitwise ops; zero allocations; noexcept.

struct WsSha1 {
    uint32_t h[5];
    uint64_t length;  // total input length in bits
    uint8_t  buffer[64];
    size_t   buffer_used;

    static constexpr uint32_t rotl(uint32_t x, int n) noexcept {
        return (x << n) | (x >> (32 - n));
    }

    void init() noexcept {
        h[0] = 0x67452301u; h[1] = 0xEFCDAB89u; h[2] = 0x98BADCFEu;
        h[3] = 0x10325476u; h[4] = 0xC3D2E1F0u;
        length = 0; buffer_used = 0;
    }

    void process_block(const uint8_t* block) noexcept {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4])     << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) <<  8) |
                    static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if      (i < 20) { f = (b & c) | ((~b) & d);        k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
            uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const uint8_t* data, size_t len) noexcept {
        length += static_cast<uint64_t>(len) * 8u;
        while (len > 0) {
            const size_t take = std::min<size_t>(len, 64 - buffer_used);
            std::memcpy(buffer + buffer_used, data, take);
            buffer_used += take; data += take; len -= take;
            if (buffer_used == 64) {
                process_block(buffer);
                buffer_used = 0;
            }
        }
    }

    void finalize(uint8_t out[20]) noexcept {
        // Snapshot the bit-length BEFORE appending padding; update() mutates
        // `length` per fed byte, so padding bytes would otherwise inflate it.
        const uint64_t len_bits = length;
        const uint8_t  pad      = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (buffer_used != 56) update(&zero, 1);
        for (int i = 7; i >= 0; --i) {
            const uint8_t b = static_cast<uint8_t>((len_bits >> (i * 8)) & 0xFFu);
            update(&b, 1);
        }
        for (int i = 0; i < 5; ++i) {
            out[i * 4]     = static_cast<uint8_t>((h[i] >> 24) & 0xFFu);
            out[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFFu);
            out[i * 4 + 2] = static_cast<uint8_t>((h[i] >>  8) & 0xFFu);
            out[i * 4 + 3] = static_cast<uint8_t>( h[i]        & 0xFFu);
        }
    }
};

/// @brief One-shot SHA-1 over a string view, returning the 20-byte digest.
[[nodiscard]] inline std::array<uint8_t, 20>
ws_sha1(std::string_view input) noexcept {
    WsSha1 ctx;
    ctx.init();
    ctx.update(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    std::array<uint8_t, 20> digest{};
    ctx.finalize(digest.data());
    return digest;
}

// ---------------------------------------------------------------------------
// RFC 6455 magic GUID — concatenated with the client's Sec-WebSocket-Key
// ---------------------------------------------------------------------------
inline constexpr std::string_view kWsMagicGuid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// @brief Compute Sec-WebSocket-Accept for a given Sec-WebSocket-Key.
///
/// Defined as:  base64(SHA1(key + magic_guid))
///
/// @return a heap-allocated std::string (typically 28 bytes + '=' padding),
///         suitable for both building the request-side mock response and
///         for verifying the server's response on the client side.
[[nodiscard]] inline std::string
ws_compute_accept(std::string_view client_key) noexcept {
    // Concat on the stack — client_key is <= 24 bytes, guid is 36 bytes.
    // Total input to SHA-1 is <= 60 bytes, well under one SHA-1 block.
    char scratch[64];
    const size_t n1 = std::min<size_t>(client_key.size(), 32);
    std::memcpy(scratch, client_key.data(), n1);
    std::memcpy(scratch + n1, kWsMagicGuid.data(), kWsMagicGuid.size());
    const auto digest = ws_sha1(std::string_view{scratch, n1 + kWsMagicGuid.size()});
    return ::eph::core::detail::base64_encode(digest.data(), digest.size());
}

// ---------------------------------------------------------------------------
// Random 16-byte nonce via getrandom(2). Returns false on repeated kernel failure.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool ws_random_nonce(uint8_t out[16]) noexcept {
    for (int attempt = 0; attempt < 3; ++attempt) {
        ssize_t got = ::getrandom(out, 16, 0);
        if (got == 16) return true;
        // On EINTR / short read the glibc wrapper already loops, but be
        // defensive; retry up to 3x before giving up.
        SPDLOG_WARN("ws_handshake: getrandom returned {} (attempt {}/3)",
                    got, attempt + 1);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Case-insensitive ASCII equality, local to avoid pulling http.hpp internals.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool ws_iequal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        uint8_t ca = static_cast<uint8_t>(a[i]);
        uint8_t cb = static_cast<uint8_t>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<uint8_t>(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<uint8_t>(cb + 32);
        if (ca != cb) return false;
    }
    return true;
}

/// @brief Find header by case-insensitive name in a parsed HTTP response.
[[nodiscard]] inline std::optional<std::string_view>
ws_find_header(std::span<const HttpHeader> headers,
               std::string_view            name) noexcept {
    for (const auto& h : headers) {
        if (ws_iequal(h.name, name)) return h.value;
    }
    return std::nullopt;
}

/// @brief RFC 7230 §3.2.6 token split for `Connection` header values:
///        check whether any comma-delimited token equals `needle` (case-ins).
[[nodiscard]] inline bool
ws_connection_has_upgrade(std::string_view value) noexcept {
    size_t pos = 0;
    while (pos < value.size()) {
        // Skip leading OWS and commas.
        while (pos < value.size() &&
               (value[pos] == ' ' || value[pos] == '\t' || value[pos] == ',')) {
            ++pos;
        }
        size_t end = pos;
        while (end < value.size() && value[end] != ',') ++end;
        // Trim trailing OWS.
        size_t tok_end = end;
        while (tok_end > pos &&
               (value[tok_end - 1] == ' ' || value[tok_end - 1] == '\t')) {
            --tok_end;
        }
        if (tok_end > pos) {
            std::string_view tok = value.substr(pos, tok_end - pos);
            if (ws_iequal(tok, "Upgrade")) return true;
        }
        pos = (end < value.size()) ? end + 1 : end;
    }
    return false;
}

// ---------------------------------------------------------------------------
// perform_ws_handshake
// ---------------------------------------------------------------------------

/// @brief Drive a client-side WebSocket HTTP Upgrade handshake.
///
/// @param io             ByteSink (see file comment for the duck-typed contract)
/// @param host           Value to emit as the `Host:` header (usually the SNI
///                       hostname for TLS or the raw host:port for plaintext)
/// @param ws_path        Request-target, e.g. `/ws/btcusdt@bookTicker`
/// @param extra_headers  Optional caller-supplied headers appended AFTER the
///                       five mandatory ones. Injection defense is inherited
///                       from `build_http_request`.
/// @param timeout        Cumulative deadline for the whole request+response
///                       phase. Timer uses `clock_gettime(CLOCK_MONOTONIC)`
///                       via `std::chrono::steady_clock`.
/// @param leftover       Optional out-parameter: any bytes that arrived in
///                       the same `recv()` call as the response body but
///                       belong to post-handshake traffic. Caller should
///                       seed them into its reassembly buffer.
///
/// @return `{}` on success, `ErrorInfo` on any failure.
template <class ByteSink>
[[nodiscard]] inline std::expected<void, ::eph::core::ErrorInfo>
perform_ws_handshake(
    ByteSink&                       io,
    std::string_view                host,
    std::string_view                ws_path,
    std::span<const HttpHeader>     extra_headers = {},
    std::chrono::milliseconds       timeout       = std::chrono::seconds{10},
    std::vector<uint8_t>*           leftover      = nullptr) noexcept
{
    SPDLOG_DEBUG("ws_handshake: begin host='{}' path='{}' extras={} timeout={}ms",
                 std::string(host), std::string(ws_path),
                 extra_headers.size(), timeout.count());

    // ── 1. Generate + base64-encode the 16-byte client nonce ──────────────
    uint8_t nonce[16];
    if (!ws_random_nonce(nonce)) {
        SPDLOG_ERROR("ws_handshake: getrandom failed; cannot generate nonce");
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WsHandshakeFailed,
            "ws_handshake: getrandom failed"});
    }
    std::string client_key =
        ::eph::core::detail::base64_encode(nonce, sizeof(nonce));
    SPDLOG_TRACE("ws_handshake: client_key='{}'", client_key);

    // ── 2. Build the HTTP/1.1 Upgrade request ─────────────────────────────
    //
    // Mandatory headers per RFC 6455 §4.1:
    //   Host, Upgrade, Connection, Sec-WebSocket-Key, Sec-WebSocket-Version
    //
    // We pre-declare a small-vector-ish array on the stack and copy in
    // the mandatory + extra headers. 5 + extras <= kMaxHeaderCount (64).
    constexpr size_t kBaseHdrs = 5;
    if (extra_headers.size() > kMaxHeaderCount - kBaseHdrs) {
        SPDLOG_WARN("ws_handshake: too many extra headers: {}",
                    extra_headers.size());
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WsHandshakeFailed,
            "ws_handshake: too many extra headers"});
    }
    std::array<HttpHeader, kMaxHeaderCount> hdrs{};
    hdrs[0] = HttpHeader{"Host",                  host};
    hdrs[1] = HttpHeader{"Upgrade",               "websocket"};
    hdrs[2] = HttpHeader{"Connection",            "Upgrade"};
    hdrs[3] = HttpHeader{"Sec-WebSocket-Key",     client_key};
    hdrs[4] = HttpHeader{"Sec-WebSocket-Version", "13"};
    for (size_t i = 0; i < extra_headers.size(); ++i) {
        hdrs[kBaseHdrs + i] = extra_headers[i];
    }
    const size_t total_hdrs = kBaseHdrs + extra_headers.size();

    uint8_t req_buf[4096];
    auto built = build_http_request(
        req_buf, sizeof(req_buf),
        "GET", ws_path,
        std::span<const HttpHeader>(hdrs.data(), total_hdrs));
    if (!built) {
        SPDLOG_WARN("ws_handshake: build_http_request failed: {}",
                    built.error().detail);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WsHandshakeFailed,
            "ws_handshake: failed to build request"});
    }
    const size_t req_len = *built;

    // ── 3. Send the request (loop until fully drained) ────────────────────
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    size_t sent = 0;
    while (sent < req_len) {
        if (std::chrono::steady_clock::now() >= deadline) {
            SPDLOG_WARN("ws_handshake: send deadline exceeded at {}/{} bytes",
                        sent, req_len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::Timeout,
                "ws_handshake: send timeout"});
        }
        auto sr = io.send(std::span<const uint8_t>(req_buf + sent, req_len - sent));
        if (!sr) {
            // WouldBlock: spin-retry until deadline.
            if (sr.error().code == ::eph::core::Error::WouldBlock) continue;
            SPDLOG_WARN("ws_handshake: ByteSink::send err={}", sr.error().detail);
            return std::unexpected(sr.error());
        }
        sent += *sr;
    }
    SPDLOG_DEBUG("ws_handshake: sent {}B request", req_len);

    // ── 4. Recv response — loop parse_http_response until Complete ────────
    //
    // We stage into a stack buffer sized for a realistic exchange response
    // (~2 KiB is typical for Binance; 8 KiB leaves generous headroom).
    uint8_t  rx_buf[8192];
    size_t   rx_len = 0;
    HttpHeader header_storage[kMaxHeaderCount];
    std::optional<ParseResult<HttpResponse>> parsed;
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
            SPDLOG_WARN("ws_handshake: read deadline exceeded at {}B buffered",
                        rx_len);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::Timeout,
                "ws_handshake: read timeout"});
        }
        if (rx_len == sizeof(rx_buf)) {
            SPDLOG_WARN("ws_handshake: response exceeds {}B buffer",
                        sizeof(rx_buf));
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WsHandshakeFailed,
                "ws_handshake: response too large"});
        }
        auto rr = io.recv(rx_buf + rx_len, sizeof(rx_buf) - rx_len);
        if (!rr) {
            if (rr.error().code == ::eph::core::Error::WouldBlock) continue;
            SPDLOG_WARN("ws_handshake: ByteSink::recv err={}", rr.error().detail);
            return std::unexpected(rr.error());
        }
        if (*rr == 0) {
            // Treat 0 as WouldBlock: the fake sinks in tests return 0 when
            // scripted input is exhausted temporarily. Real impls should
            // return Disconnected instead.
            continue;
        }
        rx_len += *rr;

        auto pr = parse_http_response(
            std::span<const uint8_t>(rx_buf, rx_len),
            std::span<HttpHeader>(header_storage, kMaxHeaderCount));
        if (!pr) {
            SPDLOG_WARN("ws_handshake: parse_http_response err={}",
                        pr.error().detail);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WsHandshakeFailed,
                "ws_handshake: malformed response"});
        }
        if (pr->has_value()) {
            parsed = **pr;
            break;
        }
        // Incomplete — loop and recv more.
    }

    const auto& resp = parsed->value;

    // ── 5. Verify status == 101 Switching Protocols ───────────────────────
    if (resp.status_code != 101) {
        SPDLOG_WARN("ws_handshake: unexpected status {} (reason='{}')",
                    resp.status_code, std::string(resp.reason_phrase));
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WsHandshakeFailed,
            "ws_handshake: non-101 status"});
    }

    // ── 6. Upgrade: websocket (case-insensitive) ──────────────────────────
    auto upgrade_hdr = ws_find_header(resp.headers, "Upgrade");
    if (!upgrade_hdr || !ws_iequal(*upgrade_hdr, "websocket")) {
        SPDLOG_WARN("ws_handshake: missing/invalid Upgrade header");
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WsHandshakeFailed,
            "ws_handshake: missing or invalid Upgrade header"});
    }

    // ── 7. Connection: Upgrade (may be a comma-separated token list) ──────
    auto conn_hdr = ws_find_header(resp.headers, "Connection");
    if (!conn_hdr || !ws_connection_has_upgrade(*conn_hdr)) {
        SPDLOG_WARN("ws_handshake: missing/invalid Connection: Upgrade");
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WsHandshakeFailed,
            "ws_handshake: missing Connection: Upgrade"});
    }

    // ── 8. Sec-WebSocket-Accept correctness ───────────────────────────────
    auto accept_hdr = ws_find_header(resp.headers, "Sec-WebSocket-Accept");
    if (!accept_hdr) {
        SPDLOG_WARN("ws_handshake: missing Sec-WebSocket-Accept header");
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WsHandshakeFailed,
            "ws_handshake: missing Sec-WebSocket-Accept"});
    }
    const std::string expected_accept = ws_compute_accept(client_key);
    if (*accept_hdr != expected_accept) {
        SPDLOG_WARN("ws_handshake: Sec-WebSocket-Accept mismatch "
                    "(expected='{}' got='{}')",
                    expected_accept, std::string(*accept_hdr));
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WsHandshakeFailed,
            "ws_handshake: Sec-WebSocket-Accept mismatch"});
    }

    // ── 9. Stash any over-read bytes for the caller's reasm buffer ────────
    const size_t consumed = parsed->consumed;
    if (leftover != nullptr && consumed < rx_len) {
        const size_t tail = rx_len - consumed;
        leftover->insert(leftover->end(),
                          rx_buf + consumed,
                          rx_buf + consumed + tail);
        SPDLOG_DEBUG("ws_handshake: stashed {}B of over-read post-handshake bytes",
                     tail);
    } else if (leftover == nullptr && consumed < rx_len) {
        SPDLOG_WARN("ws_handshake: {}B of over-read bytes dropped "
                    "(no leftover sink provided)", rx_len - consumed);
    }

    SPDLOG_INFO("ws_handshake: OK path='{}' host='{}' ({}B req, {}B resp)",
                std::string(ws_path), std::string(host), req_len, consumed);
    return {};
}

} // namespace eph::net::detail
