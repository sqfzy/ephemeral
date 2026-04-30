/// @file test_jwt_signed_request.cpp
/// @brief Unit tests for `eph::net::Es256PrivateKey` and `build_coinbase_jwt`.
///
/// ## Test approach
///
/// Coinbase Advanced Trade does not publish a static reference vector
/// (the JWT inputs include a per-token `nonce` and the `nbf`/`exp`
/// claims, all of which differ across calls). The standard way to
/// validate a JWT implementation is therefore to round-trip-verify
/// against the matching public key — if `EVP_DigestVerify` accepts the
/// signature we produced, the math is correct.
///
/// We exercise both halves of the public surface:
///
///   * `Es256PrivateKey::from_pem`: happy path (valid P-256 PEM),
///     unhappy paths (RSA PEM, garbage bytes).
///   * `build_coinbase_jwt`: structural checks (3 base64url-decoded
///     parts, header / payload claims), wire-format checks (signature
///     is exactly 64 bytes — i.e. P-1363 form, not the DER form some
///     JWT libraries silently emit), cryptographic correctness
///     (round-trip `EVP_DigestVerify`), and CSPRNG wiring (two default
///     calls → two different tokens).

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ec_key.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>

#include "eph/core/error.hpp"
#include "eph/net/jwt_signed_request.hpp"

using eph::core::Error;
using eph::net::CoinbaseJwtParams;
using eph::net::Es256PrivateKey;
using eph::net::build_coinbase_jwt;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Test vectors — embedded PEM blobs
// ─────────────────────────────────────────────────────────────────────────────

// Generated via `openssl ecparam -genkey -name prime256v1 -noout`. This is
// a throwaway P-256 key used only for self-roundtrip tests; it has no
// production lifetime and is committed deliberately.
constexpr std::string_view kEcP256PemValid =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEINOpmaBE6cuLkglCisJtB93Y4yJ2RGC4HSHdUJZesfueoAoGCCqGSM49\n"
    "AwEHoUQDQgAEipxCkur6xELXaT83IyfmFcCIETWrJCXZRs+en43AvHt+Lu0i15E9\n"
    "90z0OphnjtVyoeuhbuMPChrOEkZbGyZUMw==\n"
    "-----END EC PRIVATE KEY-----\n";

