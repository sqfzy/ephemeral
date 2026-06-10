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
    // Production client_key is 24 bytes (base64 of a 16-byte nonce).
    // The server-side `mockex/ws_server.hpp::accept_handshake` extracts
    // the `Sec-WebSocket-Key` header value verbatim from a remote HTTP
    // request, however, so a malicious / buggy peer can deliver an
    // arbitrarily-sized string here. Size scratch generously and clamp
    // the per-call copy to the explicit budget so stack-buffer overflow
    // is impossible regardless of input length.
    //
    // Layout: [client_key bytes (≤ kMaxKeyLen)] [kWsMagicGuid (36 bytes)]
    //         total ≤ 192 bytes — comfortably under one stack page and
    //         keeps SHA-1's 64-byte block iteration count bounded.
    constexpr size_t kMaxKeyLen = 156;  // RFC 6455 §4.1 key is 24B; 156 covers
                                        // every documented length and well
                                        // beyond, while keeping scratch ≤ 192B.
    char scratch[kMaxKeyLen + kWsMagicGuid.size()];
    static_assert(sizeof(scratch) >= kMaxKeyLen + kWsMagicGuid.size(),
                  "ws_compute_accept scratch must hold key + magic GUID");
    const size_t n1 = std::min<size_t>(client_key.size(), kMaxKeyLen);
    if (n1 < client_key.size()) [[unlikely]] {
        SPDLOG_WARN("ws_compute_accept: client_key truncated from {} to {} "
                    "bytes (expected ~24)", client_key.size(), n1);
    }
    if (n1 > 0) {
        std::memcpy(scratch, client_key.data(), n1);
    }
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

/// @brief Find header by case-insensitive name in a parsed HTTP response.
[[nodiscard]] inline std::optional<std::string_view>
ws_find_header(std::span<const HttpHeader> headers,
               std::string_view            name) noexcept {
    for (const auto& h : headers) {
        if (iequal(h.name, name)) return h.value;
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
            if (iequal(tok, "Upgrade")) return true;
        }
        pos = (end < value.size()) ? end + 1 : end;
    }
    return false;
}

// ---------------------------------------------------------------------------
// permessage-deflate (RFC 7692) negotiation helpers
// ---------------------------------------------------------------------------

/// @brief Per-handshake permessage-deflate state, both input (what to
///        request) and output (what was negotiated).
///
/// Lives outside `StreamConfig` so the handshake helper can be tested
/// against fake byte sinks without dragging the backend config types in.
/// The TcpStream factories materialise an instance, set the input
/// fields from the user's `StreamConfig`, run `perform_ws_handshake`,
/// then read the output fields to drive the codec hookup.
struct WsHandshakeDeflate {
    /// Input: if true, the handshake injects
    /// `Sec-WebSocket-Extensions: permessage-deflate` (with no
    /// non-default parameters — RFC 7692 §7 default
    /// `client_max_window_bits` is 15) into the request. Caller-supplied
    /// `extra_headers` already carrying the same name take precedence;
    /// the helper does not duplicate.
    bool request{false};

    /// Output: server confirmed the extension. Only set when `request`
    /// was true AND the server's `Sec-WebSocket-Extensions` response
    /// matched a parameter set we know how to handle. If the server
    /// included unknown parameters or a different extension, the field
    /// stays false and the handshake itself errors out
    /// (`WsHandshakeFailed`) — fail-closed is the conservative
    /// interpretation of "we cannot honour the extension contract".
    bool negotiated{false};

    /// Output: server told us via `server_no_context_takeover` that it
    /// would NOT preserve its LZ77 window across messages. Only
    /// meaningful when `negotiated=true`.
    bool server_no_context_takeover{false};
};

