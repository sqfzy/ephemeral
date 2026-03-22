#include <benchmark/benchmark.h>

#include <cstdlib>
#include <cstring>

#include "../common.h"
#include "eph/containers/evicting_queue.hpp"

using eph::containers::EvictingQueue;
using namespace eph::utils;

// ============================================================================
// 辅助工具：定长载荷 (Payload)
// ============================================================================

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

// ============================================================================
// 单线程 Push/Pop Overhead
// ============================================================================

template <size_t PayloadSize, size_t BufSize>
static void BM_EvictingQueue_Push(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    EvictingQueue<T, BufSize> rb;
    T in_val;

    for ([[maybe_unused]] auto _ : state) {
        rb.push(in_val);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}

template <size_t PayloadSize, size_t BufSize>
static void BM_EvictingQueue_PushPop(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    EvictingQueue<T, BufSize> rb;
    T in_val;
    T out_val;

    for ([[maybe_unused]] auto _ : state) {
        rb.push(in_val);
        rb.pop_latest(out_val);

        benchmark::DoNotOptimize(out_val);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

template <size_t PayloadSize, size_t BufSize>
static void BM_EvictingQueue_ZeroCopy_PushPop(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    EvictingQueue<T, BufSize> rb;

    for ([[maybe_unused]] auto _ : state) {
        rb.produce([](T& slot) {
            if constexpr (PayloadSize == 8)
                slot.v = 1;
            else
                slot.data[0] = 1;
        });

        rb.consume_latest([](const T& slot) {
            if constexpr (PayloadSize == 8) {
                auto v = slot.v;
                benchmark::DoNotOptimize(v);
            } else {
                auto d = slot.data[0];
                benchmark::DoNotOptimize(d);
            }
        });

        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

// 单线程 Push/Pop
REGISTER_MATRIX(BM_EvictingQueue_Push);
REGISTER_MATRIX(BM_EvictingQueue_PushPop);
REGISTER_MATRIX(BM_EvictingQueue_ZeroCopy_PushPop);

BENCHMARK_MAIN();
