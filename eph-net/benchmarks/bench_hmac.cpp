/// @file bench_hmac.cpp
/// Micro-benchmarks for HMAC-SHA256 signing operations.
///
/// HMAC signing sits on the send path for authenticated REST API calls
/// (Binance, OKX, etc.). Measuring throughput and latency ensures the
/// crypto backend (aws-lc) performs well for our use case.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

#include "eph/net/hmac.hpp"

using namespace eph::net;

// ---------------------------------------------------------------------------
// HMAC-SHA256 benchmarks
// ---------------------------------------------------------------------------

/// Benchmark hmac_sha256_hex with a typical Binance-sized query string.
static void BM_HmacSha256Hex_BinanceQuery(benchmark::State& state) {
    std::string_view key = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
    std::string_view msg =
        "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC"
        "&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";

    for (auto _ : state) {
        auto result = hmac_sha256_hex(key, msg);
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(msg.size()));
}
BENCHMARK(BM_HmacSha256Hex_BinanceQuery);

/// Benchmark raw hmac_sha256 (no hex encoding overhead).
static void BM_HmacSha256_Raw(benchmark::State& state) {
    std::string_view key = "test-secret-key-32-bytes-long!!!";
    std::string_view msg = "timestamp=1699999999999&symbol=BTCUSDT";

    for (auto _ : state) {
        auto result = hmac_sha256(key, msg);
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(msg.size()));
}
BENCHMARK(BM_HmacSha256_Raw);

/// Benchmark hmac_verify (includes compute + constant-time compare).
static void BM_HmacVerify(benchmark::State& state) {
    std::string_view key = "test-key-for-verification";
    std::string_view msg = "data-to-verify&timestamp=12345";
    auto digest = hmac_sha256(key, msg);

    for (auto _ : state) {
        bool ok = hmac_verify(key, msg, *digest);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_HmacVerify);

/// Benchmark hmac_verify_hex (includes compute + hex encode + constant-time compare).
static void BM_HmacVerifyHex(benchmark::State& state) {
    std::string_view key = "test-key-for-verification";
    std::string_view msg = "data-to-verify&timestamp=12345";
    auto hex = hmac_sha256_hex(key, msg);

    for (auto _ : state) {
        bool ok = hmac_verify_hex(key, msg, *hex);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_HmacVerifyHex);

/// Benchmark to_hex encoding (isolated from HMAC compute).
static void BM_ToHex_32Bytes(benchmark::State& state) {
    std::array<uint8_t, 32> data{};
    for (size_t i = 0; i < 32; ++i) data[i] = static_cast<uint8_t>(i);

    for (auto _ : state) {
        auto result = to_hex(data);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_ToHex_32Bytes);

/// Benchmark to_base64 encoding of a 32-byte digest (OKX signature path).
static void BM_ToBase64_32Bytes(benchmark::State& state) {
    std::array<uint8_t, 32> data{};
    for (size_t i = 0; i < 32; ++i) data[i] = static_cast<uint8_t>(i);

    for (auto _ : state) {
        auto result = to_base64(data);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_ToBase64_32Bytes);

/// Benchmark HMAC with varying message sizes.
static void BM_HmacSha256_MessageSize(benchmark::State& state) {
    std::string_view key = "fixed-key";
    std::string msg(static_cast<size_t>(state.range(0)), 'x');

    for (auto _ : state) {
        auto result = hmac_sha256(key, msg);
        benchmark::DoNotOptimize(result);
    }
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(BM_HmacSha256_MessageSize)->Range(16, 4096);

BENCHMARK_MAIN();