/// @brief Parse one comma-delimited extension token from a
///        `Sec-WebSocket-Extensions` header value and update `state`
///        if it matches `permessage-deflate`.
///
/// Recognised parameters (RFC 7692 §7.1):
///   * `server_no_context_takeover`               — sets state.server_no_context_takeover
///   * `client_no_context_takeover`               — accepted, no state change
///                                                  (we never preserve client context
///                                                  since we don't deflate outbound)
///   * `server_max_window_bits` (= 15)            — accepted iff equals 15 (default).
///                                                  Other values mean the server wants
///                                                  a smaller window than zlib's
///                                                  `-MAX_WBITS` (=15) inflater is
///                                                  configured for; rather than
///                                                  reinitialise we conservatively
///                                                  reject.
///   * `client_max_window_bits` (= 15)            — same constraint.
///
/// Any unknown parameter fails-closed: the function returns false so
/// the caller can reject the whole handshake. This is deliberate — a
/// silent "extension partly supported" mode would silently corrupt
/// data when the wire format we don't understand kicks in.
[[nodiscard]] inline bool
ws_parse_permessage_deflate_token(std::string_view              tok,
                                  WsHandshakeDeflate&           state) noexcept {
    // Strip leading/trailing OWS.
    auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t')) s.remove_suffix(1);
        return s;
    };
    tok = trim(tok);
    if (tok.empty()) return false;

    // Split off the extension name (up to the first ';').
    const size_t sc = tok.find(';');
    std::string_view name = trim(tok.substr(0, sc));
    if (!iequal(name, "permessage-deflate")) {
        return false;
    }

    state.negotiated = true;

    if (sc == std::string_view::npos) {
        // No parameters; defaults everywhere.
        return true;
    }

    // Walk the remaining ';'-delimited params.
    std::string_view rest = tok.substr(sc + 1);
    while (!rest.empty()) {
        const size_t next = rest.find(';');
        std::string_view param = trim(rest.substr(0, next));
        rest = (next == std::string_view::npos)
                   ? std::string_view{}
                   : rest.substr(next + 1);
        if (param.empty()) continue;

        const size_t eq = param.find('=');
        std::string_view pname  = trim(param.substr(0, eq));
        std::string_view pvalue = (eq == std::string_view::npos)
                                      ? std::string_view{}
                                      : trim(param.substr(eq + 1));
        // Strip surrounding quotes per RFC 7230 quoted-string form.
        if (pvalue.size() >= 2 && pvalue.front() == '"' && pvalue.back() == '"') {
            pvalue = pvalue.substr(1, pvalue.size() - 2);
        }

        if (iequal(pname, "server_no_context_takeover")) {
            // No value expected per RFC 7692.
            state.server_no_context_takeover = true;
        } else if (iequal(pname, "client_no_context_takeover")) {
            // We don't deflate outbound, so this is informational —
            // we never had context to take over. Accept silently.
        } else if (iequal(pname, "server_max_window_bits") ||
                   iequal(pname, "client_max_window_bits")) {
            // We hard-code 15 (zlib's -MAX_WBITS) at inflater init.
            // Anything else needs re-init we don't bother with — fail
            // closed and let the user disable deflate via config.
            if (pvalue != "15") {
                SPDLOG_WARN(
                    "ws_handshake: server set {}={} (we only support 15)",
                    pname, pvalue);
                state.negotiated = false;
                return false;
            }
        } else {
            SPDLOG_WARN("ws_handshake: unknown permessage-deflate "
                        "parameter '{}'", pname);
            state.negotiated = false;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// WsHandshakeDriver — resumable, non-blocking WS Upgrade state machine
// ---------------------------------------------------------------------------

/// @brief Poll-loop-driven client WebSocket Upgrade handshake.
///
/// The non-blocking core that `perform_ws_handshake` wraps for the blocking
/// path and that the TcpStream connect state machine drives directly across
/// poll cycles. `create()` builds the request once; each `step(io)` performs
/// at most one non-blocking `send`/`recv` on the `ByteSink` and reports
/// progress. RFC 6455 §4.1 verification (status 101, Upgrade/Connection
/// headers, Sec-WebSocket-Accept, permessage-deflate negotiation) runs once
/// the response is fully buffered; post-handshake over-read is exposed via
/// `leftover()`.
class WsHandshakeDriver {
public:
    /// @brief Build the Upgrade request and return a driver ready to `step()`.
    ///        Performs the mandatory-header collision check, nonce generation
    ///        and request serialization up front (the only fallible cold work)
    ///        so `step()` is pure I/O + parsing.
    [[nodiscard]] static std::expected<WsHandshakeDriver, ::eph::core::ErrorInfo>
    create(std::string_view            host,
           std::string_view            ws_path,
           std::span<const HttpHeader> extra_headers = {},
           WsHandshakeDeflate*         deflate       = nullptr) noexcept {
        WsHandshakeDriver d;
        d.deflate_ = deflate;

        // ── 0. Reject extra headers colliding with RFC 6455 mandatory names ──
        {
            static constexpr std::string_view kMandatoryNames[] = {
                "Host", "Upgrade", "Connection",
                "Sec-WebSocket-Key", "Sec-WebSocket-Version",
            };
            for (const auto& eh : extra_headers) {
                for (auto mn : kMandatoryNames) {
                    if (iequal(eh.name, mn)) {
                        SPDLOG_WARN("ws_handshake: extra header '{}' conflicts "
                                    "with mandatory WebSocket header", eh.name);
                        return std::unexpected(::eph::core::ErrorInfo{
                            ::eph::core::Error::WsHandshakeFailed,
                            "ws_handshake: extra header conflicts with a "
                            "mandatory WebSocket header"});
                    }
                }
            }
        }

        // ── 1. Generate + base64-encode the 16-byte client nonce ──────────────
        uint8_t nonce[16];
        if (!ws_random_nonce(nonce)) {
            SPDLOG_ERROR("ws_handshake: getrandom failed; cannot generate nonce");
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WsHandshakeFailed,
                "ws_handshake: getrandom failed"});
        }
        d.client_key_ = ::eph::core::detail::base64_encode(nonce, sizeof(nonce));

        // ── 2. Build the HTTP/1.1 Upgrade request ─────────────────────────────
        constexpr size_t kBaseHdrs = 5;
        const bool inject_deflate =
            (deflate && deflate->request) &&
            std::none_of(extra_headers.begin(), extra_headers.end(),
                         [](const HttpHeader& h) {
                             return iequal(h.name, "Sec-WebSocket-Extensions");
                         });
        const size_t injected_count = inject_deflate ? 1u : 0u;
        if (extra_headers.size() + injected_count > kMaxHeaderCount - kBaseHdrs) {
            SPDLOG_WARN("ws_handshake: too many extra headers: {} (+ {} injected)",
                        extra_headers.size(), injected_count);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WsHandshakeFailed,
                "ws_handshake: too many extra headers"});
        }
        std::array<HttpHeader, kMaxHeaderCount> hdrs{};
        hdrs[0] = HttpHeader{"Host",                  host};
        hdrs[1] = HttpHeader{"Upgrade",               "websocket"};
        hdrs[2] = HttpHeader{"Connection",            "Upgrade"};
        hdrs[3] = HttpHeader{"Sec-WebSocket-Key",     d.client_key_};
        hdrs[4] = HttpHeader{"Sec-WebSocket-Version", "13"};
        size_t cursor = kBaseHdrs;
        if (inject_deflate) {
            hdrs[cursor++] = HttpHeader{"Sec-WebSocket-Extensions",
                                        "permessage-deflate"};
        }
        for (size_t i = 0; i < extra_headers.size(); ++i) {
            hdrs[cursor + i] = extra_headers[i];
        }
        const size_t total_hdrs = cursor + extra_headers.size();

        auto built = build_http_request(
            d.req_.data(), d.req_.size(), "GET", ws_path,
            std::span<const HttpHeader>(hdrs.data(), total_hdrs));
        if (!built) {
            SPDLOG_WARN("ws_handshake: build_http_request failed: {}",
                        built.error().detail);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WsHandshakeFailed,
                "ws_handshake: failed to build request"});
        }
        d.req_len_ = *built;
        d.host_    = host;
        d.ws_path_ = ws_path;
        return d;
    }

    /// @brief Advance one non-blocking step.
    /// @return `true`  — handshake complete (`leftover()` valid);
    ///         `false` — pending (call again on the next readable/writable poll);
    ///         `unexpected` — fatal handshake error.
    template <class ByteSink>
    [[nodiscard]] std::expected<bool, ::eph::core::ErrorInfo>
    step(ByteSink& io) noexcept {
        if (phase_ == Phase::Sending) {
            auto sr = io.send(std::span<const uint8_t>(
                req_.data() + sent_, req_len_ - sent_));
            if (!sr) {
                if (sr.error().code == ::eph::core::Error::WouldBlock) return false;
                SPDLOG_WARN("ws_handshake: ByteSink::send err={}", sr.error().detail);
                return std::unexpected(sr.error());
            }
            if (*sr == 0) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::BufferFull,
                    "ws_handshake: ByteSink::send returned 0 bytes"});
            }
            sent_ += *sr;
            if (sent_ < req_len_) return false;  // partial — resume next cycle
            SPDLOG_DEBUG("ws_handshake: sent {}B request", req_len_);
            phase_ = Phase::Receiving;
            return false;  // begin receiving next cycle
        }

        if (phase_ == Phase::Receiving) {
            if (rx_len_ == rx_.size()) {
                SPDLOG_WARN("ws_handshake: response exceeds {}B buffer", rx_.size());
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::WsHandshakeFailed,
                    "ws_handshake: response too large"});
            }
            auto rr = io.recv(rx_.data() + rx_len_, rx_.size() - rx_len_);
            if (!rr) {
                if (rr.error().code == ::eph::core::Error::WouldBlock) return false;
                SPDLOG_WARN("ws_handshake: ByteSink::recv err={}", rr.error().detail);
                return std::unexpected(rr.error());
            }
            if (*rr == 0) return false;  // no data yet — pending
            rx_len_ += *rr;

            auto pr = parse_http_response(
                std::span<const uint8_t>(rx_.data(), rx_len_),
                std::span<HttpHeader>(header_storage_, kMaxHeaderCount));
            if (!pr) {
                SPDLOG_WARN("ws_handshake: parse_http_response err={}",
                            pr.error().detail);
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::WsHandshakeFailed,
                    "ws_handshake: malformed response"});
            }
            if (!pr->has_value()) return false;  // incomplete — recv more
            return validate_(**pr);
        }

        // Called after Done/Failed — programming error.
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::WsHandshakeFailed,
            "ws_handshake: step() after terminal phase"});
    }

    /// @brief Post-handshake over-read bytes (arrived in the same recv as the
    ///        101 response). Valid only after `step()` returned `true`.
    [[nodiscard]] std::span<const uint8_t> leftover() const noexcept {
        return std::span<const uint8_t>(leftover_.data(), leftover_.size());
    }

