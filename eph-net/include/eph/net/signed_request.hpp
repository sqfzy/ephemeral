#pragma once

/// @file signed_request.hpp
/// @brief Venue-traits-driven HMAC sign-into-header helper.
///
/// ## Why this exists
///
/// `eph::net::HmacSha256Key` + `hmac_sha256_sign` already give us a clean
/// primitive — they compute the MAC. But the wire format every crypto venue
/// expects is different in three orthogonal ways:
///
///   1. **What gets MAC'd** — Binance signs the querystring (with the
///      timestamp pre-appended); OKX signs `timestamp + method + path +
///      body`; Bybit signs `timestamp + api_key + recv_window + payload`.
///   2. **How the tag is rendered** — Binance and Bybit use lowercase hex,
///      OKX uses base64.
///   3. **Which headers carry what** — `X-MBX-SIGNATURE` / `X-MBX-TIMESTAMP`
///      vs `OK-ACCESS-SIGN` / `OK-ACCESS-TIMESTAMP` vs `X-BAPI-SIGN` /
///      `X-BAPI-TIMESTAMP` / `X-BAPI-RECV-WINDOW`.
///
/// We model this as a **traits struct per venue**. The traits are static
/// inline pure functions — no virtual dispatch, no runtime venue switch —
/// so picking a venue at compile time is a zero-cost decision.
/// `SignedRequest<Traits>` then provides a uniform `headers_for_*` API that
/// returns the right set of `HttpHeader` records for the caller to splice
/// onto their HTTP request.
///
/// ## Scope
///
/// We ship Binance, OKX, and Bybit (the three biggest crypto venues by
/// volume). Other venues are user extensions: write a struct that matches
/// the (unwritten) "venue traits" duck-typed shape and instantiate
/// `SignedRequest<MyTraits>`. Concretely the traits must expose:
///
///   * `static constexpr std::string_view sign_header`
///   * `static constexpr std::string_view ts_header`
///   * a `format_signature(std::span<const uint8_t, 32>)` returning the
///     ASCII rendering used by that venue (hex or base64)
///   * a `build_to_sign(...)` overload appropriate to that venue's signing
///     scheme — the `SignedRequest` template forwards via tagged member
///     functions per venue, so additional venues will need additional
///     `headers_for_*` helpers; we deliberately do not pretend a single
///     uniform `build_to_sign` signature works for all of them.
///
/// ## Threading
///
/// `SignedRequest` is not thread-safe for construction/destruction (it
/// owns the moved-in `HmacSha256Key`). It is thread-safe for concurrent
/// signing — the underlying `hmac_sha256_sign` is stateless. Per the HFT
/// threading model each venue adapter typically owns its own
/// `SignedRequest` on a single trading thread anyway.
///
/// ## Hot path / allocation
///
/// `headers_for_*` allocates: a `std::vector<HttpHeader>` (3-4 elements)
/// plus 1-2 small `std::string`s for the rendered signature/timestamp. The
/// caller side of the API surface is the place where allocation is
/// already happening (HTTP request building); the underlying
/// `hmac_sha256_sign` itself remains zero-alloc and `noexcept`. If callers
/// want zero-alloc signing they can use `HmacSha256` directly and format
/// the headers into pre-allocated buffers — the traits' `build_to_sign` /
/// `format_signature` are public and reusable.
///
/// ## Lifetime
///
/// All `string_view` fields in the returned `HttpHeader` records point at
/// strings owned by the `SignedRequest` invocation that produced them. We
/// return strings (heap-owned) bundled in a `SignedHeaderBundle` so that
/// the views remain valid until the caller is done splicing them into the
/// outgoing request.

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/detail/base64.hpp" // pure-C++ base64 encoder
#include "eph/net/hmac.hpp"
#include "eph/net/http.hpp"           // HttpHeader

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// Common helpers — render the raw 32-byte tag into venue-specific encodings
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @brief Lowercase 64-char hex render of a 32-byte tag.
///
/// Wraps `HmacSha256Tag::to_hex()` for a `std::span<const uint8_t, 32>`
/// input so the venue traits can take the raw bytes (rather than depending
/// on the `HmacSha256Tag` type directly — keeps the traits structurally
/// pluggable for users who want to plug in their own MAC primitive).
[[nodiscard]] inline std::string render_hex_lower(
    std::span<const uint8_t, 32> raw) {
    HmacSha256Tag tag{};
    std::memcpy(tag.bytes.data(), raw.data(), 32);
    return tag.to_hex();
}

