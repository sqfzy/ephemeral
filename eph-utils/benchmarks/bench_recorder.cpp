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

BENCHMARK_MAIN();
