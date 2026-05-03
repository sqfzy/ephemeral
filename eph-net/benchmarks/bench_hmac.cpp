/// @file bench_hmac.cpp
/// Microbenchmarks for `eph::net::HmacSha256Key` / `hmac_sha256_sign`.
///
/// Coverage gap (batch 11): `eph-net` had **no** benchmarks before this
/// file. HMAC-SHA256 sits on the request-signing hot path of every crypto
/// venue REST adapter (Binance, Bybit, OKX) — every order, every cancel,
/// every account-snapshot poll bottoms out here. We measure the realistic
/// payload sizes (a Binance signed query is typically 80-300 chars; a
/// Bybit/OKX signed body is typically 100-500 chars) plus the upper
/// extreme (4 KiB — a batch order request) so a future change touching
/// HMAC plumbing has a baseline to compare against.
///
/// Why these particular cases:
/// - **SignSpan/SignStringView at 64/256/1024/4096 B**: spans the realistic
///   spectrum; the two overloads should match within noise (the
///   string_view overload is a one-liner forwarder).
/// - **TagToHexZeroAlloc**: the canonical `to_hex(span<uint8_t,64>)` —
///   the HFT-safe encoder. Should be ~tens of ns and bounded by the 64
///   nibble-lookups, no allocations.
/// - **TagToHexAllocating**: the convenience `to_hex()` that allocates a
///   `std::string`. Bench it to make the "don't call this on the hot
///   path" advice quantitative.
/// - **KeyConstruct16/32/64/96**: HmacSha256Key normalizes longer-than-
///   block-size keys via SHA-256, so a 96-byte key takes a hash + memcpy
///   while a 16/32/64 byte key is a memcpy + zero-pad. Pinning the cost
///   confirms the hash branch isn't quietly running for short keys.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/net/hmac.hpp"

using eph::net::HmacSha256Key;
using eph::net::HmacSha256Tag;
using eph::net::hmac_sha256_sign;

namespace {

/// A representative API secret — 64 hex chars (the typical Binance shape).
constexpr std::string_view kApiSecret =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/// Build a representative-looking signed-message payload of `n` bytes.
/// Uses a varied byte pattern so the SHA compression rounds get realistic
/// input (an all-zero input can fold suspiciously well on some toolchains).
[[nodiscard]] std::string build_payload(std::size_t n) {
    std::string s;
    s.reserve(n);
    static constexpr char kCharset[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789=&";
    constexpr std::size_t kCharsetN = sizeof(kCharset) - 1;
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(kCharset[i % kCharsetN]);
    }
    return s;
}

/// Build a raw-bytes key of length `n`.
[[nodiscard]] std::vector<uint8_t> build_key(std::size_t n) {
    std::vector<uint8_t> k(n);
    for (std::size_t i = 0; i < n; ++i) {
        k[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFF);
    }
    return k;
}

} // namespace

// ---------------------------------------------------------------------------
// Sign — span<const uint8_t> overload (the canonical one)
// ---------------------------------------------------------------------------

static void BM_HmacSignSpan(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    auto payload = build_payload(n);
    HmacSha256Key key{kApiSecret};
    std::span<const uint8_t> msg{
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size()};

    for (auto _ : state) {
        auto tag = hmac_sha256_sign(key, msg);
        benchmark::DoNotOptimize(tag);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
}
BENCHMARK(BM_HmacSignSpan)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// ---------------------------------------------------------------------------
// Sign — string_view overload (one-liner forwarder; should match the span)
// ---------------------------------------------------------------------------

static void BM_HmacSignStringView(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    auto payload = build_payload(n);
    HmacSha256Key key{kApiSecret};
    std::string_view msg{payload};

    for (auto _ : state) {
        auto tag = hmac_sha256_sign(key, msg);
        benchmark::DoNotOptimize(tag);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(n));
}
BENCHMARK(BM_HmacSignStringView)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// ---------------------------------------------------------------------------
// Tag → hex (the per-request encode-into-header step)
// ---------------------------------------------------------------------------

static void BM_HmacTagToHexZeroAlloc(benchmark::State& state) {
    HmacSha256Key key{kApiSecret};
    auto tag = hmac_sha256_sign(key, std::string_view{"some-payload"});
    std::array<uint8_t, 64> hex_out{};
    for (auto _ : state) {
        const auto written = tag.to_hex(std::span<uint8_t, 64>{hex_out});
        benchmark::DoNotOptimize(hex_out);
        benchmark::DoNotOptimize(written);
    }
}
BENCHMARK(BM_HmacTagToHexZeroAlloc);

static void BM_HmacTagToHexAllocating(benchmark::State& state) {
    HmacSha256Key key{kApiSecret};
    auto tag = hmac_sha256_sign(key, std::string_view{"some-payload"});
    for (auto _ : state) {
        auto s = tag.to_hex();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_HmacTagToHexAllocating);

// ---------------------------------------------------------------------------
// Key construction — the pre-flight cost; differs by raw-key length
// ---------------------------------------------------------------------------

static void BM_HmacKeyConstruct(benchmark::State& state) {
    const std::size_t n = static_cast<std::size_t>(state.range(0));
    auto raw = build_key(n);
    std::span<const uint8_t> raw_span{raw};
    for (auto _ : state) {
        HmacSha256Key k{raw_span};
        benchmark::DoNotOptimize(k);
    }
}
// 16 = short key (zero-padded); 32 = AES-256 sized; 64 = block-aligned;
// 96 = forces SHA-256 of the raw key (longer than block, see hmac.hpp ctor).
BENCHMARK(BM_HmacKeyConstruct)->Arg(16)->Arg(32)->Arg(64)->Arg(96);

int main(int argc, char** argv) {
    // Defensive: on a clean aws-lc build, hmac_sha256_sign never logs at
    // ERROR. Mute spdlog runtime output so a future regression that
    // accidentally fires the error branch (e.g. an EVP_MD lookup failure)
    // does not flood the bench output and confuse the operator.
    spdlog::set_level(spdlog::level::off);

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