/// @brief Standard RFC 4648 base64 of a 32-byte tag.
///
/// Returns 44 characters: 32 bytes → 44 chars including '=' padding.
[[nodiscard]] inline std::string render_base64(
    std::span<const uint8_t, 32> raw) {
    return ::eph::core::detail::base64_encode(raw.data(), raw.size());
}

/// @brief Format a uint64_t as decimal ASCII (no leading zeros, no padding).
///
/// We avoid `std::to_string` to be std::format-bug-resistant on older libc++
/// builds; the manual implementation is also a touch faster on the hot path.
[[nodiscard]] inline std::string u64_to_decimal(uint64_t v) {
    if (v == 0) return std::string{"0"};
    char buf[20];
    size_t n = 0;
    while (v > 0 && n < sizeof(buf)) {
        buf[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = buf[n - 1 - i];
    return s;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Binance traits (Spot / Futures REST: SIGNED endpoints)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Binance Spot/Futures HMAC-SHA256 signing traits.
///
/// Reference: https://binance-docs.github.io/apidocs/spot/en/#signed-trade-user_data-and-margin-endpoint-security
///
/// The Binance signing rule is conceptually simple:
///
///   1. Append `timestamp=<ms>` (and optionally `recvWindow=<ms>`) to the
///      querystring or body that the request would otherwise send.
///   2. HMAC-SHA256 the resulting *querystring* (or x-www-form-urlencoded
///      body, for POST) with the API secret.
///   3. Append `&signature=<hex>` to the original querystring/body OR send
///      the signature in the `X-MBX-SIGNATURE` header — the docs accept
///      both. We expose the header path because that's where the rest of
///      the codebase routes auth (Tokio-style: don't surprise the user
///      with a hidden querystring rewrite).
///
/// `X-MBX-APIKEY` (the public key) is the user's responsibility — the
/// secret-bearing struct is intentionally kept far from key registration.
struct BinanceSignTraits {
    static constexpr std::string_view sign_header = "X-MBX-SIGNATURE";
    static constexpr std::string_view ts_header   = "X-MBX-TIMESTAMP";

    /// @brief Build the Binance pre-MAC string.
    ///
    /// For GET: `query` is the full querystring with `timestamp=<ms>`
    /// already spliced in by the caller — Binance signs whatever the wire
    /// query is. We accept it pre-formed rather than re-splicing here so
    /// the caller stays in control of parameter order (Binance does not
    /// require canonical ordering, but signing what you actually send is
    /// the only safe rule).
    ///
    /// For POST x-www-form-urlencoded: pass the body as `query` and an
    /// empty `body` — the body IS the signed string for Binance.
    ///
    /// `body` is reserved for venues that sign body separately; here it is
    /// always concatenated AFTER the querystring (trivially compatible
    /// with the "sign exactly what you send" rule).
    [[nodiscard]] static std::string build_to_sign(
        std::string_view query,
        std::string_view body,
        uint64_t /*ts_ms unused — caller has already embedded it in `query`*/) {
        std::string s;
        s.reserve(query.size() + body.size());
        s.append(query);
        s.append(body);
        return s;
    }

    /// @brief Binance signature wire format: lowercase 64-char hex.
    [[nodiscard]] static std::string format_signature(
        std::span<const uint8_t, 32> raw) {
        return detail::render_hex_lower(raw);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// OKX traits (V5 REST)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief OKX V5 REST HMAC-SHA256 signing traits.
///
/// Reference: https://www.okx.com/docs-v5/en/#overview-rest-authentication-signature
///
/// The OKX signing rule is:
///
///   * Pre-MAC string = timestamp + method + requestPath + body
///     - `timestamp` is ISO-8601 with milliseconds, e.g. "2020-12-08T09:08:57.715Z"
///     - `method` is uppercase: "GET" / "POST" / "DELETE" / "PUT"
///     - `requestPath` is the URL path INCLUDING any querystring, e.g.
///       "/api/v5/account/balance?ccy=BTC"
///     - `body` is the raw JSON body for POST, or empty string for GET
///   * MAC the pre-MAC string with the secret key
///   * Encode the MAC as **base64** (not hex) — this is the OKX
///     idiosyncrasy; everyone else does hex.
///
/// OKX additionally requires `OK-ACCESS-KEY` (api key, public),
/// `OK-ACCESS-PASSPHRASE` (the password set when the API key was
/// generated), and optionally `OK-ACCESS-PROJECT` for some Web3 endpoints.
/// Those are caller-managed; the traits here only handle the signature
/// + timestamp pair.
struct OkxSignTraits {
    static constexpr std::string_view sign_header = "OK-ACCESS-SIGN";
    static constexpr std::string_view ts_header   = "OK-ACCESS-TIMESTAMP";

    /// @brief Build the OKX pre-MAC string.
    ///
    /// `path` should already include any querystring (as required by OKX).
    /// `body` is empty for GET / DELETE, the JSON request body for POST.
    /// `ts_iso` is the ISO-8601-with-ms UTC timestamp string (caller-
    /// formatted — we do not call `clock_gettime` here because (a) HFT
    /// threading model owns clock access and (b) some adapters need
    /// monotonic-vs-wall control).
    [[nodiscard]] static std::string build_to_sign(
        std::string_view method,
        std::string_view path,
        std::string_view body,
        std::string_view ts_iso) {
        std::string s;
        s.reserve(ts_iso.size() + method.size() + path.size() + body.size());
        s.append(ts_iso);
        s.append(method);
        s.append(path);
        s.append(body);
        return s;
    }

    /// @brief OKX signature wire format: standard RFC 4648 base64 (44 chars,
    ///        '=' padded). 32-byte input always produces "..=" suffix.
    [[nodiscard]] static std::string format_signature(
        std::span<const uint8_t, 32> raw) {
        return detail::render_base64(raw);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Bybit V5 traits
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Bybit V5 REST HMAC-SHA256 signing traits.
///
/// Reference: https://bybit-exchange.github.io/docs/v5/guide#parameters-for-authenticated-endpoints
///
/// The Bybit signing rule is:
///
///   * Pre-MAC string = timestamp + api_key + recv_window + (queryString OR body)
///     - `timestamp` is ms since epoch as a decimal string ("1658385579423")
///     - `api_key` is the public key (Bybit's signing scheme bakes the
///       public key INTO the MAC input — uncommon but means a leaked
///       secret cannot be reused with a different key)
///     - `recv_window` is the stale-tolerance window in ms (string)
///     - For GET, the payload is the canonicalized querystring (NO leading
///       '?'); for POST it is the raw JSON body string.
///   * MAC with the API secret, render as lowercase hex.
struct BybitSignTraits {
    static constexpr std::string_view sign_header = "X-BAPI-SIGN";
    static constexpr std::string_view ts_header   = "X-BAPI-TIMESTAMP";
    static constexpr std::string_view recv_header = "X-BAPI-RECV-WINDOW";

    /// @brief Build the Bybit pre-MAC string.
    [[nodiscard]] static std::string build_to_sign(
        std::string_view api_key,
        std::string_view ts_ms,
        std::string_view recv_window,
        std::string_view payload) {
        std::string s;
        s.reserve(ts_ms.size() + api_key.size() + recv_window.size() + payload.size());
        s.append(ts_ms);
        s.append(api_key);
        s.append(recv_window);
        s.append(payload);
        return s;
    }

    /// @brief Bybit signature wire format: lowercase 64-char hex (same as
    ///        Binance).
    [[nodiscard]] static std::string format_signature(
        std::span<const uint8_t, 32> raw) {
        return detail::render_hex_lower(raw);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// SignedHeaderBundle — owns the rendered strings, exposes views via .headers
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A bag of rendered headers produced by `SignedRequest`.
///
/// Owns the backing `std::string`s so the `HttpHeader::value` views remain
/// valid for the lifetime of the bundle. Caller copies the headers into
/// their `HttpClient::Request::headers` (typically by appending to the
/// caller-provided header storage span) before the bundle goes out of
/// scope.
///
/// We keep this dead simple — no fancy allocator, no SBO. The hot path
/// for any HFT REST flow is "construct request → send → wait". The 1-3
/// small heap allocations involved are negligible compared to the syscall
/// + TLS + network round-trip.
struct SignedHeaderBundle {
    /// @brief Backing storage for header values. Index-aligned with `headers`
    ///        when the bundle was built — but callers should treat this as
    ///        opaque and only read from `headers`.
    std::vector<std::string>  storage;

    /// @brief Header views safe to splice onto an outgoing request, valid
    ///        for the lifetime of this bundle.
    std::vector<HttpHeader>   headers;

    SignedHeaderBundle() = default;

    // Non-copyable: copying would clone `storage` into fresh std::string
    // objects with different addresses, while `headers` would be copied
    // verbatim with views still pointing into the ORIGINAL bundle's
    // storage — once the original goes out of scope the copy holds
    // dangling string_views (use-after-free). Move is O(1) ptr-swap on
    // the underlying vectors so the contained string objects keep their
    // addresses; the views stay valid against the moved-into bundle.
    SignedHeaderBundle(const SignedHeaderBundle&)            = delete;
    SignedHeaderBundle& operator=(const SignedHeaderBundle&) = delete;
    SignedHeaderBundle(SignedHeaderBundle&&)            noexcept = default;
    SignedHeaderBundle& operator=(SignedHeaderBundle&&) noexcept = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// SignedRequest<Traits> — the user-facing helper
// ─────────────────────────────────────────────────────────────────────────────

/// @brief HMAC sign-into-header helper, parameterized by venue traits.
///
/// Owns a `HmacSha256Key` (RAII-cleared on destruction). All signing goes
/// through the traits' `build_to_sign` / `format_signature` so the wire
/// format is correct by construction for the chosen venue.
template <class Traits>
class SignedRequest {
public:
    /// @brief Construct from a moved-in key. Non-copyable transitively
    ///        (HmacSha256Key is non-copyable).
    explicit SignedRequest(HmacSha256Key key) noexcept
        : key_(std::move(key)) {
        SPDLOG_TRACE("SignedRequest<{}>: constructed",
                     typeid(Traits).name());
    }

    SignedRequest(const SignedRequest&)            = delete;
    SignedRequest& operator=(const SignedRequest&) = delete;
    SignedRequest(SignedRequest&&)            noexcept = default;
    SignedRequest& operator=(SignedRequest&&) noexcept = default;

    // ── Accessors on the underlying key ────────────────────────────────────
    /// @brief Read-only key access — primarily for callers that want to
    ///        bypass `headers_for_*` and sign into a custom buffer.
    [[nodiscard]] const HmacSha256Key& key() const noexcept { return key_; }

    // ─────────────────────────────────────────────────────────────────────
    // Binance — `headers_for_query` / `headers_for_body`
    //
    // These overloads are only enabled when Traits == BinanceSignTraits;
    // SFINAE is via `requires` clauses so a different Traits doesn't get
    // confused-name-lookup errors at instantiation.
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Build Binance signed headers for a GET-style query.
    ///
    /// `query_with_timestamp` is the full querystring including
    /// `timestamp=<ms>` already spliced in. We sign it verbatim.
    ///
    /// The bundle contains:
    ///   * `X-MBX-SIGNATURE: <hex>`
    ///   * `X-MBX-TIMESTAMP: <ts_ms decimal>`
    ///
    /// Caller is still responsible for `X-MBX-APIKEY`.
    [[nodiscard]] SignedHeaderBundle headers_for_query(
        std::string_view query_with_timestamp,
        uint64_t         ts_ms) const
        requires std::is_same_v<Traits, BinanceSignTraits>
    {
        const std::string to_sign = Traits::build_to_sign(
            query_with_timestamp, /*body=*/"", ts_ms);
        return finalize_(to_sign,
                         /*ts_str=*/detail::u64_to_decimal(ts_ms),
                         /*recv_window=*/{});
    }

    /// @brief Build Binance signed headers for a POST x-www-form-urlencoded body.
    ///
    /// Same rule: pass the body verbatim as the signed payload.
    [[nodiscard]] SignedHeaderBundle headers_for_body(
        std::string_view body_with_timestamp,
        uint64_t         ts_ms) const
        requires std::is_same_v<Traits, BinanceSignTraits>
    {
        const std::string to_sign = Traits::build_to_sign(
            body_with_timestamp, /*body=*/"", ts_ms);
        return finalize_(to_sign,
                         /*ts_str=*/detail::u64_to_decimal(ts_ms),
                         /*recv_window=*/{});
    }

    // ─────────────────────────────────────────────────────────────────────
    // OKX — `headers_for_okx`
    //
    // OKX signs `timestamp + method + path + body`, base64. We expose a
    // single `headers_for_okx` helper because the OKX scheme doesn't fit
    // the "query vs body" dichotomy of Binance/Bybit.
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Build OKX V5 signed headers.
    ///
    /// `path` MUST include any querystring (per OKX docs); for POST
    /// requests `body` is the raw JSON; for GET it is empty. `ts_iso` is
    /// the ISO-8601-with-ms timestamp the caller will also send in
    /// `OK-ACCESS-TIMESTAMP` — we use the same string for both signing
    /// and the header value to guarantee they agree.
    ///
    /// Bundle contains:
    ///   * `OK-ACCESS-SIGN: <base64>`
    ///   * `OK-ACCESS-TIMESTAMP: <ts_iso>`
    ///
    /// Caller still responsible for `OK-ACCESS-KEY` and
    /// `OK-ACCESS-PASSPHRASE`.
    [[nodiscard]] SignedHeaderBundle headers_for_okx(
        std::string_view method,
        std::string_view path,
        std::string_view body,
        std::string_view ts_iso) const
        requires std::is_same_v<Traits, OkxSignTraits>
    {
        const std::string to_sign = Traits::build_to_sign(
            method, path, body, ts_iso);
        return finalize_(to_sign,
                         /*ts_str=*/std::string{ts_iso},
                         /*recv_window=*/{});
    }

    // ─────────────────────────────────────────────────────────────────────
    // Bybit — `headers_for_bybit`
    //
    // Bybit signs `ts + api_key + recv_window + (query OR body)`, hex. The
    // signing input includes the public api_key and recv_window, so we
    // accept all three as parameters here.
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Build Bybit V5 signed headers.
    ///
    /// `payload` is either the canonicalized querystring (no '?' prefix)
    /// for GET/DELETE, or the raw JSON body for POST. `recv_window` is the
    /// stale-tolerance window in ms as a string, e.g. "5000".
    ///
    /// Bundle contains:
    ///   * `X-BAPI-SIGN: <hex>`
    ///   * `X-BAPI-TIMESTAMP: <ts_ms>`
    ///   * `X-BAPI-RECV-WINDOW: <recv_window>`
    ///
    /// Caller still responsible for `X-BAPI-API-KEY`.
    [[nodiscard]] SignedHeaderBundle headers_for_bybit(
        std::string_view api_key,
        std::string_view ts_ms,
        std::string_view recv_window,
        std::string_view payload) const
        requires std::is_same_v<Traits, BybitSignTraits>
    {
        const std::string to_sign = Traits::build_to_sign(
            api_key, ts_ms, recv_window, payload);
        return finalize_(to_sign,
                         /*ts_str=*/std::string{ts_ms},
                         /*recv_window=*/std::string{recv_window});
    }

    // ─────────────────────────────────────────────────────────────────────
    // Low-level escape hatch: just return the rendered signature string
    // for callers that want to format the headers themselves (or extend to
    // a venue this template doesn't cover yet).
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Sign `to_sign` and return the venue-formatted signature.
    ///
    /// Useful for: (a) custom venues whose Traits don't fit the
    /// `headers_for_*` shape, (b) tests, (c) any caller that wants to
    /// avoid the small allocations in `headers_for_*`.
    [[nodiscard]] std::string sign_to_string(std::string_view to_sign) const noexcept(false) {
        const auto tag = hmac_sha256_sign(key_, to_sign);
        return Traits::format_signature(
            std::span<const uint8_t, 32>{tag.bytes.data(), 32});
    }

private:
    /// @brief Build the bundle from an already-built pre-MAC string and
    ///        already-formatted timestamp/recv-window strings.
    ///
    /// `recv_window` is empty for venues that don't have a recv-window
    /// header — in that case it is omitted from the returned bundle.
    [[nodiscard]] SignedHeaderBundle finalize_(
        const std::string& to_sign,
        std::string        ts_str,
        std::string        recv_window) const {
        SignedHeaderBundle out;
        out.storage.reserve(3);
        out.headers.reserve(3);

        // 1) Signature
        const auto tag = hmac_sha256_sign(key_, to_sign);
        out.storage.emplace_back(Traits::format_signature(
            std::span<const uint8_t, 32>{tag.bytes.data(), 32}));
        out.headers.push_back(HttpHeader{
            Traits::sign_header,
            std::string_view{out.storage.back()}});

        // 2) Timestamp
        out.storage.emplace_back(std::move(ts_str));
        out.headers.push_back(HttpHeader{
            Traits::ts_header,
            std::string_view{out.storage.back()}});

        // 3) Recv-window (Bybit only — empty string suppresses it).
        if constexpr (requires { Traits::recv_header; }) {
            if (!recv_window.empty()) {
                out.storage.emplace_back(std::move(recv_window));
                out.headers.push_back(HttpHeader{
                    Traits::recv_header,
                    std::string_view{out.storage.back()}});
            }
        }

        SPDLOG_DEBUG("SignedRequest: produced {} headers (sign={}B)",
                     out.headers.size(),
                     out.storage.empty() ? 0 : out.storage.front().size());
        return out;
    }

    HmacSha256Key key_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time guarantees
// ─────────────────────────────────────────────────────────────────────────────

static_assert(!std::is_copy_constructible_v<SignedRequest<BinanceSignTraits>>,
              "SignedRequest must be non-copyable (key is non-copyable)");
static_assert(std::is_nothrow_move_constructible_v<SignedRequest<BinanceSignTraits>>,
              "SignedRequest move must be noexcept (HmacSha256Key move is noexcept)");

} // namespace eph::net
