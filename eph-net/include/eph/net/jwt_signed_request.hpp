#pragma once

/// @file jwt_signed_request.hpp
/// @brief ES256 JWT signing helper for the Coinbase Advanced Trade venue.
///
/// ## Why this exists (separate from `signed_request.hpp`)
///
/// The HMAC-SHA256 traits in `signed_request.hpp` cover Binance / OKX /
/// Bybit which all use a symmetric MAC. Coinbase Advanced Trade is the
/// outlier — it requires a **JWT** signed with **ES256** (ECDSA P-256 +
/// SHA-256), an asymmetric scheme. The signing primitive itself
/// (`EVP_DigestSign*`) and the wire-format conventions (JOSE / JWT
/// envelope, base64url, IEEE-P1363 r||s instead of DER) have nothing in
/// common with the HMAC traits, so we keep them in a separate header
/// rather than contorting `SignedRequest<Traits>`. Callers that don't
/// touch Coinbase pay zero cost — the file is not even included
/// transitively.
///
/// ## API shape
///
/// * `Es256PrivateKey` — RAII wrapper around an `EVP_PKEY*` holding a
///   P-256 private key. Loaded from PEM via `from_pem()`. The opaque
///   `void*` field deliberately hides aws-lc types from the public
///   header so consumers don't have to worry about transitive openssl
///   header pollution; the underlying type is documented in comments.
/// * `CoinbaseJwtParams` — the inputs needed to assemble the Coinbase
///   JWT header + payload (the `kid`, `sub`, `iss`, `nbf`, `exp`, `uri`
///   claim shape is fixed by Coinbase docs).
/// * `build_coinbase_jwt(key, params)` — produces the full
///   `header.payload.signature` JWT string. Signature is the ES256
///   r||s (IEEE P-1363) form, base64url-encoded.
///
/// ## The IEEE P-1363 gotcha (see comment in `ecdsa_to_p1363_`)
///
/// `EVP_DigestSignFinal` produces a DER-encoded ECDSA signature
/// (`SEQUENCE { INTEGER r, INTEGER s }`). JOSE / JWT requires the
/// "fixed-width concatenation" form: r as 32 bytes big-endian, s as
/// 32 bytes big-endian, no padding, no length tags — exactly 64 bytes.
/// We unwrap the DER via `ECDSA_SIG_from_bytes`, fish out the two
/// BIGNUMs with `ECDSA_SIG_get0`, and zero-pad each to 32 bytes with
/// `BN_bn2binpad`. Forgetting this conversion is the most common bug
/// in JWT implementations against Coinbase / Apple / similar — the
/// signature decodes locally as DER but the venue rejects it.
///
/// ## Threading
///
/// `Es256PrivateKey` is not thread-safe for construction/destruction
/// (it owns the moved-in `EVP_PKEY*`). It IS thread-safe for concurrent
/// calls to `build_coinbase_jwt` because aws-lc's
/// `EVP_DigestSignInit/Update/Final` against a const-treated `EVP_PKEY`
/// only reads from the key.
///
/// ## Hot-path / allocation
///
/// JWT building is per-REST-request, not per-tick. The 5-7 small heap
/// allocations involved (header / payload / sig strings) are negligible
/// versus the TLS round-trip; we have not made an effort to pool them.
/// If a caller has profiled this and needs zero-alloc signing, the same
/// underlying `ecdsa_p1363_sign_` private helper is reusable against a
/// caller-supplied buffer.

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>   // NID_X9_62_prime256v1
#include <openssl/pem.h>
#include <openssl/rand.h>

#include "eph/core/log.hpp"

#include "eph/core/detail/json_escape.hpp"
#include "eph/core/error.hpp"

