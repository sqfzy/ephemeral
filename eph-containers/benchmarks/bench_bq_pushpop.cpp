#include <benchmark/benchmark.h>

#include <cstring>

#include "bench_matrix.hpp"
#include "eph/containers/bounded_queue.hpp"

using eph::containers::BoundedQueue;
using namespace eph::utils;

// ============================================================================
// 辅助工具：定长载荷 (Payload)
// ============================================================================

template <size_t Bytes>
struct alignas(8) Payload {
    uint8_t data[Bytes];

    // 提供默认构造，防止编译器认为变量未初始化而优化掉
    Payload() { std::memset(data, 0, Bytes); }
};

// 特化：对于 8 字节，直接模拟 uint64_t，更符合寄存器传参场景
template <>
struct alignas(8) Payload<8> {
    uint64_t v;
    Payload() : v(0) {}
};

// ============================================================================
//  BoundedQueue_PushPop<PayloadSize, BufSize>
// 场景：单线程操作开销
// ============================================================================

template <size_t PayloadSize, size_t BufSize>
static void BM_BoundedQueue_PushPop(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    BoundedQueue<T, BufSize> q;
    T val;
    T out;

    for ([[maybe_unused]] auto _ : state) {
        q.push(val);
        q.pop(out);

        benchmark::DoNotOptimize(out);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * 2);
}

template <size_t PayloadSize, size_t BufSize>
static void BM_BoundedQueue_ZeroCopy_PushPop(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    BoundedQueue<T, BufSize> q;

    for ([[maybe_unused]] auto _ : state) {
        q.produce([](T& slot) {
            if constexpr (PayloadSize == 8)
                slot.v = 1;
            else
                slot.data[0] = 1;
        });

        q.consume([](const T& slot) {
            if constexpr (PayloadSize == 8)
                benchmark::DoNotOptimize(slot.v);
            else
                benchmark::DoNotOptimize(slot.data[0]);
        });

        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * 2);
}

// 单线程 Push/Pop
REGISTER_MATRIX(BM_BoundedQueue_PushPop);
REGISTER_MATRIX(BM_BoundedQueue_ZeroCopy_PushPop);

BENCHMARK_MAIN();
