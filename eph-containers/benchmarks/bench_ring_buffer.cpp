/// @file bench_ring_buffer.cpp
/// RingBuffer benchmarks -- push throughput, random access latency.
///
/// Measures hot-path performance of the RingBuffer used for tick history
/// lookback in HFT signal pipelines.

#include <cstdint>
#include <random>

#include <benchmark/benchmark.h>

#include "eph/containers/ring_buffer.hpp"

using eph::containers::RingBuffer;

namespace {

struct Tick {
    double price;
    double qty;
    uint64_t timestamp;
};

} // namespace

// ---------------------------------------------------------------------------
// Push throughput (single-threaded writer)
// ---------------------------------------------------------------------------

static void BM_RingBuffer_Push_64(benchmark::State& state) {
    RingBuffer<Tick, 64> rb;
    Tick tick{150.25, 100.0, 1000000};
    for (auto _ : state) {
        rb.push(tick);
        benchmark::DoNotOptimize(rb.count());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBuffer_Push_64);

static void BM_RingBuffer_Push_1024(benchmark::State& state) {
    RingBuffer<Tick, 1024> rb;
    Tick tick{150.25, 100.0, 1000000};
    for (auto _ : state) {
        rb.push(tick);
        benchmark::DoNotOptimize(rb.count());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBuffer_Push_1024);

static void BM_RingBuffer_Push_16384(benchmark::State& state) {
    RingBuffer<Tick, 16384> rb;
    Tick tick{150.25, 100.0, 1000000};
    for (auto _ : state) {
        rb.push(tick);
        benchmark::DoNotOptimize(rb.count());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBuffer_Push_16384);

// ---------------------------------------------------------------------------
// Random access latency (at() with varying lookback depth)
// ---------------------------------------------------------------------------

static void BM_RingBuffer_At_Front(benchmark::State& state) {
    RingBuffer<Tick, 1024> rb;
    for (size_t i = 0; i < 1024; ++i)
        rb.push({static_cast<double>(i), 1.0, i});

    for (auto _ : state) {
        auto val = rb.at(0);  // Most recent
        benchmark::DoNotOptimize(val);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBuffer_At_Front);

static void BM_RingBuffer_At_Deep(benchmark::State& state) {
    RingBuffer<Tick, 1024> rb;
    for (size_t i = 0; i < 1024; ++i)
        rb.push({static_cast<double>(i), 1.0, i});

    for (auto _ : state) {
        auto val = rb.at(500);  // 500 ticks ago
        benchmark::DoNotOptimize(val);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBuffer_At_Deep);

static void BM_RingBuffer_At_Back(benchmark::State& state) {
    RingBuffer<Tick, 1024> rb;
    for (size_t i = 0; i < 1024; ++i)
        rb.push({static_cast<double>(i), 1.0, i});

    for (auto _ : state) {
        auto val = rb.at(1023);  // Oldest element
        benchmark::DoNotOptimize(val);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBuffer_At_Back);

// ---------------------------------------------------------------------------
// Lookback pattern: scan last N ticks (e.g., for VWAP)
// ---------------------------------------------------------------------------

static void BM_RingBuffer_Scan_Last_N(benchmark::State& state) {
    constexpr size_t kCapacity = 1024;
    RingBuffer<Tick, kCapacity> rb;
    for (size_t i = 0; i < kCapacity; ++i)
        rb.push({100.0 + static_cast<double>(i % 100) * 0.01, 1.0, i});

    const size_t scan_depth = static_cast<size_t>(state.range(0));

    for (auto _ : state) {
        double sum = 0.0;
        for (size_t i = 0; i < scan_depth; ++i) {
            auto val = rb.at(i);
            if (val) sum += val->price;
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * scan_depth);
}
BENCHMARK(BM_RingBuffer_Scan_Last_N)
    ->Arg(10)    // Scan last 10 ticks
    ->Arg(50)    // Scan last 50 ticks
    ->Arg(100)   // Scan last 100 ticks
    ->Arg(500);  // Scan last 500 ticks

BENCHMARK_MAIN();