namespace eph::net {

namespace detail {
inline spdlog::logger* jwt_signed_request_logger() {
    static spdlog::logger* l = ::eph::log::get("net.jwt_signed_request");
    return l;
}
} // namespace detail

// ────────────────────────────────────────────────────────────────────────────
// detail — base64url + JSON glue
// ────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @brief Base64url encoding (RFC 7515 §2): standard base64 alphabet with
///        `+` → `-`, `/` → `_`, and stripped `=` padding.
///
/// JOSE / JWT uses this variant for every component. We hand-roll it
/// here rather than re-using `eph::core::detail::base64_encode` followed
/// by an in-place fixup because the fixup loop costs more than the
/// encoder itself, and we want a single noexcept-friendly function.
[[nodiscard]] inline std::string base64url_encode(
    const uint8_t* data, size_t len) {
    static constexpr char kChars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    // Output size: 4 chars per 3 bytes, minus padding-equivalent for the
    // last incomplete group. Reserve generously to avoid reallocations.
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t triple =
            (static_cast<uint32_t>(data[i]) << 16) |
            (static_cast<uint32_t>(data[i + 1]) << 8) |
            static_cast<uint32_t>(data[i + 2]);
        result += kChars[(triple >> 18) & 0x3F];
        result += kChars[(triple >> 12) & 0x3F];
        result += kChars[(triple >> 6) & 0x3F];
        result += kChars[triple & 0x3F];
    }
    if (i < len) {
        const size_t rem = len - i;  // 1 or 2
        uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        if (rem == 2) triple |= static_cast<uint32_t>(data[i + 1]) << 8;
        result += kChars[(triple >> 18) & 0x3F];
        result += kChars[(triple >> 12) & 0x3F];
        if (rem == 2) {
            result += kChars[(triple >> 6) & 0x3F];
        }
        // No '=' padding for base64url.
    }
    return result;
}

/// @brief Convenience overload for byte spans.
[[nodiscard]] inline std::string base64url_encode(
    std::span<const uint8_t> bytes) {
    return base64url_encode(bytes.data(), bytes.size());
}

