/// @file bench_jwt.cpp
/// Microbenchmarks for `eph::net::Es256PrivateKey` PEM loading and
/// `build_coinbase_jwt` token construction.
///
/// Coverage gap (batch 11): Coinbase Advanced Trade is the only crypto
/// venue eph supports today that uses ES256 JWT auth (vs HMAC-SHA256
/// for Binance/OKX/Bybit). Every signed REST request to Coinbase
/// builds a fresh JWT (the docs cap `exp - nbf` at 120s, so the token
/// is essentially per-request). Pinning the construction cost matters
/// for HFT-style burst flows (account-state polls, batch order
/// management).
///
/// Why these specific cases:
/// - **Es256PemLoad**: parsing the PEM-wrapped EC private key. One-time
///   per session, but worth measuring so a future refactor doesn't
///   accidentally invoke aws-lc with a per-request key load.
/// - **BuildCoinbaseJwtFresh**: full JWT build path — header/payload
///   JSON, base64url encode, ECDSA sign, DER→P-1363 conversion,
///   final assembly. The 99% case (CSPRNG nonce auto-generated).
/// - **BuildCoinbaseJwtNonceOverride**: same path with a caller-supplied
///   nonce. Skips the RAND_bytes call — the difference reveals the
///   CSPRNG cost.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/net/jwt_signed_request.hpp"

using eph::net::CoinbaseJwtParams;
using eph::net::Es256PrivateKey;
using eph::net::build_coinbase_jwt;

namespace {

// Throwaway P-256 key (matches the one used in test_jwt_signed_request.cpp;
// has no production lifetime).
constexpr std::string_view kEcP256PemValid =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEINOpmaBE6cuLkglCisJtB93Y4yJ2RGC4HSHdUJZesfueoAoGCCqGSM49\n"
    "AwEHoUQDQgAEipxCkur6xELXaT83IyfmFcCIETWrJCXZRs+en43AvHt+Lu0i15E9\n"
    "90z0OphnjtVyoeuhbuMPChrOEkZbGyZUMw==\n"
    "-----END EC PRIVATE KEY-----\n";

constexpr std::string_view kKeyId =
    "organizations/ABC123/apiKeys/XYZ789";
constexpr std::string_view kApiKeyName =
    "organizations/ABC123/apiKeys/XYZ789-name";

} // namespace

// ---------------------------------------------------------------------------
// PEM key load — one-time per session but worth pinning
// ---------------------------------------------------------------------------

static void BM_Es256PemLoad(benchmark::State& state) {
    for (auto _ : state) {
        auto k = Es256PrivateKey::from_pem(kEcP256PemValid);
        benchmark::DoNotOptimize(k);
    }
}
BENCHMARK(BM_Es256PemLoad);

// ---------------------------------------------------------------------------
// JWT build — the per-request path on Coinbase
// ---------------------------------------------------------------------------

static void BM_BuildCoinbaseJwtFresh(benchmark::State& state) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    if (!key_exp) state.SkipWithError("PEM load failed in setup");
    auto& key = *key_exp;

    CoinbaseJwtParams p{
        .key_id        = kKeyId,
        .api_key_name  = kApiKeyName,
        .method        = "GET",
        .uri           = "exchange.coinbase.com/v3/brokerage/accounts",
        .now_unix_secs = 1733846400ULL,
        .ttl_secs      = 120,
        .nonce_override = {},  // builder pulls fresh CSPRNG bytes
    };

    for (auto _ : state) {
        auto jwt = build_coinbase_jwt(key, p);
        benchmark::DoNotOptimize(jwt);
    }
}
BENCHMARK(BM_BuildCoinbaseJwtFresh);

static void BM_BuildCoinbaseJwtNonceOverride(benchmark::State& state) {
    auto key_exp = Es256PrivateKey::from_pem(kEcP256PemValid);
    if (!key_exp) state.SkipWithError("PEM load failed in setup");
    auto& key = *key_exp;

    // Deterministic 32-byte nonce — skips the aws-lc RAND_bytes call.
    std::array<uint8_t, 32> nonce_bytes{};
    for (std::size_t i = 0; i < nonce_bytes.size(); ++i) {
        nonce_bytes[i] = static_cast<uint8_t>(i);
    }
    CoinbaseJwtParams p{
        .key_id        = kKeyId,
        .api_key_name  = kApiKeyName,
        .method        = "GET",
        .uri           = "exchange.coinbase.com/v3/brokerage/accounts",
        .now_unix_secs = 1733846400ULL,
        .ttl_secs      = 120,
        .nonce_override = std::span<const uint8_t>{nonce_bytes},
    };

    for (auto _ : state) {
        auto jwt = build_coinbase_jwt(key, p);
        benchmark::DoNotOptimize(jwt);
    }
}
BENCHMARK(BM_BuildCoinbaseJwtNonceOverride);

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::off);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