// Generated via `openssl genrsa 2048`. Used only to verify that
// from_pem() correctly rejects non-EC keys.
constexpr std::string_view kRsa2048Pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDGYcM/EhPk6hrd\n"
    "sTiPhf1svlJoHm0M0wTUKmvLM15Faei2BmxeLedYwGEcyKoCRkeFLKrwxuJVAN2p\n"
    "fBrsBJ6IYJCJwGRpSFJVbD1k4ehdBfIXFqO/0LE0numq3dJFqxv9S++p/3/FcV0K\n"
    "8N4RYoPFZ9RNOYVfkzwrrWcJaVu9/B5ueHhUZX9A/unIbzo+G/Jf9zNG/lhYVAeX\n"
    "wDtAZd9ovYhIe6+A/L/QMpMq7/Kbkxu8AJpgtwFKr/IUZiSkSGTyRqwY9t4/KJl8\n"
    "06NrUvtLuhIL6h4TJeYuHexrHdjWwFBE6MzgkzJCdPGeRVFyEoq/btZlx07wdP3D\n"
    "3yk/cALhAgMBAAECggEAJeZaf0usWlDxVgY8CItwKZzIsJSTf942r3P1SQpkyb0c\n"
    "lN3wSSPa7WU/iFi7xhh4JHSuqbZNWjECqBUKLaoKQYK1SmPjqwuCk0hNCF9yXYc7\n"
    "w3ZzTTuJB0UO3jJnsCCrBb7CqEckOWvZezQeMNMR2p5l/GTvWp2N4shLhYxH1ykj\n"
    "3SCRMM5OepaLCQ7QSx6xOzeLP+rrkiy/epcKUUX0fjy57W0z4/VxxFufJ4cGLsVJ\n"
    "LHUEnlKvOXD3Jer+uFRNAq29DWzji3cHJQiSAdfWdnPJIRuzvgAvY/t4S8R+m6ze\n"
    "Rs5KCCN0A6msM8vRlK0vmmBx4NMZXPO1wq+8xxB+mQKBgQDpETyoRoFQIRurj2e0\n"
    "Lij/buKq4lNk+a9scb9lkkaQ5GnY30CePh51ivtbMAlcoMbJJ5P/JdJJJX4DYUe1\n"
    "Gt0cEGFtgbnwHyufGsw10KqFTzpVC3X/LNbsmc3W2a3oZtNPkbWxKCaUwNzCqO0i\n"
    "ib1sWUS2S7XlnZVHwAihbi7dXQKBgQDZ5tReFpdkPIXCYF8ZVE/kNj6264I98j1R\n"
    "TJM7uNnisEMJ1RqH1lJHP5MisTfd0pICqOheBtx32+k5E3rsJsEU03xhS5j12LM5\n"
    "tFJX2dIDF3kXba6cYryphysply/fbBpQJsMA7UIdOQQSbQxB/pKi7oqgpKzSOSkz\n"
    "lDCrZfdfVQKBgAJE1rBUr2GWUOykor/QSznhXHeJaIJtI9YMbW6Rs/opHxarZbek\n"
    "pytBxRyoJQ8vyX+f0QME3T01Djr+MXKD5m8lga1NPAAobYZI/n/vnhlaIhk92VI4\n"
    "n4cCIEzdJaJDjf8SThCBcY61KfEDL/vMF3n8jHyx0/1+QTvHlM3tgqtJAoGAKGg2\n"
    "r2/vCQZ0I3RtjivlWMN6Y79OeqBGIKJblzKTLQdUlykub0weG9o9Nay5WGgo7VdX\n"
    "J8CL96oPGKd1Hv+cxHjnUr+LEOPrcGLw9huNZ0deDCspuxRQOfu31FGV7g+E3aIi\n"
    "fTSCExs0lxojsMU9eftUN6/x4FX7PvZXBCG3erUCgYBhIHvUq04+zyLtVsCZ/A8y\n"
    "vPZyDNpsBZTb49ifqjmPdiv3kVoPztEHCh/Iwp2gtvP7dxo9uABDgJDP8BNSX5SC\n"
    "SjNGmi6knrRsjWRRWe/csQFlAKv6n2Y/XUIiOpwf+GHjYTUeyh3l4s0lBLowCDF6\n"
    "wsn7FOHib9rv8D5XvCBBgg==\n"
    "-----END PRIVATE KEY-----\n";

// ─────────────────────────────────────────────────────────────────────────────
// Helpers — base64url decode + JWT split (test-only, no perf concerns)
// ─────────────────────────────────────────────────────────────────────────────

