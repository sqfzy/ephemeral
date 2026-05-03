/// @file bench_rate_limiter.cpp
/// Microbenchmarks for `eph::utils::TokenBucket` and `KillSwitch`.
///
/// Coverage gap (batch 11): both primitives sit on the per-order hot
/// path of every HFT strategy (token bucket = exchange rate-limit gate;
/// kill switch = compliance / risk poll). Neither had a microbench.
///
/// Why these particular cases:
/// - **TokenBucketTryAcquireSuccess**: the steady-state pass — bucket
///   has plenty of tokens. Mutex acquire + refill math + decrement.
/// - **TokenBucketTryAcquireFail**: the throttled state — bucket is
///   empty so every call rejects. Confirms the reject path is not
///   measurably more expensive (DoS resistance).
/// - **TokenBucketTryAcquireZeroWeight**: the weight==0 free no-op.
///   Should be lock-free + branch-only.
/// - **KillSwitchTrippedFastPath**: the 99.9% case — `if (!ks.tripped())`
///   in the trading hot loop. Single acquire-load + branch.
/// - **KillSwitchTripFirst** / **KillSwitchTripIdempotent**: trip()
///   called once vs already-tripped. The fast-bail path on the second
///   call must skip the CAS.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include <benchmark/benchmark.h>
#include <spdlog/spdlog.h>

#include "eph/utils/kill_switch.hpp"
#include "eph/utils/rate_limiter.hpp"

using eph::utils::KillSwitch;
using eph::utils::TokenBucket;

// ---------------------------------------------------------------------------
// TokenBucket
// ---------------------------------------------------------------------------

static void BM_TokenBucketTryAcquireSuccess(benchmark::State& state) {
    // Big bucket, very high refill — every call succeeds.
    TokenBucket bucket{TokenBucket::Config{
        .capacity = 1'000'000,
        .refill_per_second = 1e9,  // refills faster than we can call
    }};
    for (auto _ : state) {
        auto r = bucket.try_acquire(1);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_TokenBucketTryAcquireSuccess);

static void BM_TokenBucketTryAcquireFail(benchmark::State& state) {
    // Tiny bucket with zero refill — drain it then every call rejects.
    TokenBucket bucket{TokenBucket::Config{
        .capacity = 1,
        .refill_per_second = 0.0,
    }};
    (void)bucket.try_acquire(1);  // drain to empty
    for (auto _ : state) {
        auto r = bucket.try_acquire(1);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_TokenBucketTryAcquireFail);

static void BM_TokenBucketTryAcquireZeroWeight(benchmark::State& state) {
    // weight=0 is the "free probe" path — must be branch-only, no mutex.
    TokenBucket bucket{TokenBucket::Config{
        .capacity = 100,
        .refill_per_second = 100.0,
    }};
    for (auto _ : state) {
        auto r = bucket.try_acquire(0);
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_TokenBucketTryAcquireZeroWeight);

static void BM_TokenBucketAvailableTokens(benchmark::State& state) {
    TokenBucket bucket{TokenBucket::Config{
        .capacity = 100,
        .refill_per_second = 100.0,
    }};
    for (auto _ : state) {
        auto v = bucket.available_tokens();
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_TokenBucketAvailableTokens);

// ---------------------------------------------------------------------------
// KillSwitch — the hot loop poll path
// ---------------------------------------------------------------------------

static void BM_KillSwitchTrippedPoll(benchmark::State& state) {
    KillSwitch ks;  // never tripped
    for (auto _ : state) {
        auto v = ks.tripped();
        benchmark::DoNotOptimize(v);
    }
}
BENCHMARK(BM_KillSwitchTrippedPoll);

static void BM_KillSwitchTripFirst(benchmark::State& state) {
    // Fresh switch each iteration — measures the CAS-wins path. The
    // construction cost is small (atomic init + std::function default).
    for (auto _ : state) {
        KillSwitch ks;
        ks.trip();
        benchmark::DoNotOptimize(ks);
    }
}
BENCHMARK(BM_KillSwitchTripFirst);

static void BM_KillSwitchTripIdempotent(benchmark::State& state) {
    // Pre-tripped — measures the fast-bail in trip() (acquire-load only).
    KillSwitch ks;
    ks.trip();
    for (auto _ : state) {
        ks.trip();  // no-op
    }
}
BENCHMARK(BM_KillSwitchTripIdempotent);

int main(int argc, char** argv) {
    // TokenBucket logs at DEBUG when reject; SPDLOG_ACTIVE_LEVEL gates
    // that out at compile time in release. Belt-and-braces: also mute
    // runtime so a future log-level change can't accidentally flood
    // the bench output.
    spdlog::set_level(spdlog::level::off);

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
