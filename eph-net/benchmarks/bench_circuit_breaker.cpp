/// @file bench_circuit_breaker.cpp
/// Micro-benchmarks for CircuitBreaker operations.
///
/// CircuitBreaker sits on the send path — allow() is checked before every
/// outbound request. While mutex-protected (not on the hot nanosecond path),
/// its overhead matters for high-frequency request loops (~100K req/s).

#include <benchmark/benchmark.h>

#include "eph/net/circuit_breaker.hpp"

using namespace eph::net;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// allow() — hot-path gate check in Closed state (most common case)
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_Allow_Closed(benchmark::State& state) {
    CircuitBreaker cb(CircuitBreaker::Config{5, 30s, 1});

    for (auto _ : state) {
        bool ok = cb.allow();
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_CircuitBreaker_Allow_Closed);

// ---------------------------------------------------------------------------
// allow() — Open state (blocked, no transition)
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_Allow_Open(benchmark::State& state) {
    CircuitBreaker cb(CircuitBreaker::Config{1, 60s, 1});
    cb.record_failure(); // trip to Open

    for (auto _ : state) {
        bool ok = cb.allow();
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_CircuitBreaker_Allow_Open);

// ---------------------------------------------------------------------------
// record_failure() — Closed state, below threshold
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_RecordFailure(benchmark::State& state) {
    // Use a high threshold so we never trip during the benchmark.
    // Reset periodically to keep failure_count bounded.
    CircuitBreaker cb(CircuitBreaker::Config{1'000'000, 30s, 1});

    size_t i = 0;
    for (auto _ : state) {
        cb.record_failure();
        if (++i % 100'000 == 0) cb.reset();
    }
}
BENCHMARK(BM_CircuitBreaker_RecordFailure);

// ---------------------------------------------------------------------------
// record_success() — Closed state (resets failure count)
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_RecordSuccess(benchmark::State& state) {
    CircuitBreaker cb(CircuitBreaker::Config{5, 30s, 1});

    for (auto _ : state) {
        cb.record_success();
    }
}
BENCHMARK(BM_CircuitBreaker_RecordSuccess);

// ---------------------------------------------------------------------------
// state() — read-only query
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_State(benchmark::State& state) {
    CircuitBreaker cb(CircuitBreaker::Config{5, 30s, 1});

    for (auto _ : state) {
        auto s = cb.state();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_CircuitBreaker_State);

// ---------------------------------------------------------------------------
// failure_count() — read-only query
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_FailureCount(benchmark::State& state) {
    CircuitBreaker cb(CircuitBreaker::Config{100, 30s, 1});
    cb.record_failure();
    cb.record_failure();
    cb.record_failure();

    for (auto _ : state) {
        auto n = cb.failure_count();
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_CircuitBreaker_FailureCount);

// ---------------------------------------------------------------------------
// is_tripped() — convenience predicate
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_IsTripped_Closed(benchmark::State& state) {
    CircuitBreaker cb(CircuitBreaker::Config{5, 30s, 1});

    for (auto _ : state) {
        bool t = cb.is_tripped();
        benchmark::DoNotOptimize(t);
    }
}
BENCHMARK(BM_CircuitBreaker_IsTripped_Closed);

static void BM_CircuitBreaker_IsTripped_Open(benchmark::State& state) {
    CircuitBreaker cb(CircuitBreaker::Config{1, 60s, 1});
    cb.record_failure(); // trip

    for (auto _ : state) {
        bool t = cb.is_tripped();
        benchmark::DoNotOptimize(t);
    }
}
BENCHMARK(BM_CircuitBreaker_IsTripped_Open);

// ---------------------------------------------------------------------------
// to_json() — monitoring snapshot serialization
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_ToJson(benchmark::State& state) {
    CircuitBreaker cb(CircuitBreaker::Config{5, 30s, 2});
    cb.record_failure();
    cb.record_failure();

    for (auto _ : state) {
        auto j = cb.to_json();
        benchmark::DoNotOptimize(j);
    }
}
BENCHMARK(BM_CircuitBreaker_ToJson);

// ---------------------------------------------------------------------------
// time_until_half_open() — monitoring query in Open state
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_TimeUntilHalfOpen(benchmark::State& state) {
    CircuitBreaker cb(CircuitBreaker::Config{1, 60s, 1});
    cb.record_failure(); // trip to Open

    for (auto _ : state) {
        auto t = cb.time_until_half_open();
        benchmark::DoNotOptimize(t);
    }
}
BENCHMARK(BM_CircuitBreaker_TimeUntilHalfOpen);

// ---------------------------------------------------------------------------
// dump() — human-readable state snapshot for logging
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_Dump(benchmark::State& state) {
    CircuitBreaker cb(CircuitBreaker::Config{5, 30s, 2});
    cb.record_failure();
    cb.record_failure();

    for (auto _ : state) {
        auto d = cb.dump();
        benchmark::DoNotOptimize(d);
    }
}
BENCHMARK(BM_CircuitBreaker_Dump);

// ---------------------------------------------------------------------------
// Full trip cycle: record_failure x N -> allow (blocked) -> reset
// ---------------------------------------------------------------------------

static void BM_CircuitBreaker_TripAndReset(benchmark::State& state) {
    for (auto _ : state) {
        CircuitBreaker cb(CircuitBreaker::Config{3, 30s, 1});
        cb.record_failure();
        cb.record_failure();
        cb.record_failure();
        benchmark::DoNotOptimize(cb.allow()); // blocked
        cb.reset();
        benchmark::DoNotOptimize(cb.allow()); // allowed
    }
}
BENCHMARK(BM_CircuitBreaker_TripAndReset);

BENCHMARK_MAIN();