/// Decode a base64url string (no padding, RFC 7515 alphabet) → bytes.
/// Returns an empty vector on malformed input — sufficient for the test
/// asserts.
std::vector<uint8_t> base64url_decode(std::string_view in) {
    auto from_alpha = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
        if (c >= '0' && c <= '9') return 52 + (c - '0');
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve((in.size() * 3) / 4 + 1);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        int v = from_alpha(c);
        if (v < 0) return {};  // bad char
        buf = (buf << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

/// Split a JWT on the two '.' separators. Returns three views into `s`.
struct JwtParts {
    std::string_view header_b64;
    std::string_view payload_b64;
    std::string_view sig_b64;
};
bool split_jwt(std::string_view s, JwtParts& out) {
    auto p1 = s.find('.');
    if (p1 == std::string_view::npos) return false;
    auto p2 = s.find('.', p1 + 1);
    if (p2 == std::string_view::npos) return false;
    out.header_b64 = s.substr(0, p1);
    out.payload_b64 = s.substr(p1 + 1, p2 - p1 - 1);
    out.sig_b64 = s.substr(p2 + 1);
    return true;
}

/// Crude string contains check — saves dragging in a JSON parser when
/// the JWT bodies are tiny and field-shaped.
bool contains(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// PEM loading
// ────────────────────────────────────────────────────────────────────────────

TEST(Es256PrivateKey, PemLoadHappyPath) {
    auto key = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key.has_value())
        << "from_pem failed for known-good P-256 key: code="
        << static_cast<int>(key.error().code)
        << " detail=" << key.error().detail;
    EXPECT_TRUE(static_cast<bool>(*key));
    EXPECT_NE(key->native_handle(), nullptr);
}

TEST(Es256PrivateKey, PemLoadRejectsRsa) {
    auto key = Es256PrivateKey::from_pem(kRsa2048Pem);
    ASSERT_FALSE(key.has_value()) << "RSA key was accepted as ES256";
    EXPECT_EQ(key.error().code, Error::InvalidConfig);
}

TEST(Es256PrivateKey, PemLoadRejectsGarbage) {
    constexpr std::string_view garbage =
        "this is not a PEM file at all, just some bytes";
    auto key = Es256PrivateKey::from_pem(garbage);
    ASSERT_FALSE(key.has_value());
    EXPECT_EQ(key.error().code, Error::InvalidConfig);
}

TEST(Es256PrivateKey, PemLoadRejectsEmpty) {
    auto key = Es256PrivateKey::from_pem("");
    ASSERT_FALSE(key.has_value());
    EXPECT_EQ(key.error().code, Error::InvalidConfig);
}

TEST(Es256PrivateKey, MoveTransfersOwnership) {
    auto key1_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key1_exp.has_value());
    Es256PrivateKey key1 = std::move(*key1_exp);
    EXPECT_TRUE(static_cast<bool>(key1));

    Es256PrivateKey key2 = std::move(key1);
    EXPECT_TRUE(static_cast<bool>(key2));
    // Moved-from key1 is reset.
    EXPECT_FALSE(static_cast<bool>(key1));
    EXPECT_EQ(key1.native_handle(), nullptr);
}

// ────────────────────────────────────────────────────────────────────────────
// JWT structural / round-trip
// ────────────────────────────────────────────────────────────────────────────

TEST(BuildCoinbaseJwt, JwtRoundtripDecodes) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    CoinbaseJwtParams p{
        .key_id        = "organizations/abc/apiKeys/xyz",
        .api_key_name  = "my-trading-bot/v1",
        .method        = "GET",
        .uri           = "exchange.coinbase.com/v3/brokerage/accounts",
        .now_unix_secs = 1714867200,
        .ttl_secs      = 120,
    };

    auto jwt = build_coinbase_jwt(*key_exp, p);
    ASSERT_TRUE(jwt.has_value())
        << "build_coinbase_jwt returned error: "
        << jwt.error().detail;

    JwtParts parts{};
    ASSERT_TRUE(split_jwt(*jwt, parts)) << "JWT did not have 3 parts: " << *jwt;

    auto header_bytes = base64url_decode(parts.header_b64);
    auto payload_bytes = base64url_decode(parts.payload_b64);
    ASSERT_FALSE(header_bytes.empty());
    ASSERT_FALSE(payload_bytes.empty());

    std::string header_json(header_bytes.begin(), header_bytes.end());
    std::string payload_json(payload_bytes.begin(), payload_bytes.end());

    // Header structure: ES256 + JWT + kid + nonce
    EXPECT_TRUE(contains(header_json, R"("alg":"ES256")")) << header_json;
    EXPECT_TRUE(contains(header_json, R"("typ":"JWT")")) << header_json;
    EXPECT_TRUE(contains(header_json,
        R"("kid":"organizations/abc/apiKeys/xyz")")) << header_json;
    EXPECT_TRUE(contains(header_json, R"("nonce":")")) << header_json;

    // Payload structure: iss + nbf/exp + sub + uri ("METHOD path")
    EXPECT_TRUE(contains(payload_json, R"("iss":"coinbase-cloud")"))
        << payload_json;
    EXPECT_TRUE(contains(payload_json, R"("nbf":1714867200)")) << payload_json;
    EXPECT_TRUE(contains(payload_json, R"("exp":1714867320)")) << payload_json;
    EXPECT_TRUE(contains(payload_json, R"("sub":"my-trading-bot/v1")"))
        << payload_json;
    EXPECT_TRUE(contains(payload_json,
        R"("uri":"GET exchange.coinbase.com/v3/brokerage/accounts")"))
        << payload_json;
}

TEST(BuildCoinbaseJwt, SignatureIs64BytesP1363) {
    // P-1363 form mandates exactly 32 + 32 = 64 bytes. This catches
    // the very common "we sent the DER blob as the JWT signature" bug.
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    CoinbaseJwtParams p{
        .key_id        = "kid-x",
        .api_key_name  = "name-x",
        .method        = "POST",
        .uri           = "exchange.coinbase.com/v3/brokerage/orders",
        .now_unix_secs = 1714867200,
    };
    auto jwt = build_coinbase_jwt(*key_exp, p);
    ASSERT_TRUE(jwt.has_value());

    JwtParts parts{};
    ASSERT_TRUE(split_jwt(*jwt, parts));
    auto sig = base64url_decode(parts.sig_b64);
    EXPECT_EQ(sig.size(), 64u)
        << "expected 64-byte P-1363 sig, got " << sig.size()
        << " (signature wire form is incorrect — this is the JWT/JOSE pitfall)";
}

TEST(BuildCoinbaseJwt, NonceCsprngVariesAcrossCalls) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    CoinbaseJwtParams p{
        .key_id        = "kid",
        .api_key_name  = "name",
        .method        = "GET",
        .uri           = "host/path",
        .now_unix_secs = 100,
    };
    auto a = build_coinbase_jwt(*key_exp, p);
    auto b = build_coinbase_jwt(*key_exp, p);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    // Two tokens with default nonce must differ — both header.nonce
    // and the signature differ across calls when CSPRNG is wired.
    EXPECT_NE(*a, *b);
}

