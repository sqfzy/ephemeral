/// @file bench_hdr_histogram.cpp
/// Benchmarks for HdrHistogram — record and percentile query throughput.
/// Important for latency measurement in hot loops.

#include <cstdint>
#include <random>

#include <benchmark/benchmark.h>

#include "eph/utils/hdr_histogram.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// Recording benchmarks
// ---------------------------------------------------------------------------

// Record a constant value (best case — same bucket every time)
static void BM_HdrRecord_Constant(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);
    for (auto _ : state) {
        (void)h.record(1000);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrRecord_Constant);

// Record sequentially increasing values (traverses many sub-buckets)
static void BM_HdrRecord_Sequential(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);
    uint64_t val = 1;
    for (auto _ : state) {
        (void)h.record(val);
        val = (val < 1'000'000) ? val + 1 : 1;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrRecord_Sequential);

// Record pre-generated random values (realistic latency distribution)
static void BM_HdrRecord_Random(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);

    // Pre-generate random values to avoid RNG overhead in the loop
    constexpr size_t kSize = 8192;
    std::vector<uint64_t> values(kSize);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(1, 1'000'000);
    for (auto& v : values) v = dist(rng);

    size_t idx = 0;
    for (auto _ : state) {
        (void)h.record(values[idx]);
        idx = (idx + 1) & (kSize - 1);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrRecord_Random);

// ---------------------------------------------------------------------------
// Percentile query benchmarks
// ---------------------------------------------------------------------------

// Query p50/p99/p999 from a populated histogram
static void BM_HdrPercentile_P50(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);
    // Populate with realistic latency data
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(100, 100'000);
    for (int i = 0; i < 100'000; ++i) (void)h.record(dist(rng));

    for (auto _ : state) {
        auto v = h.get_value_at_percentile(50.0);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrPercentile_P50);

static void BM_HdrPercentile_P99(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(100, 100'000);
    for (int i = 0; i < 100'000; ++i) (void)h.record(dist(rng));

    for (auto _ : state) {
        auto v = h.get_value_at_percentile(99.0);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrPercentile_P99);

static void BM_HdrPercentile_P999(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(100, 100'000);
    for (int i = 0; i < 100'000; ++i) (void)h.record(dist(rng));

    for (auto _ : state) {
        auto v = h.get_value_at_percentile(99.9);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrPercentile_P999);

// ---------------------------------------------------------------------------
// Statistics benchmarks
// ---------------------------------------------------------------------------

static void BM_HdrMean(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(100, 100'000);
    for (int i = 0; i < 100'000; ++i) (void)h.record(dist(rng));

    for (auto _ : state) {
        auto v = h.get_mean();
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrMean);

static void BM_HdrReset(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(100, 100'000);

    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < 10'000; ++i) (void)h.record(dist(rng));
        state.ResumeTiming();
        h.reset();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrReset);

// ---------------------------------------------------------------------------
// Batch percentile query
// ---------------------------------------------------------------------------

static void BM_HdrGetPercentiles_Batch(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(100, 100'000);
    for (int i = 0; i < 100'000; ++i) (void)h.record(dist(rng));

    std::vector<double> pcts = {50.0, 90.0, 99.0, 99.9, 99.99};
    for (auto _ : state) {
        auto v = h.get_percentiles(pcts);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrGetPercentiles_Batch);

// ---------------------------------------------------------------------------
// Report and JSON formatting
// ---------------------------------------------------------------------------

static void BM_HdrReport(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(100, 100'000);
    for (int i = 0; i < 100'000; ++i) (void)h.record(dist(rng));

    for (auto _ : state) {
        auto s = h.report("Latency", "ns");
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrReport);

static void BM_HdrToJson(benchmark::State& state) {
    HdrHistogram h(1, 3'600'000'000ULL, 3);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> dist(100, 100'000);
    for (int i = 0; i < 100'000; ++i) (void)h.record(dist(rng));

    for (auto _ : state) {
        auto s = h.to_json();
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrToJson);

// ---------------------------------------------------------------------------
// for_each_linear — high-offset histogram (tests skip optimization)
// ---------------------------------------------------------------------------

static void BM_HdrForEachLinear_HighOffset(benchmark::State& state) {
    HdrHistogram h(1, 1'000'000, 3);
    // Values start at 100'000 — tests the optimization of skipping empty prefix
    for (uint64_t v = 100'000; v <= 100'100; ++v) {
        (void)h.record(v);
    }

    for (auto _ : state) {
        uint64_t total = 0;
        h.for_each_linear(10, [&](const HdrHistogram::LinearEntry& e) {
            total += e.count;
        });
        benchmark::DoNotOptimize(total);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HdrForEachLinear_HighOffset);

BENCHMARK_MAIN();
