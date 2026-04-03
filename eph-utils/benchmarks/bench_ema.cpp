/// @file bench_ema.cpp
/// Benchmarks for EMA and EmaCrossover — update throughput.
/// EMA is used in hot paths for signal smoothing.

#include <cstdint>
#include <random>

#include <benchmark/benchmark.h>

#include "eph/utils/ema.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// EMA update benchmarks
// ---------------------------------------------------------------------------

// Measure raw EMA update cost (steady-state, no branch miss from init)
static void BM_EmaUpdate(benchmark::State& state) {
    Ema ema(0.1);
    (void)ema.update(100.0);  // seed

    double val = 100.0;
    for (auto _ : state) {
        val = ema.update(val + 0.01);
        benchmark::DoNotOptimize(val);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EmaUpdate);

// EMA update with pre-generated random prices (realistic)
static void BM_EmaUpdate_Random(benchmark::State& state) {
    Ema ema(0.1);
    (void)ema.update(100.0);

    constexpr size_t kSize = 8192;
    std::vector<double> prices(kSize);
    std::mt19937_64 rng(42);
    std::normal_distribution<double> dist(100.0, 5.0);
    for (auto& p : prices) p = dist(rng);

    size_t idx = 0;
    for (auto _ : state) {
        auto v = ema.update(prices[idx]);
        benchmark::DoNotOptimize(v);
        idx = (idx + 1) & (kSize - 1);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EmaUpdate_Random);

// ---------------------------------------------------------------------------
// EmaCrossover benchmarks
// ---------------------------------------------------------------------------

// Crossover update with random prices
static void BM_EmaCrossover_Update(benchmark::State& state) {
    EmaCrossover cross(5, 20);
    (void)cross.update(100.0);

    constexpr size_t kSize = 8192;
    std::vector<double> prices(kSize);
    std::mt19937_64 rng(42);
    std::normal_distribution<double> dist(100.0, 5.0);
    for (auto& p : prices) p = dist(rng);

    size_t idx = 0;
    for (auto _ : state) {
        auto sig = cross.update(prices[idx]);
        benchmark::DoNotOptimize(sig);
        idx = (idx + 1) & (kSize - 1);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EmaCrossover_Update);

BENCHMARK_MAIN();