TEST(BuildCoinbaseJwt, NonceOverrideIsDeterministic) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    std::array<uint8_t, 32> fixed_nonce{};
    for (size_t i = 0; i < fixed_nonce.size(); ++i)
        fixed_nonce[i] = static_cast<uint8_t>(i + 1);

    CoinbaseJwtParams p{
        .key_id        = "kid",
        .api_key_name  = "name",
        .method        = "GET",
        .uri           = "host/path",
        .now_unix_secs = 100,
        .ttl_secs      = 120,
        .nonce_override = std::span<const uint8_t>(fixed_nonce.data(),
                                                    fixed_nonce.size()),
    };

    auto a = build_coinbase_jwt(*key_exp, p);
    auto b = build_coinbase_jwt(*key_exp, p);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    // Header + payload are deterministic when nonce is fixed and time
    // is fixed; the signature varies because ECDSA's `k` is randomized
    // (RFC 6979 deterministic ECDSA is NOT the default in aws-lc).
    JwtParts pa{}, pb{};
    ASSERT_TRUE(split_jwt(*a, pa));
    ASSERT_TRUE(split_jwt(*b, pb));
    EXPECT_EQ(pa.header_b64, pb.header_b64);
    EXPECT_EQ(pa.payload_b64, pb.payload_b64);
}

TEST(BuildCoinbaseJwt, RejectsEmptyRequiredFields) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    CoinbaseJwtParams base{
        .key_id        = "kid",
        .api_key_name  = "name",
        .method        = "GET",
        .uri           = "host/path",
        .now_unix_secs = 100,
    };

    auto p1 = base; p1.method = "";
    EXPECT_FALSE(build_coinbase_jwt(*key_exp, p1).has_value());

    auto p2 = base; p2.uri = "";
    EXPECT_FALSE(build_coinbase_jwt(*key_exp, p2).has_value());

    auto p3 = base; p3.key_id = "";
    EXPECT_FALSE(build_coinbase_jwt(*key_exp, p3).has_value());

    auto p4 = base; p4.api_key_name = "";
    EXPECT_FALSE(build_coinbase_jwt(*key_exp, p4).has_value());
}

