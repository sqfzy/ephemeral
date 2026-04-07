/// @file bench_rate_limiter.cpp
/// Micro-benchmarks for RateLimiter token bucket operations.
///
/// RateLimiter::try_acquire() gates every outbound API request. Its overhead
/// directly impacts request submission latency, especially in burst scenarios
/// where many requests are submitted in rapid succession.

#include <benchmark/benchmark.h>

#include "eph/net/rate_limiter.hpp"

using namespace eph::net;

// ---------------------------------------------------------------------------
// try_acquire(1) — hot-path token consumption (tokens available)
// ---------------------------------------------------------------------------

static void BM_RateLimiter_TryAcquire_Available(benchmark::State& state) {
    // Very high rate ensures tokens are always available.
    RateLimiter rl(1'000'000'000.0, 1'000'000);

    for (auto _ : state) {
        bool ok = rl.try_acquire();
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_RateLimiter_TryAcquire_Available);

// ---------------------------------------------------------------------------
// try_acquire(1) — tokens exhausted (fast reject path)
// ---------------------------------------------------------------------------

static void BM_RateLimiter_TryAcquire_Exhausted(benchmark::State& state) {
    // Zero rate, exhaust burst immediately.
    RateLimiter rl(0.0, 1);
    rl.try_acquire(); // exhaust the single token

    for (auto _ : state) {
        bool ok = rl.try_acquire();
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_RateLimiter_TryAcquire_Exhausted);

// ---------------------------------------------------------------------------
// try_acquire(N) — batch token consumption
// ---------------------------------------------------------------------------

static void BM_RateLimiter_TryAcquireBatch(benchmark::State& state) {
    const auto n = static_cast<size_t>(state.range(0));
    RateLimiter rl(1'000'000'000.0, 1'000'000);

    for (auto _ : state) {
        bool ok = rl.try_acquire(n);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_RateLimiter_TryAcquireBatch)->Arg(1)->Arg(10)->Arg(100);

// ---------------------------------------------------------------------------
// available() — monitoring query (includes refill computation)
// ---------------------------------------------------------------------------

static void BM_RateLimiter_Available(benchmark::State& state) {
    RateLimiter rl(1000.0, 100);

    for (auto _ : state) {
        double a = rl.available();
        benchmark::DoNotOptimize(a);
    }
}
BENCHMARK(BM_RateLimiter_Available);

// ---------------------------------------------------------------------------
// is_exhausted() — convenience predicate
// ---------------------------------------------------------------------------

static void BM_RateLimiter_IsExhausted(benchmark::State& state) {
    RateLimiter rl(1000.0, 100);

    for (auto _ : state) {
        bool e = rl.is_exhausted();
        benchmark::DoNotOptimize(e);
    }
}
BENCHMARK(BM_RateLimiter_IsExhausted);

// ---------------------------------------------------------------------------
// to_json() — monitoring snapshot serialization
// ---------------------------------------------------------------------------

static void BM_RateLimiter_ToJson(benchmark::State& state) {
    RateLimiter rl(1200.0, 50);

    for (auto _ : state) {
        auto j = rl.to_json();
        benchmark::DoNotOptimize(j);
    }
}
BENCHMARK(BM_RateLimiter_ToJson);

// ---------------------------------------------------------------------------
// dump() — human-readable state snapshot for logging
// ---------------------------------------------------------------------------

static void BM_RateLimiter_Dump(benchmark::State& state) {
    RateLimiter rl(1200.0, 50);

    for (auto _ : state) {
        auto d = rl.dump();
        benchmark::DoNotOptimize(d);
    }
}
BENCHMARK(BM_RateLimiter_Dump);

// ---------------------------------------------------------------------------
// reset() — refill to burst capacity
// ---------------------------------------------------------------------------

static void BM_RateLimiter_Reset(benchmark::State& state) {
    RateLimiter rl(1000.0, 100);

    for (auto _ : state) {
        rl.reset();
    }
}
BENCHMARK(BM_RateLimiter_Reset);

// ---------------------------------------------------------------------------
// Burst drain: exhaust all tokens then reset (simulates burst window cycle)
// ---------------------------------------------------------------------------

static void BM_RateLimiter_BurstDrainAndReset(benchmark::State& state) {
    const auto burst = static_cast<size_t>(state.range(0));
    RateLimiter rl(0.0, burst);

    for (auto _ : state) {
        // Drain all tokens one at a time
        for (size_t i = 0; i < burst; ++i) {
            benchmark::DoNotOptimize(rl.try_acquire());
        }
        rl.reset();
    }
    state.SetItemsProcessed(
        static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(burst));
}
BENCHMARK(BM_RateLimiter_BurstDrainAndReset)->Arg(10)->Arg(100)->Arg(1000);

// ---------------------------------------------------------------------------
// Const-ref queries — verify no perf regression from mutable refactoring
// ---------------------------------------------------------------------------

static void BM_RateLimiter_ConstAvailable(benchmark::State& state) {
    RateLimiter rl(1000.0, 100);
    const RateLimiter& crl = rl;

    for (auto _ : state) {
        double a = crl.available();
        benchmark::DoNotOptimize(a);
    }
}
BENCHMARK(BM_RateLimiter_ConstAvailable);

static void BM_RateLimiter_ConstToJson(benchmark::State& state) {
    RateLimiter rl(1200.0, 50);
    const RateLimiter& crl = rl;

    for (auto _ : state) {
        auto j = crl.to_json();
        benchmark::DoNotOptimize(j);
    }
}
BENCHMARK(BM_RateLimiter_ConstToJson);

BENCHMARK_MAIN();