/// @brief Convenience overload for string views (no allocation versus the
///        span overload — same backing path).
[[nodiscard]] inline std::string base64url_encode(std::string_view sv) {
    return base64url_encode(
        reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
}

/// @brief Decimal render of a uint64_t into a string. Matches the helper
///        in signed_request.hpp::detail but kept inline here so this
///        header has no extra coupling.
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

/// @brief Hex-render `bytes` as 2*N lowercase ASCII chars (no '0x', no spacing).
///
/// Used for the JWT header's `nonce` claim — Coinbase docs say nonce is
/// "a unique identifier"; using 32 random bytes hex-encoded matches the
/// reference Python implementation `secrets.token_hex(16)` (which
/// produces 32 hex chars from 16 random bytes). We use 32 bytes (64 hex
/// chars) for extra collision resistance — Coinbase accepts any string
/// here.
[[nodiscard]] inline std::string hex_lower(
    std::span<const uint8_t> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s(bytes.size() * 2, '\0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        s[i * 2]     = kHex[bytes[i] >> 4];
        s[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    return s;
}

}  // namespace detail

// ────────────────────────────────────────────────────────────────────────────
// Es256PrivateKey — RAII over EVP_PKEY (P-256)
// ────────────────────────────────────────────────────────────────────────────

/// @brief Owning handle for an ES256 (ECDSA P-256) private key.
///
/// The underlying `void*` is in fact an `EVP_PKEY*`. We keep it opaque
/// in the field declaration to (a) avoid pulling additional aws-lc
/// typedefs into call sites that include this header for the
/// `from_pem()` API only, and (b) document that the public ABI is
/// the ownership semantics, not the concrete openssl handle.
///
/// Move-only: copying a private key is almost never what the caller
/// wants; non-copyable forces an explicit decision (re-load from PEM
/// or share via reference).
class Es256PrivateKey {
public:
    /// @brief Load a P-256 private key from a PEM-encoded string.
    ///
    /// Accepts both the modern PKCS#8 PEM (`-----BEGIN PRIVATE KEY-----`)
    /// and the legacy SEC1 PEM (`-----BEGIN EC PRIVATE KEY-----`) — the
    /// `PEM_read_bio_PrivateKey` aws-lc API auto-detects via the PEM
    /// label. Coinbase Cloud's API key download uses PKCS#8 today; we
    /// support both for defensive compatibility.
    ///
    /// Validates that the parsed key is actually EC P-256 (not RSA,
    /// not Ed25519, not P-384) — anything else returns
    /// `Error::InvalidConfig` because Coinbase Advanced Trade only
    /// accepts ES256.
    [[nodiscard]] static std::expected<Es256PrivateKey, ::eph::core::ErrorInfo>
    from_pem(std::string_view pem) noexcept {
        if (pem.empty()) {
            EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "Es256PrivateKey::from_pem: empty PEM");
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig, "empty PEM"});
        }

        // BIO_new_mem_buf is read-only over the supplied buffer — no copy.
        // The cast to int is safe because PEM blobs are tiny (a few KB).
        BIO* bio = BIO_new_mem_buf(
            pem.data(), static_cast<int>(pem.size()));
        if (bio == nullptr) {
            EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "Es256PrivateKey::from_pem: BIO_new_mem_buf failed");
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::OutOfMemory, "BIO alloc"});
        }

        // PEM_read_bio_PrivateKey is the modern unified entry; it accepts
        // either PKCS#8 (`BEGIN PRIVATE KEY`) or the legacy SEC1
        // (`BEGIN EC PRIVATE KEY`) — aws-lc dispatches on the label.
        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(
            bio, /*x=*/nullptr, /*cb=*/nullptr, /*u=*/nullptr);
        BIO_free(bio);

        if (pkey == nullptr) {
            EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "Es256PrivateKey::from_pem: PEM_read_bio_PrivateKey "
                         "returned null (malformed PEM, encrypted PEM with "
                         "no callback, or unrecognized key type)");
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "PEM parse failed"});
        }

        // Validate algorithm: must be EC.
        const int id = EVP_PKEY_id(pkey);
        if (id != EVP_PKEY_EC) {
            EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "Es256PrivateKey::from_pem: key is not EC "
                         "(EVP_PKEY_id={})", id);
            EVP_PKEY_free(pkey);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "key is not ECDSA"});
        }

        // Validate curve: must be P-256 (NID_X9_62_prime256v1).
        const EC_KEY* ec = EVP_PKEY_get0_EC_KEY(pkey);
        if (ec == nullptr) {
            EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "Es256PrivateKey::from_pem: EVP_PKEY_get0_EC_KEY "
                         "returned null");
            EVP_PKEY_free(pkey);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "EC_KEY extract failed"});
        }
        const EC_GROUP* group = EC_KEY_get0_group(ec);
        if (group == nullptr ||
            EC_GROUP_get_curve_name(group) != NID_X9_62_prime256v1) {
            EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "Es256PrivateKey::from_pem: key is EC but not "
                         "P-256 (curve_nid={})",
                         group ? EC_GROUP_get_curve_name(group) : 0);
            EVP_PKEY_free(pkey);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "key is not P-256"});
        }

        EPH_LOG_DEBUG(detail::jwt_signed_request_logger(), "Es256PrivateKey::from_pem: loaded P-256 key OK");
        Es256PrivateKey out;
        out.key_ = pkey;
        return out;
    }

    Es256PrivateKey() noexcept : key_(nullptr) {}
    Es256PrivateKey(const Es256PrivateKey&)            = delete;
    Es256PrivateKey& operator=(const Es256PrivateKey&) = delete;

    Es256PrivateKey(Es256PrivateKey&& other) noexcept : key_(other.key_) {
        other.key_ = nullptr;
    }

    Es256PrivateKey& operator=(Es256PrivateKey&& other) noexcept {
        if (this != &other) {
            free_key_();
            key_ = other.key_;
            other.key_ = nullptr;
        }
        return *this;
    }

    ~Es256PrivateKey() noexcept { free_key_(); }

    /// @brief Read-only access to the underlying `EVP_PKEY*`. Returned as
    ///        `void*` to keep the public field type opaque; cast back to
    ///        `EVP_PKEY*` at the use site (the `build_coinbase_jwt` path
    ///        is the only consumer in the public API).
    [[nodiscard]] void* native_handle() const noexcept { return key_; }

    /// @brief Truthy when the handle holds a loaded key. False after a
    ///        moved-from state or default-constructed instance.
    [[nodiscard]] explicit operator bool() const noexcept {
        return key_ != nullptr;
    }

private:
    void free_key_() noexcept {
        if (key_ != nullptr) {
            EVP_PKEY_free(static_cast<EVP_PKEY*>(key_));
            key_ = nullptr;
        }
    }

    /// @brief Opaque `EVP_PKEY*`. See class docstring for rationale.
    void* key_;
};

// ────────────────────────────────────────────────────────────────────────────
// Coinbase JWT params + builder
// ────────────────────────────────────────────────────────────────────────────

