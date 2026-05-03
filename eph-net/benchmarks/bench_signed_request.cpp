/// @file bench_signed_request.cpp
/// Microbenchmarks for `eph::net::SignedRequest<Traits>` — the venue-
/// traits-driven HMAC sign-into-header helper for Binance / OKX / Bybit.
///
/// Coverage gap (batch 11): the canonical "sign + bundle headers" path
/// every signed-REST hot path takes. Bench tracks the per-call cost of
/// each venue's wire format so any future regression in `finalize_` (the
/// shared header-construction routine) surfaces immediately.
///
/// Why these particular cases:
/// - **Binance / OKX / Bybit `headers_for_*`** at a representative
///   payload size (a typical order-management body is ~150 chars).
///   These three exercise the three different wire formats:
///     * Binance:  hex render + 2-header bundle (sign + ts).
///     * OKX:      base64 render + 2-header bundle.
///     * Bybit:    hex render + 3-header bundle (sign + ts + recv-window).
/// - **`sign_to_string`** — the low-level escape hatch that just returns
///   the venue-formatted signature. Confirms the cost stays within
///   `hmac_sign + format_signature` and isn't quietly doing any header
///   plumbing the caller didn't ask for.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/net/hmac.hpp"
#include "eph/net/signed_request.hpp"

using eph::net::BinanceSignTraits;
using eph::net::BybitSignTraits;
using eph::net::HmacSha256Key;
using eph::net::OkxSignTraits;
using eph::net::SignedRequest;

namespace {

constexpr std::string_view kApiSecret =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// A representative Binance signed-query string: ~140 chars, the typical
// shape for a SPOT new-order request with timestamp pre-spliced.
constexpr std::string_view kBinanceQuery =
    "symbol=BTCUSDT&side=BUY&type=LIMIT&timeInForce=GTC&quantity=0.001"
    "&price=50000.00&recvWindow=5000&timestamp=1733846400000";

// A representative OKX V5 path + JSON body: ~80B path, ~120B body.
constexpr std::string_view kOkxPath =
    "/api/v5/trade/order";
constexpr std::string_view kOkxBody =
    R"({"instId":"BTC-USDT-SWAP","tdMode":"cross","side":"buy",)"
    R"("ordType":"limit","sz":"1","px":"50000"})";
constexpr std::string_view kOkxTs =
    "2025-12-10T15:30:00.000Z";

// A representative Bybit V5 query: similar shape to Binance.
constexpr std::string_view kBybitQuery =
    "category=linear&symbol=BTCUSDT&side=Buy&orderType=Limit&qty=0.001"
    "&price=50000.00&timeInForce=GTC";
constexpr std::string_view kBybitTs       = "1733846400000";
constexpr std::string_view kBybitApiKey   = "abcdef0123456789abcdef0123456789";
constexpr std::string_view kBybitRecvWnd  = "5000";

} // namespace

// ---------------------------------------------------------------------------
// Binance — `headers_for_query` (the 95% case for SPOT/Futures REST)
// ---------------------------------------------------------------------------

static void BM_SignedReqBinanceHeadersForQuery(benchmark::State& state) {
    SignedRequest<BinanceSignTraits> sr{HmacSha256Key{kApiSecret}};
    constexpr uint64_t ts_ms = 1733846400000ULL;
    for (auto _ : state) {
        auto bundle = sr.headers_for_query(kBinanceQuery, ts_ms);
        benchmark::DoNotOptimize(bundle);
    }
}
BENCHMARK(BM_SignedReqBinanceHeadersForQuery);

// ---------------------------------------------------------------------------
// OKX — `headers_for_okx` (POST + JSON body shape)
// ---------------------------------------------------------------------------

static void BM_SignedReqOkxHeadersForOkx(benchmark::State& state) {
    SignedRequest<OkxSignTraits> sr{HmacSha256Key{kApiSecret}};
    for (auto _ : state) {
        auto bundle = sr.headers_for_okx(
            "POST", kOkxPath, kOkxBody, kOkxTs);
        benchmark::DoNotOptimize(bundle);
    }
}
BENCHMARK(BM_SignedReqOkxHeadersForOkx);

// ---------------------------------------------------------------------------
// Bybit — `headers_for_bybit` (3-header bundle: sign + ts + recv-window)
// ---------------------------------------------------------------------------

static void BM_SignedReqBybitHeadersForBybit(benchmark::State& state) {
    SignedRequest<BybitSignTraits> sr{HmacSha256Key{kApiSecret}};
    for (auto _ : state) {
        auto bundle = sr.headers_for_bybit(
            kBybitApiKey, kBybitTs, kBybitRecvWnd, kBybitQuery);
        benchmark::DoNotOptimize(bundle);
    }
}
BENCHMARK(BM_SignedReqBybitHeadersForBybit);

// ---------------------------------------------------------------------------
// Low-level escape hatch — `sign_to_string`. Should be a touch under
// the `headers_for_*` paths because no header bundle is built.
// ---------------------------------------------------------------------------

static void BM_SignedReqBinanceSignToString(benchmark::State& state) {
    SignedRequest<BinanceSignTraits> sr{HmacSha256Key{kApiSecret}};
    for (auto _ : state) {
        auto sig = sr.sign_to_string(kBinanceQuery);
        benchmark::DoNotOptimize(sig);
    }
}
BENCHMARK(BM_SignedReqBinanceSignToString);

static void BM_SignedReqOkxSignToString(benchmark::State& state) {
    SignedRequest<OkxSignTraits> sr{HmacSha256Key{kApiSecret}};
    // Sign just the body (the `sign_to_string` overload doesn't bundle the
    // venue's pre-MAC formatting — the caller pre-builds the to-sign
    // string; we feed the body directly to size the rendering cost).
    for (auto _ : state) {
        auto sig = sr.sign_to_string(kOkxBody);
        benchmark::DoNotOptimize(sig);
    }
}
BENCHMARK(BM_SignedReqOkxSignToString);

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::off);
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