private:
    enum class Phase : uint8_t { Sending, Receiving, Done, Failed };

    /// @brief RFC 6455 §4.1 steps 5–9: validate the fully-buffered response
    ///        and stash over-read. Returns `true` on success.
    [[nodiscard]] std::expected<bool, ::eph::core::ErrorInfo>
    validate_(const ParseResult<HttpResponse>& parsed) noexcept {
        const auto& resp = parsed.value;

        // ── 5. status == 101 ──
        if (resp.status_code != 101) [[unlikely]] {
            SPDLOG_WARN("ws_handshake: unexpected status {} (reason='{}')",
                        resp.status_code, resp.reason_phrase);
            phase_ = Phase::Failed;
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WsHandshakeFailed,
                "ws_handshake: non-101 status"});
        }
        // ── 6. Upgrade: websocket ──
        auto upgrade_hdr = ws_find_header(resp.headers, "Upgrade");
        if (!upgrade_hdr || !iequal(*upgrade_hdr, "websocket")) [[unlikely]] {
            SPDLOG_WARN("ws_handshake: missing/invalid Upgrade header");
            phase_ = Phase::Failed;
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WsHandshakeFailed,
                "ws_handshake: missing or invalid Upgrade header"});
        }
        // ── 7. Connection: Upgrade ──
        auto conn_hdr = ws_find_header(resp.headers, "Connection");
        if (!conn_hdr || !ws_connection_has_upgrade(*conn_hdr)) [[unlikely]] {
            SPDLOG_WARN("ws_handshake: missing/invalid Connection: Upgrade");
            phase_ = Phase::Failed;
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WsHandshakeFailed,
                "ws_handshake: missing Connection: Upgrade"});
        }
        // ── 8. Sec-WebSocket-Accept ──
        auto accept_hdr = ws_find_header(resp.headers, "Sec-WebSocket-Accept");
        if (!accept_hdr) [[unlikely]] {
            SPDLOG_WARN("ws_handshake: missing Sec-WebSocket-Accept header");
            phase_ = Phase::Failed;
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WsHandshakeFailed,
                "ws_handshake: missing Sec-WebSocket-Accept"});
        }
        const std::string expected_accept = ws_compute_accept(client_key_);
        if (*accept_hdr != expected_accept) [[unlikely]] {
            SPDLOG_WARN("ws_handshake: Sec-WebSocket-Accept mismatch "
                        "(expected='{}' got='{}')", expected_accept, *accept_hdr);
            phase_ = Phase::Failed;
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::WsHandshakeFailed,
                "ws_handshake: Sec-WebSocket-Accept mismatch"});
        }

        // ── 8.5. Sec-WebSocket-Extensions handling ──
        auto ext_hdr = ws_find_header(resp.headers, "Sec-WebSocket-Extensions");
        if (ext_hdr) {
            const bool requested_deflate = (deflate_ && deflate_->request);
            if (!requested_deflate) {
                SPDLOG_WARN("ws_handshake: server enabled unsolicited extension(s) "
                            "'{}' — rejecting (no extensions were offered)", *ext_hdr);
                phase_ = Phase::Failed;
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::WsHandshakeFailed,
                    "ws_handshake: server enabled unsolicited "
                    "Sec-WebSocket-Extensions"});
            }
            bool any_known = false;
            std::string_view list = *ext_hdr;
            while (!list.empty()) {
                const size_t comma = list.find(',');
                std::string_view tok = list.substr(0, comma);
                list = (comma == std::string_view::npos)
                           ? std::string_view{} : list.substr(comma + 1);
                if (tok.empty()) continue;
                if (!ws_parse_permessage_deflate_token(tok, *deflate_)) {
                    SPDLOG_WARN("ws_handshake: server returned unsupported "
                                "extension token '{}' (full: '{}')", tok, *ext_hdr);
                    phase_ = Phase::Failed;
                    return std::unexpected(::eph::core::ErrorInfo{
                        ::eph::core::Error::WsHandshakeFailed,
                        "ws_handshake: server returned unsupported "
                        "Sec-WebSocket-Extensions"});
                }
                any_known = true;
            }
            if (!any_known || !deflate_->negotiated) {
                SPDLOG_WARN("ws_handshake: empty/unparseable "
                            "Sec-WebSocket-Extensions response '{}'", *ext_hdr);
                phase_ = Phase::Failed;
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::WsHandshakeFailed,
                    "ws_handshake: empty/unparseable extension response"});
            }
            SPDLOG_INFO("ws_handshake: permessage-deflate negotiated "
                        "(server_no_context_takeover={})",
                        deflate_->server_no_context_takeover);
        } else if (deflate_ && deflate_->request) {
            SPDLOG_DEBUG("ws_handshake: server did not accept "
                         "permessage-deflate — falling back to uncompressed");
        }

        // ── 9. Stash over-read for the caller's reasm buffer ──
        const size_t consumed = parsed.consumed;
        if (consumed < rx_len_) {
            leftover_.assign(rx_.data() + consumed, rx_.data() + rx_len_);
            SPDLOG_DEBUG("ws_handshake: stashed {}B of over-read post-handshake "
                         "bytes", rx_len_ - consumed);
        }
        SPDLOG_INFO("ws_handshake: OK path='{}' host='{}' ({}B req, {}B resp)",
                    ws_path_, host_, req_len_, consumed);
        phase_ = Phase::Done;
        return true;
    }

    Phase               phase_   = Phase::Sending;
    WsHandshakeDeflate* deflate_ = nullptr;
    std::array<uint8_t, 4096> req_{};
    size_t              req_len_ = 0;
    size_t              sent_    = 0;
    std::array<uint8_t, 8192> rx_{};
    size_t              rx_len_  = 0;
    std::string         client_key_;
    std::string_view    host_;
    std::string_view    ws_path_;
    HttpHeader          header_storage_[kMaxHeaderCount];
    std::vector<uint8_t> leftover_;
};

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
    std::vector<uint8_t>*           leftover      = nullptr,
    WsHandshakeDeflate*             deflate       = nullptr) noexcept
{
    auto d = WsHandshakeDriver::create(host, ws_path, extra_headers, deflate);
    if (!d) return std::unexpected(d.error());

    // Blocking drive: step until done or the cumulative deadline expires.
    // The non-blocking TcpStream path drives WsHandshakeDriver directly.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto r = d->step(io);
        if (!r) return std::unexpected(r.error());
        if (*r) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            SPDLOG_WARN("ws_handshake: deadline exceeded ({}ms)", timeout.count());
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::Timeout, "ws_handshake: deadline exceeded"});
        }
    }
    if (leftover != nullptr) {
        auto lo = d->leftover();
        leftover->insert(leftover->end(), lo.begin(), lo.end());
    }
    return {};
}

} // namespace eph::net::detail
