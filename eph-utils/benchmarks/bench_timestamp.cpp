/// @file bench_timestamp.cpp
/// Benchmarks for timestamp conversion and formatting functions.
/// These are on the critical path for market data processing.

#include <cstdint>

#include <benchmark/benchmark.h>

#include "eph/utils/timestamp.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// Unit conversion benchmarks (should be near-zero cost)
// ---------------------------------------------------------------------------

static void BM_MsToNs(benchmark::State& state) {
    int64_t ms = 1'774'656'000'123LL;
    for (auto _ : state) {
        auto ns = ms_to_ns(ms);
        benchmark::DoNotOptimize(ns);
        ms++;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MsToNs);

static void BM_NsToMs(benchmark::State& state) {
    uint64_t ns = 1'774'656'000'123'456'789ULL;
    for (auto _ : state) {
        auto ms = ns_to_ms(ns);
        benchmark::DoNotOptimize(ms);
        ns++;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NsToMs);

// ---------------------------------------------------------------------------
// Wall-clock benchmarks
// ---------------------------------------------------------------------------

static void BM_NowNs(benchmark::State& state) {
    for (auto _ : state) {
        auto ts = now_ns();
        benchmark::DoNotOptimize(ts);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NowNs);

static void BM_NowMs(benchmark::State& state) {
    for (auto _ : state) {
        auto ts = now_ms();
        benchmark::DoNotOptimize(ts);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NowMs);

// ---------------------------------------------------------------------------
// Feed latency computation
// ---------------------------------------------------------------------------

static void BM_FeedLatencyNs(benchmark::State& state) {
    uint64_t exchange_ts = now_ns();
    for (auto _ : state) {
        auto lat = feed_latency_ns(exchange_ts);
        benchmark::DoNotOptimize(lat);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FeedLatencyNs);

// ---------------------------------------------------------------------------
// Formatting benchmarks (not hot-path, but useful for logging overhead)
// ---------------------------------------------------------------------------

static void BM_FormatTimestampNs(benchmark::State& state) {
    uint64_t epoch_ns = 1'774'656'000'123'456'789ULL;
    for (auto _ : state) {
        auto s = format_timestamp_ns(epoch_ns);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FormatTimestampNs);

static void BM_FormatTimestampMs(benchmark::State& state) {
    int64_t epoch_ms = 1'774'656'000'123LL;
    for (auto _ : state) {
        auto s = format_timestamp_ms(epoch_ms);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FormatTimestampMs);

BENCHMARK_MAIN();
