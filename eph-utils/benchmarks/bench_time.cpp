/// @file bench_time.cpp
/// Benchmarks for TSC timing utilities — now(), to_ns(), to_cycles().
/// TSC::now() is the most latency-critical function in the library.

#include <cstdint>

#include <benchmark/benchmark.h>

#include "eph/utils/time.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// TSC::now() — raw cycle counter read
// ---------------------------------------------------------------------------

static void BM_TSCNow(benchmark::State& state) {
    for (auto _ : state) {
        uint64_t tsc = TSC::now();
        benchmark::DoNotOptimize(tsc);
    }
}
BENCHMARK(BM_TSCNow);

// ---------------------------------------------------------------------------
// TSC::to_ns() — cycle-to-nanosecond conversion
// ---------------------------------------------------------------------------

static void BM_TSCToNs(benchmark::State& state) {
    TSC::init();
    uint64_t cycles = 1000;
    for (auto _ : state) {
        auto ns = TSC::to_ns(cycles);
        benchmark::DoNotOptimize(ns);
        ++cycles;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TSCToNs);

// ---------------------------------------------------------------------------
// TSC::to_cycles() — nanosecond-to-cycle conversion
// ---------------------------------------------------------------------------

static void BM_TSCToCycles(benchmark::State& state) {
    TSC::init();
    double ns = 1000.0;
    for (auto _ : state) {
        auto c = TSC::to_cycles(ns);
        benchmark::DoNotOptimize(c);
        ns += 0.1;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TSCToCycles);

// ---------------------------------------------------------------------------
// TSC::now() delta — measure a minimal operation
// ---------------------------------------------------------------------------

static void BM_TSCDeltaPair(benchmark::State& state) {
    for (auto _ : state) {
        uint64_t start = TSC::now();
        benchmark::ClobberMemory();
        uint64_t end = TSC::now();
        uint64_t delta = end - start;
        benchmark::DoNotOptimize(delta);
    }
}
BENCHMARK(BM_TSCDeltaPair);

BENCHMARK_MAIN();
