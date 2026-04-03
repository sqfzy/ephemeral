/// @file bench_control_plane.cpp
/// Micro-benchmarks for control-plane components: RateLimiter, CircuitBreaker.
///
/// These components sit on the send path for rate-limited exchanges.
/// While not latency-critical (sub-microsecond is fine), measuring their
/// overhead ensures changes don't regress unexpectedly.

#include <benchmark/benchmark.h>

#include "eph/net/circuit_breaker.hpp"
#include "eph/net/rate_limiter.hpp"

using namespace eph::net;

// ---------------------------------------------------------------------------
// RateLimiter benchmarks
// ---------------------------------------------------------------------------

/// Measure try_acquire() throughput when tokens are available.
/// This is the fast path (no contention, no blocking).
static void BM_RateLimiter_TryAcquire_Available(benchmark::State& state) {
    // High rate ensures tokens are always available
    RateLimiter rl(1'000'000'000.0, 1'000'000);

    for (auto _ : state) {
        bool ok = rl.try_acquire();
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_RateLimiter_TryAcquire_Available);

/// Measure try_acquire() when tokens are exhausted (fast rejection path).
static void BM_RateLimiter_TryAcquire_Exhausted(benchmark::State& state) {
    // Zero rate, burst=1: exhaust immediately, all subsequent calls fail
    RateLimiter rl(0.0, 1);
    (void)rl.try_acquire(); // exhaust

    for (auto _ : state) {
        bool ok = rl.try_acquire();
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_RateLimiter_TryAcquire_Exhausted);

/// Measure available() query overhead (includes refill calculation).
static void BM_RateLimiter_Available(benchmark::State& state) {
    RateLimiter rl(1000.0, 100);

    for (auto _ : state) {
        double avail = rl.available();
        benchmark::DoNotOptimize(avail);
    }
}
BENCHMARK(BM_RateLimiter_Available);

// ---------------------------------------------------------------------------
// CircuitBreaker benchmarks
// ---------------------------------------------------------------------------

/// Measure allow() in Closed state (common case, always returns true).
static void BM_CircuitBreaker_Allow_Closed(benchmark::State& state) {
    CircuitBreaker cb;

    for (auto _ : state) {
        bool ok = cb.allow();
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_CircuitBreaker_Allow_Closed);

/// Measure allow() in Open state (fast rejection path).
static void BM_CircuitBreaker_Allow_Open(benchmark::State& state) {
    // Trip the breaker and use a long open_duration so it stays Open
    CircuitBreaker cb(CircuitBreaker::Config{1, std::chrono::seconds{3600}});
    cb.record_failure();

    for (auto _ : state) {
        bool ok = cb.allow();
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_CircuitBreaker_Allow_Open);

/// Measure record_success() in Closed state (common case, resets counter).
static void BM_CircuitBreaker_RecordSuccess_Closed(benchmark::State& state) {
    CircuitBreaker cb;

    for (auto _ : state) {
        cb.record_success();
    }
}
BENCHMARK(BM_CircuitBreaker_RecordSuccess_Closed);

/// Measure record_failure() in Closed state (increments counter).
static void BM_CircuitBreaker_RecordFailure_Closed(benchmark::State& state) {
    // High threshold so it never trips during the benchmark
    CircuitBreaker cb(CircuitBreaker::Config{
        1'000'000'000, std::chrono::seconds{60}});

    for (auto _ : state) {
        cb.record_failure();
    }
}
BENCHMARK(BM_CircuitBreaker_RecordFailure_Closed);

/// Measure state() query overhead.
static void BM_CircuitBreaker_State(benchmark::State& state) {
    CircuitBreaker cb;

    for (auto _ : state) {
        auto s = cb.state();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_CircuitBreaker_State);

BENCHMARK_MAIN();
