/// @file bench_recorder.cpp
/// Benchmarks for Recorder and ConcurrentRecorder — record() throughput.
/// These are used on the measurement hot path for latency profiling.

#include <cstdint>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/utils/recorder.hpp"

using namespace eph::utils;

// ---------------------------------------------------------------------------
// Recorder::record() — single-threaded recording throughput
// ---------------------------------------------------------------------------

static void BM_RecorderRecord(benchmark::State& state) {
    Recorder rec("bench");
    uint64_t cycles = 100;
    for (auto _ : state) {
        auto ok = rec.record(cycles);
        benchmark::DoNotOptimize(ok);
        cycles = (cycles + 7) & 0xFFFFFF;  // vary cycles, stay in range
        if (cycles == 0) cycles = 1;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RecorderRecord);

// ---------------------------------------------------------------------------
// Recorder::record_values() — batch recording
// ---------------------------------------------------------------------------

static void BM_RecorderRecordValues(benchmark::State& state) {
    Recorder rec("bench_batch");
    uint64_t cycles = 500;
    for (auto _ : state) {
        auto ok = rec.record_values(cycles, 10);
        benchmark::DoNotOptimize(ok);
        cycles = (cycles + 13) & 0xFFFFFF;
        if (cycles == 0) cycles = 1;
    }
    state.SetItemsProcessed(state.iterations() * 10);
}
BENCHMARK(BM_RecorderRecordValues);

// ---------------------------------------------------------------------------
// ConcurrentRecorder::record() — single thread (no contention baseline)
// ---------------------------------------------------------------------------

static void BM_ConcurrentRecorderRecord_1Thread(benchmark::State& state) {
    ConcurrentRecorder rec("bench_concurrent");
    uint64_t cycles = 100;
    for (auto _ : state) {
        auto ok = rec.record(cycles);
        benchmark::DoNotOptimize(ok);
        cycles = (cycles + 7) & 0xFFFFFF;
        if (cycles == 0) cycles = 1;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConcurrentRecorderRecord_1Thread);

// ---------------------------------------------------------------------------
// ConcurrentRecorder::record() — multi-threaded
// ---------------------------------------------------------------------------

static void BM_ConcurrentRecorderRecord_MT(benchmark::State& state) {
    static ConcurrentRecorder rec("bench_mt");
    uint64_t cycles = 100 + state.thread_index() * 1000;
    for (auto _ : state) {
        auto ok = rec.record(cycles);
        benchmark::DoNotOptimize(ok);
        cycles = (cycles + 7) & 0xFFFFFF;
        if (cycles == 0) cycles = 1;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConcurrentRecorderRecord_MT)->Threads(1)->Threads(2)->Threads(4);

// ---------------------------------------------------------------------------
// Recorder::compute_stats() — the per-bench-run output critical path
// ---------------------------------------------------------------------------
//
// Coverage gap (batch 11): every benchmark scenario (`lat_tcp`, `lat_udp`,
// `lat_ws`, the four `ex_*` scenarios) ends with a `Recorder::compute_stats()`
// + `export_json` pair to write its run results. That step traverses the
// HdrHistogram to extract {p50, p90, p99, p99.9} + std-dev. With sample
// counts in the 10-100k range per run, this isn't free — pinning the cost
// surfaces any future regression in the histogram percentile-walk.

static void BM_RecorderComputeStats(benchmark::State& state) {
    Recorder rec("bench_compute_stats");
    // Pre-load with state.range(0) samples spanning a realistic latency
    // distribution (a noisy ~100 ns floor up to a 100 us tail).
    const auto n_samples = static_cast<uint64_t>(state.range(0));
    for (uint64_t i = 0; i < n_samples; ++i) {
        const uint64_t cycles = 200 + (i * 17) % 200000;
        (void)rec.record(cycles);
    }
    for (auto _ : state) {
        auto stats = rec.compute_stats();
        benchmark::DoNotOptimize(stats);
    }
}
BENCHMARK(BM_RecorderComputeStats)->Arg(1000)->Arg(10000)->Arg(100000);

BENCHMARK_MAIN();