/// @brief Inputs to `build_coinbase_jwt`. Every field is a view into
///        caller-owned storage that must remain valid for the duration
///        of the call (and not longer — the function copies into the
///        returned string).
struct CoinbaseJwtParams {
    std::string_view key_id;          ///< the "kid" header claim (Cloud API key id)
    std::string_view api_key_name;    ///< the "sub" payload claim (Cloud "name")
    std::string_view method;          ///< HTTP method, e.g. "GET" / "POST"
    std::string_view uri;             ///< host + path, e.g. "exchange.coinbase.com/v3/brokerage/accounts"
    uint64_t         now_unix_secs;   ///< current wall-clock unix seconds (caller-supplied)
    uint64_t         ttl_secs = 120;  ///< token validity window; default 120s per Coinbase docs

    /// @brief Optional caller-supplied nonce. Empty (default) → the
    ///        builder pulls `kCoinbaseJwtNonceLen` fresh CSPRNG bytes
    ///        via aws-lc `RAND_bytes`. Non-empty: the caller's bytes
    ///        are used verbatim (mainly for deterministic tests) AND
    ///        MUST be exactly `kCoinbaseJwtNonceLen` (32) bytes —
    ///        anything else is rejected with `InvalidConfig` at build
    ///        time. See `kCoinbaseJwtNonceLen` for rationale.
    std::span<const uint8_t> nonce_override = {};
};

/// @brief Coinbase Advanced Trade documents `exp - nbf` MUST NOT exceed
///        120 seconds. Tokens with longer windows are rejected at the
///        venue with a 401 that carries no machine-readable hint of
///        WHY — surfacing the cap up-front saves operators a 30-minute
///        chase. Default `CoinbaseJwtParams::ttl_secs` (120) hits this
///        limit exactly.
inline constexpr uint64_t kCoinbaseJwtTtlSecsMax = 120;

/// @brief Coinbase requires `nbf < exp` strictly; `ttl_secs == 0` would
///        mint a token whose nbf == exp and the venue rejects it. We
///        enforce ≥ 1 to keep this representable.
inline constexpr uint64_t kCoinbaseJwtTtlSecsMin = 1;

/// @brief Required byte length of the JWT nonce. RFC 7519 itself does not
///        prescribe a length, but Coinbase Advanced Trade's documented
///        nonce field is a 32-byte (256-bit) random value, hex-encoded
///        into the JWT header. Wrong-sized overrides (e.g. a 16-byte
///        legacy nonce, or an accidentally-truncated test fixture)
///        produce a malformed header that the venue may either reject or
///        — worse — silently accept while logging the request as
///        suspicious. Catching the size mismatch up-front turns this
///        latent foot-gun into an `InvalidConfig` at build time.
inline constexpr size_t kCoinbaseJwtNonceLen = 32;

namespace detail {

/// @brief Convert a DER-encoded ECDSA signature to fixed-width
///        IEEE P-1363 r||s (32 + 32 = 64 bytes for P-256).
///
/// ## The byte layout
///
/// `EVP_DigestSignFinal` for ECDSA produces a DER `SEQUENCE { INTEGER r,
/// INTEGER s }`. The lengths are variable (typically 70-72 bytes for
/// P-256, occasionally 71 when one of r / s is missing a leading
/// zero-byte for non-negative-integer encoding). JOSE (RFC 7518 §3.4)
/// mandates the alternative "concatenation" form: r as a fixed
/// big-endian 32-byte integer, s likewise, no length / tag bytes.
///
/// ```
///   DER:    30 46  02 21 00 <r 32B>  02 21 00 <s 32B>
///   P-1363:                  <r 32B>          <s 32B>
/// ```
///
/// ## Why this matters
///
/// Skipping this conversion is the single most common JWT-against-
/// Coinbase bug. The local JWT decoder will happily parse a
/// DER-shaped signature (it's just bytes after all) but the venue
/// rejects the token because the OAuth / JOSE spec is unambiguous
/// about the wire format.
///
/// We use `BN_bn2binpad` because `BN_bn2bin` does NOT zero-pad the
/// front when r or s happens to start with leading zeroes (perfectly
/// valid for short BIGNUMs) — that would silently produce a 31- or
/// 30-byte segment and the caller would have to reach for arithmetic
/// they shouldn't have to write.
///
/// @return P-1363 r||s bytes on success, or an ErrorInfo on parse /
///         padding failure (which only occurs on internal aws-lc
///         malfunction; we surface it for completeness).
[[nodiscard]] inline std::expected<std::array<uint8_t, 64>, ::eph::core::ErrorInfo>
ecdsa_der_to_p1363_(const uint8_t* der, size_t der_len) noexcept {
    ECDSA_SIG* sig = ECDSA_SIG_from_bytes(der, der_len);
    if (sig == nullptr) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "ecdsa_der_to_p1363_: ECDSA_SIG_from_bytes failed "
                     "(der_len={})", der_len);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "ECDSA DER parse failed"});
    }

    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(sig, &r, &s);

    std::array<uint8_t, 64> out{};
    // BN_bn2binpad pads with leading zeros if r or s is shorter than
    // 32 bytes (which is normal for P-256 — uniform random BIGNUMs
    // shorter than 32 bytes occur with probability ~2^-8). It returns
    // the number of bytes written, or -1 on overflow (BIGNUM larger
    // than the requested width — should never happen for P-256 r/s).
    const int wr = BN_bn2binpad(r, out.data(), 32);
    const int ws = BN_bn2binpad(s, out.data() + 32, 32);
    ECDSA_SIG_free(sig);

    if (wr != 32 || ws != 32) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "ecdsa_der_to_p1363_: BN_bn2binpad unexpected width "
                     "(wr={}, ws={})", wr, ws);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "BN_bn2binpad failed"});
    }
    return out;
}

}  // namespace detail

