/// @file bench_bq_batch.cpp
/// Benchmark for BoundedQueue batch operations vs single-element operations.
///
/// Compares:
///   - try_push_n / try_consume_n  (batch span/visitor)
///   - try_produce_n / try_consume_n  (batch zero-copy)
///   - push / pop loop  (baseline single-element)
///
/// Parameters: PayloadSize × BatchSize

#include <cstring>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/containers/bounded_queue.hpp"

using eph::containers::BoundedQueue;

// ─────────────────────────────────────────────────────────────────────────────
// Payload types
// ─────────────────────────────────────────────────────────────────────────────

template <size_t Bytes>
struct alignas(8) Payload {
    uint8_t data[Bytes];
    Payload() { std::memset(data, 0, Bytes); }
};

template <>
struct alignas(8) Payload<8> {
    uint64_t v;
    Payload() : v(0) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Single-element baseline: push N items one at a time, then pop N one at a time
// ─────────────────────────────────────────────────────────────────────────────

template <size_t PayloadSize>
static void BM_BQ_Single_PushPop(benchmark::State& state) {
    constexpr size_t kQueueCap = 1024;
    using T = Payload<PayloadSize>;
    BoundedQueue<T, kQueueCap> q;

    auto batch_n = static_cast<size_t>(state.range(0));
    T val{};
    T out{};

    for ([[maybe_unused]] auto _ : state) {
        // Push N items one by one
        for (size_t i = 0; i < batch_n; ++i) {
            q.push(val);
        }
        // Pop N items one by one
        for (size_t i = 0; i < batch_n; ++i) {
            q.pop(out);
        }
        benchmark::DoNotOptimize(out);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch_n) * 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Batch span: try_push_n + try_consume_n (visitor)
// ─────────────────────────────────────────────────────────────────────────────

template <size_t PayloadSize>
static void BM_BQ_Batch_PushN_ConsumeN(benchmark::State& state) {
    constexpr size_t kQueueCap = 1024;
    using T = Payload<PayloadSize>;
    BoundedQueue<T, kQueueCap> q;

    auto batch_n = static_cast<size_t>(state.range(0));
    std::vector<T> items(batch_n);

    for ([[maybe_unused]] auto _ : state) {
        // Batch push via span
        q.push_n(std::span<const T>(items.data(), batch_n));

        // Batch consume via visitor.
        // Local copy + non-const & for benchmark::DoNotOptimize: the
        // const-ref overload is deprecated in google/benchmark because
        // the compiler can elide loads of the underlying storage; a
        // non-const reference forces the value into a register and is
        // the recommended replacement.
        size_t consumed = 0;
        while (consumed < batch_n) {
            consumed += q.try_consume_n(batch_n - consumed,
                [](const T& slot, [[maybe_unused]] size_t idx) {
                    T copy = slot;
                    benchmark::DoNotOptimize(copy);
                });
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch_n) * 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Batch zero-copy: try_produce_n + try_consume_n
// ─────────────────────────────────────────────────────────────────────────────

template <size_t PayloadSize>
static void BM_BQ_Batch_ProduceN_ConsumeN(benchmark::State& state) {
    constexpr size_t kQueueCap = 1024;
    using T = Payload<PayloadSize>;
    BoundedQueue<T, kQueueCap> q;

    auto batch_n = static_cast<size_t>(state.range(0));

    for ([[maybe_unused]] auto _ : state) {
        // Batch produce via zero-copy visitor
        q.produce_n(batch_n, [](T& slot, [[maybe_unused]] size_t idx) {
            if constexpr (PayloadSize == 8)
                slot.v = idx;
            else
                slot.data[0] = static_cast<uint8_t>(idx);
        });

        // Batch consume via visitor.
        // Local copy + non-const & for benchmark::DoNotOptimize: the
        // const-ref overload is deprecated in google/benchmark because
        // the compiler can elide loads of the underlying storage; a
        // non-const reference forces the value into a register and is
        // the recommended replacement.
        size_t consumed = 0;
        while (consumed < batch_n) {
            consumed += q.try_consume_n(batch_n - consumed,
                [](const T& slot, [[maybe_unused]] size_t idx) {
                    T copy = slot;
                    benchmark::DoNotOptimize(copy);
                });
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(batch_n) * 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration: PayloadSize × BatchSize
// ─────────────────────────────────────────────────────────────────────────────

static void BatchSizeArgs(::benchmark::Benchmark* b) {
    for (int n : {1, 4, 16, 64}) b->Arg(n);
}

// Payload = 8 bytes
BENCHMARK(BM_BQ_Single_PushPop<8>)->Apply(BatchSizeArgs)->Name("BQ_Single/P:8");
BENCHMARK(BM_BQ_Batch_PushN_ConsumeN<8>)->Apply(BatchSizeArgs)->Name("BQ_BatchSpan/P:8");
BENCHMARK(BM_BQ_Batch_ProduceN_ConsumeN<8>)->Apply(BatchSizeArgs)->Name("BQ_BatchZeroCopy/P:8");

// Payload = 64 bytes
BENCHMARK(BM_BQ_Single_PushPop<64>)->Apply(BatchSizeArgs)->Name("BQ_Single/P:64");
BENCHMARK(BM_BQ_Batch_PushN_ConsumeN<64>)->Apply(BatchSizeArgs)->Name("BQ_BatchSpan/P:64");
BENCHMARK(BM_BQ_Batch_ProduceN_ConsumeN<64>)->Apply(BatchSizeArgs)->Name("BQ_BatchZeroCopy/P:64");

// Payload = 512 bytes
BENCHMARK(BM_BQ_Single_PushPop<512>)->Apply(BatchSizeArgs)->Name("BQ_Single/P:512");
BENCHMARK(BM_BQ_Batch_PushN_ConsumeN<512>)->Apply(BatchSizeArgs)->Name("BQ_BatchSpan/P:512");
BENCHMARK(BM_BQ_Batch_ProduceN_ConsumeN<512>)->Apply(BatchSizeArgs)->Name("BQ_BatchZeroCopy/P:512");

BENCHMARK_MAIN();