// ── ttl_secs / now_unix_secs bounds — Coinbase exp-nbf MUST be in (0, 120]s ─
//
// Pre-fix, these foot-guns silently produced JWTs the venue rejected at
// the wire with a 401 that gave operators no actionable hint. Surface
// them locally with InvalidConfig + a SPDLOG_ERROR carrying the bad
// value.

TEST(BuildCoinbaseJwt, RejectsZeroTtl) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    CoinbaseJwtParams p{
        .key_id        = "kid",
        .api_key_name  = "name",
        .method        = "GET",
        .uri           = "host/path",
        .now_unix_secs = 1714867200,
        .ttl_secs      = 0,
    };

    auto r = build_coinbase_jwt(*key_exp, p);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BuildCoinbaseJwt, RejectsTtlAbove120) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    CoinbaseJwtParams p{
        .key_id        = "kid",
        .api_key_name  = "name",
        .method        = "GET",
        .uri           = "host/path",
        .now_unix_secs = 1714867200,
        .ttl_secs      = 121,  // 1 over the venue's documented cap
    };

    auto r = build_coinbase_jwt(*key_exp, p);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BuildCoinbaseJwt, AcceptsTtlExactly120) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    CoinbaseJwtParams p{
        .key_id        = "kid",
        .api_key_name  = "name",
        .method        = "GET",
        .uri           = "host/path",
        .now_unix_secs = 1714867200,
        .ttl_secs      = 120,  // exactly the cap
    };

    EXPECT_TRUE(build_coinbase_jwt(*key_exp, p).has_value());
}

TEST(BuildCoinbaseJwt, RejectsZeroNow) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    CoinbaseJwtParams p{
        .key_id        = "kid",
        .api_key_name  = "name",
        .method        = "GET",
        .uri           = "host/path",
        .now_unix_secs = 0,    // caller forgot to populate
        .ttl_secs      = 60,
    };

    auto r = build_coinbase_jwt(*key_exp, p);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

TEST(BuildCoinbaseJwt, RejectsNowPlusTtlOverflow) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    // UINT64_MAX - 50: any ttl ≥ 51 would overflow. 60 is in our valid
    // range, so this exercises specifically the overflow guard rather
    // than the ttl cap.
    CoinbaseJwtParams p{
        .key_id        = "kid",
        .api_key_name  = "name",
        .method        = "GET",
        .uri           = "host/path",
        .now_unix_secs = UINT64_MAX - 50,
        .ttl_secs      = 60,
    };

    auto r = build_coinbase_jwt(*key_exp, p);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Error::InvalidConfig);
}

// ────────────────────────────────────────────────────────────────────────────
// Cryptographic round-trip — extract pubkey, verify signature
// ────────────────────────────────────────────────────────────────────────────

