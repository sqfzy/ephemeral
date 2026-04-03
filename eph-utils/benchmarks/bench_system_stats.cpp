/// @file bench_system_stats.cpp
/// Benchmarks for SystemStats and SystemResourceStats operations.
/// These measure the overhead of getrusage-based profiling and
/// /proc filesystem reads.

#include <benchmark/benchmark.h>

#include "eph/utils/system_stats.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// SystemStats::snapshot() — full resource snapshot (getrusage + /proc reads)
// ---------------------------------------------------------------------------

static void BM_SystemStatsSnapshot(benchmark::State& state) {
    SystemStats stats;
    for (auto _ : state) {
        auto snap = stats.snapshot();
        benchmark::DoNotOptimize(snap);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SystemStatsSnapshot);

// ---------------------------------------------------------------------------
// SystemStats::reset() — re-baseline via getrusage
// ---------------------------------------------------------------------------

static void BM_SystemStatsReset(benchmark::State& state) {
    SystemStats stats;
    for (auto _ : state) {
        stats.reset();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SystemStatsReset);

// ---------------------------------------------------------------------------
// SystemResourceStats::dump() — format to human-readable string
// ---------------------------------------------------------------------------

static void BM_SystemResourceStatsDump(benchmark::State& state) {
    SystemResourceStats snap{
        .majflt = 2, .minflt = 500, .nvcsw = 50, .nivcsw = 10,
        .user_cpu_s = 1.234, .sys_cpu_s = 0.567, .total_cpu_s = 1.801,
        .maxrss_kb = 32768, .rss_kb = 16384, .thread_count = 8,
    };
    for (auto _ : state) {
        auto s = snap.dump();
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SystemResourceStatsDump);

// ---------------------------------------------------------------------------
// SystemResourceStats::to_json() — format to JSON string
// ---------------------------------------------------------------------------

static void BM_SystemResourceStatsToJson(benchmark::State& state) {
    SystemResourceStats snap{
        .majflt = 2, .minflt = 500, .nvcsw = 50, .nivcsw = 10,
        .user_cpu_s = 1.234, .sys_cpu_s = 0.567, .total_cpu_s = 1.801,
        .maxrss_kb = 32768, .rss_kb = 16384, .thread_count = 8,
    };
    for (auto _ : state) {
        auto s = snap.to_json();
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SystemResourceStatsToJson);

// ---------------------------------------------------------------------------
// std::format for SystemResourceStats
// ---------------------------------------------------------------------------

static void BM_SystemResourceStatsFormat(benchmark::State& state) {
    SystemResourceStats snap{
        .majflt = 2, .minflt = 500, .nvcsw = 50, .nivcsw = 10,
        .user_cpu_s = 1.234, .sys_cpu_s = 0.567, .total_cpu_s = 1.801,
        .maxrss_kb = 32768, .rss_kb = 16384, .thread_count = 8,
    };
    for (auto _ : state) {
        auto s = std::format("{}", snap);
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SystemResourceStatsFormat);

BENCHMARK_MAIN();
