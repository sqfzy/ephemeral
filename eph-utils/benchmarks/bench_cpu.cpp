/// @file bench_cpu.cpp
/// Benchmarks for CPU utilities — cpu_relax() spin-wait hint overhead.
/// cpu_relax() is called in every spin loop, so its cost directly
/// impacts lock acquisition latency.

#include <benchmark/benchmark.h>

#include "eph/utils/cpu.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// cpu_relax() — cost of a single YIELD/PAUSE instruction
// ---------------------------------------------------------------------------

static void BM_CpuRelax(benchmark::State& state) {
    for (auto _ : state) {
        cpu_relax();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CpuRelax);

// ---------------------------------------------------------------------------
// Baseline: empty loop iteration (to measure framework overhead)
// ---------------------------------------------------------------------------

static void BM_EmptyLoop(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EmptyLoop);

BENCHMARK_MAIN();