/// @brief Build a Coinbase Advanced Trade JWT.
///
/// Produces the three-part `header.payload.signature` string per RFC
/// 7519 / Coinbase's documented format:
///
///   * header  = `{"alg":"ES256","typ":"JWT","kid":"<key_id>","nonce":"<hex>"}`
///   * payload = `{"iss":"coinbase-cloud","nbf":<now>,"exp":<now+ttl>,
///                 "sub":"<api_key_name>","uri":"<METHOD requestPath>"}`
///   * signature = base64url( ES256( header_b64u + "." + payload_b64u ) )
///
/// JSON construction is hand-rolled (the field set is fixed) — we
/// escape `kid` and `sub` via `json_escape` because Coinbase Cloud key
/// names contain `/` and the JSON spec requires escape on `\` and `"`.
/// `method`, `uri`, and the integer claims are all ASCII-safe by
/// construction.
[[nodiscard]] inline std::expected<std::string, ::eph::core::ErrorInfo>
build_coinbase_jwt(const Es256PrivateKey&    key,
                   const CoinbaseJwtParams&  p) noexcept {
    if (!key) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: null key");
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig, "null key"});
    }
    if (p.method.empty() || p.uri.empty() ||
        p.key_id.empty() || p.api_key_name.empty()) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: missing required field "
                     "(method.empty={}, uri.empty={}, kid.empty={}, "
                     "sub.empty={})",
                     p.method.empty(), p.uri.empty(),
                     p.key_id.empty(), p.api_key_name.empty());
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "missing required JWT param"});
    }

    // ttl_secs bounds — Coinbase docs say `exp - nbf` MUST be in (0, 120]s.
    // ttl=0 would generate `exp == nbf` which the venue rejects strictly;
    // ttl > 120 is the most common silent JWT failure (401 without a
    // useful message). Surface both up-front rather than after the round
    // trip so debugging is local. We also reject the obviously-wrong
    // now_unix_secs == 0 path: any modern unix epoch second is > 1.5e9,
    // so 0 means the caller forgot to populate the field. A bogus now
    // would produce `nbf=0,exp=<ttl>`, which the venue accepts as a
    // pre-1970-validity token only if it's ALSO past 1970, i.e. never —
    // but the failure mode is identical to the ttl issue (silent 401).
    if (p.ttl_secs < kCoinbaseJwtTtlSecsMin ||
        p.ttl_secs > kCoinbaseJwtTtlSecsMax) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: ttl_secs={} out of range "
                     "[{}, {}] per Coinbase docs",
                     p.ttl_secs, kCoinbaseJwtTtlSecsMin,
                     kCoinbaseJwtTtlSecsMax);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "ttl_secs out of [1, 120] (Coinbase exp - nbf cap)"});
    }
    if (p.now_unix_secs == 0) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: now_unix_secs == 0 (caller "
                     "forgot to populate? — venue would reject token "
                     "with nbf=0,exp=ttl as past-validity)");
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "now_unix_secs is 0 (uninitialized?)"});
    }
    // Defensive overflow guard: now + ttl must not wrap. ttl is bounded
    // above by 120, so the only way this fires is if now_unix_secs is
    // implausibly close to UINT64_MAX (year ~5.8e11) — but the cost is
    // a single comparison and the failure mode otherwise is wraparound
    // to an "expired in the past" exp claim, which is the exact bug
    // class we're trying to surface.
    if (p.now_unix_secs > UINT64_MAX - p.ttl_secs) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: now_unix_secs={} + ttl_secs={} "
                     "would overflow uint64_t",
                     p.now_unix_secs, p.ttl_secs);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::InvalidConfig,
            "now_unix_secs + ttl_secs overflow"});
    }

    // ── 1) Resolve nonce: caller-supplied or fresh CSPRNG ─────────────────
    std::array<uint8_t, kCoinbaseJwtNonceLen> nonce_buf{};
    std::span<const uint8_t> nonce_bytes;
    if (!p.nonce_override.empty()) {
        // Reject wrong-sized nonces up-front: hex-encoding any size
        // works mechanically, but a 16-byte (legacy) or 64-byte
        // (over-padded) override produces a header field of the wrong
        // length, which the venue may either reject as malformed or
        // accept while flagging the request — both surfaces are
        // confusing to debug. See `kCoinbaseJwtNonceLen` doc.
        if (p.nonce_override.size() != kCoinbaseJwtNonceLen) {
            EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: nonce_override size={} "
                         "(must be exactly {} bytes)",
                         p.nonce_override.size(), kCoinbaseJwtNonceLen);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "nonce_override wrong size (must be 32 bytes)"});
        }
        nonce_bytes = p.nonce_override;
    } else {
        // RAND_bytes returns 1 on success, 0 on failure. aws-lc seeds from
        // /dev/urandom (or getrandom(2) on Linux ≥ 3.17) at process start
        // so this should never fail in practice; surface failure anyway.
        if (RAND_bytes(nonce_buf.data(), nonce_buf.size()) != 1) {
            EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: RAND_bytes failed");
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::OutOfMemory, "CSPRNG failed"});
        }
        nonce_bytes = std::span<const uint8_t>{nonce_buf.data(),
                                                nonce_buf.size()};
    }
    const std::string nonce_hex = detail::hex_lower(nonce_bytes);

    // ── 2) Build header JSON and base64url-encode ────────────────────────
    // Coinbase docs example (current as of 2026-01):
    //   {"alg":"ES256","typ":"JWT","kid":"organizations/.../apiKeys/...",
    //    "nonce":"<hex>"}
    std::string header;
    header.reserve(96 + p.key_id.size() + nonce_hex.size());
    header.append(R"({"alg":"ES256","typ":"JWT","kid":")");
    header.append(::eph::core::detail::json_escape(p.key_id));
    header.append(R"(","nonce":")");
    header.append(nonce_hex);
    header.append(R"("})");

    // ── 3) Build payload JSON ────────────────────────────────────────────
    // Coinbase requires:
    //   iss = "coinbase-cloud" (literal)
    //   nbf = now_unix_secs
    //   exp = now_unix_secs + ttl_secs  (max 120s per docs)
    //   sub = api_key_name
    //   uri = "<METHOD> <host/path>"   (note the space delimiter)
    const uint64_t exp = p.now_unix_secs + p.ttl_secs;
    const std::string nbf_str = detail::u64_to_decimal(p.now_unix_secs);
    const std::string exp_str = detail::u64_to_decimal(exp);

    // The Coinbase docs spell out the uri claim as "METHOD requestPath",
    // single space, no question mark, no scheme. We do not validate
    // p.uri's shape — that's the caller's responsibility.
    std::string uri_claim;
    uri_claim.reserve(p.method.size() + 1 + p.uri.size());
    uri_claim.append(p.method);
    uri_claim.push_back(' ');
    uri_claim.append(p.uri);

    std::string payload;
    payload.reserve(64 + p.api_key_name.size() + uri_claim.size() +
                    nbf_str.size() + exp_str.size());
    payload.append(R"({"iss":"coinbase-cloud","nbf":)");
    payload.append(nbf_str);
    payload.append(R"(,"exp":)");
    payload.append(exp_str);
    payload.append(R"(,"sub":")");
    payload.append(::eph::core::detail::json_escape(p.api_key_name));
    payload.append(R"(","uri":")");
    payload.append(::eph::core::detail::json_escape(uri_claim));
    payload.append(R"("})");

    const std::string header_b64u = detail::base64url_encode(header);
    const std::string payload_b64u = detail::base64url_encode(payload);

    // ── 4) Compute the ES256 signature over "header_b64u.payload_b64u" ──
    //
    // EVP_DigestSign with EVP_sha256() and a P-256 EVP_PKEY produces the
    // DER-encoded ECDSA signature (variable length, 70-72 bytes typical).
    // We then convert to fixed-width P-1363 r||s (64 bytes) for JOSE.
    std::string signing_input;
    signing_input.reserve(header_b64u.size() + 1 + payload_b64u.size());
    signing_input.append(header_b64u);
    signing_input.push_back('.');
    signing_input.append(payload_b64u);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: EVP_MD_CTX_new failed");
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::OutOfMemory, "EVP_MD_CTX alloc"});
    }

    auto* pkey = static_cast<EVP_PKEY*>(key.native_handle());

    // EVP_DigestSignInit(ctx, pctx_out, md, engine, pkey)
    if (EVP_DigestSignInit(ctx, /*pctx=*/nullptr, EVP_sha256(),
                           /*engine=*/nullptr, pkey) != 1) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: EVP_DigestSignInit failed");
        EVP_MD_CTX_free(ctx);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::TlsCipherFailed, "DigestSignInit"});
    }

    // Two-call EVP_DigestSign convention: first call with out=NULL to
    // obtain the maximum DER length, then call again with the buffer.
    // We use the one-shot EVP_DigestSign which handles the buffer size
    // probe internally.
    size_t sig_der_len = 0;
    if (EVP_DigestSign(ctx, /*out_sig=*/nullptr, &sig_der_len,
                       reinterpret_cast<const uint8_t*>(signing_input.data()),
                       signing_input.size()) != 1) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: EVP_DigestSign size-probe failed");
        EVP_MD_CTX_free(ctx);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::TlsCipherFailed, "DigestSign size probe"});
    }
    // Defensive upper bound — DER ECDSA signatures over P-256 fit in
    // 72 bytes (sequence/length 2B + two integers each up to 33B
    // including the optional leading zero byte for sign + length tag).
    if (sig_der_len == 0 || sig_der_len > 80) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: implausible DER sig length {}",
                     sig_der_len);
        EVP_MD_CTX_free(ctx);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::TlsCipherFailed,
            "DigestSign size out of range"});
    }

    std::array<uint8_t, 80> der_buf{};
    if (EVP_DigestSign(ctx, der_buf.data(), &sig_der_len,
                       reinterpret_cast<const uint8_t*>(signing_input.data()),
                       signing_input.size()) != 1) {
        EPH_LOG_ERROR(detail::jwt_signed_request_logger(), "build_coinbase_jwt: EVP_DigestSign failed");
        EVP_MD_CTX_free(ctx);
        return std::unexpected(::eph::core::ErrorInfo{
            ::eph::core::Error::TlsCipherFailed, "DigestSign"});
    }
    EVP_MD_CTX_free(ctx);

    auto p1363 = detail::ecdsa_der_to_p1363_(der_buf.data(), sig_der_len);
    if (!p1363) {
        return std::unexpected(p1363.error());
    }
    const std::string sig_b64u = detail::base64url_encode(
        std::span<const uint8_t>{p1363->data(), p1363->size()});

    // ── 5) Concatenate → "header.payload.signature" ─────────────────────
    std::string jwt;
    jwt.reserve(header_b64u.size() + payload_b64u.size() + sig_b64u.size() + 2);
    jwt.append(header_b64u);
    jwt.push_back('.');
    jwt.append(payload_b64u);
    jwt.push_back('.');
    jwt.append(sig_b64u);

    EPH_LOG_DEBUG(detail::jwt_signed_request_logger(), "build_coinbase_jwt: produced JWT ({} bytes, kid={}, "
                 "sub={}, method={})",
                 jwt.size(), p.key_id, p.api_key_name, p.method);
    return jwt;
}

// ────────────────────────────────────────────────────────────────────────────
// Compile-time guarantees
// ────────────────────────────────────────────────────────────────────────────

static_assert(!std::is_copy_constructible_v<Es256PrivateKey>,
              "Es256PrivateKey must be non-copyable");
static_assert(std::is_nothrow_move_constructible_v<Es256PrivateKey>,
              "Es256PrivateKey move must be noexcept");
static_assert(std::is_nothrow_destructible_v<Es256PrivateKey>,
              "Es256PrivateKey destructor must be noexcept");

}  // namespace eph::net