TEST(BuildCoinbaseJwt, VerifyWithPublicKey) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    CoinbaseJwtParams p{
        .key_id        = "kid",
        .api_key_name  = "subject",
        .method        = "GET",
        .uri           = "exchange.coinbase.com/v3/brokerage/accounts",
        .now_unix_secs = 1714867200,
    };
    auto jwt = build_coinbase_jwt(*key_exp, p);
    ASSERT_TRUE(jwt.has_value());

    JwtParts parts{};
    ASSERT_TRUE(split_jwt(*jwt, parts));

    // Reconstruct the signing input as base64url(header) + "." + base64url(payload).
    std::string signing_input;
    signing_input.append(parts.header_b64);
    signing_input.push_back('.');
    signing_input.append(parts.payload_b64);

    auto sig_p1363 = base64url_decode(parts.sig_b64);
    ASSERT_EQ(sig_p1363.size(), 64u);

    // Convert the JWT-form r||s back to DER, since
    // EVP_DigestVerify expects DER for ECDSA.
    BIGNUM* r = BN_bin2bn(sig_p1363.data(), 32, nullptr);
    BIGNUM* s = BN_bin2bn(sig_p1363.data() + 32, 32, nullptr);
    ASSERT_NE(r, nullptr);
    ASSERT_NE(s, nullptr);
    ECDSA_SIG* ecsig = ECDSA_SIG_new();
    ASSERT_NE(ecsig, nullptr);
    ASSERT_EQ(ECDSA_SIG_set0(ecsig, r, s), 1);  // ecsig now owns r/s
    uint8_t* der_buf = nullptr;
    int der_len = i2d_ECDSA_SIG(ecsig, &der_buf);
    ASSERT_GT(der_len, 0);

    EVP_MD_CTX* mctx = EVP_MD_CTX_new();
    ASSERT_NE(mctx, nullptr);

    auto* pkey = static_cast<EVP_PKEY*>(key_exp->native_handle());
    ASSERT_EQ(EVP_DigestVerifyInit(mctx, /*pctx=*/nullptr, EVP_sha256(),
                                    /*engine=*/nullptr, pkey), 1);

    const int rc = EVP_DigestVerify(
        mctx, der_buf, static_cast<size_t>(der_len),
        reinterpret_cast<const uint8_t*>(signing_input.data()),
        signing_input.size());

    EVP_MD_CTX_free(mctx);
    ECDSA_SIG_free(ecsig);
    OPENSSL_free(der_buf);

    EXPECT_EQ(rc, 1)
        << "EVP_DigestVerify failed: signature does not validate against "
           "the same private key — JWT math is broken";
}

TEST(BuildCoinbaseJwt, VerifyFailsForTamperedPayload) {
    // Sanity: sign once, flip a bit in the payload, verify must fail.
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    ASSERT_TRUE(key_exp.has_value());

    CoinbaseJwtParams p{
        .key_id        = "kid",
        .api_key_name  = "subject",
        .method        = "GET",
        .uri           = "exchange.coinbase.com/v3/brokerage/accounts",
        .now_unix_secs = 1714867200,
    };
    auto jwt = build_coinbase_jwt(*key_exp, p);
    ASSERT_TRUE(jwt.has_value());

    JwtParts parts{};
    ASSERT_TRUE(split_jwt(*jwt, parts));

    // Mutate signing input by changing one base64url char in payload.
    std::string mutated_payload(parts.payload_b64);
    ASSERT_FALSE(mutated_payload.empty());
    mutated_payload[0] = (mutated_payload[0] == 'A' ? 'B' : 'A');

    std::string signing_input;
    signing_input.append(parts.header_b64);
    signing_input.push_back('.');
    signing_input.append(mutated_payload);

    auto sig_p1363 = base64url_decode(parts.sig_b64);
    ASSERT_EQ(sig_p1363.size(), 64u);
    BIGNUM* r = BN_bin2bn(sig_p1363.data(), 32, nullptr);
    BIGNUM* s = BN_bin2bn(sig_p1363.data() + 32, 32, nullptr);
    ECDSA_SIG* ecsig = ECDSA_SIG_new();
    ECDSA_SIG_set0(ecsig, r, s);
    uint8_t* der_buf = nullptr;
    int der_len = i2d_ECDSA_SIG(ecsig, &der_buf);

    EVP_MD_CTX* mctx = EVP_MD_CTX_new();
    auto* pkey = static_cast<EVP_PKEY*>(key_exp->native_handle());
    EVP_DigestVerifyInit(mctx, nullptr, EVP_sha256(), nullptr, pkey);

    const int rc = EVP_DigestVerify(
        mctx, der_buf, static_cast<size_t>(der_len),
        reinterpret_cast<const uint8_t*>(signing_input.data()),
        signing_input.size());

    EVP_MD_CTX_free(mctx);
    ECDSA_SIG_free(ecsig);
    OPENSSL_free(der_buf);

    EXPECT_NE(rc, 1) << "Tampered payload incorrectly verified — "
                        "verification path is broken";
}
