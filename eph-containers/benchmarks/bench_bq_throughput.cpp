#include <benchmark/benchmark.h>
#include <cstring>
#include <vector>

#include "eph/containers/bounded_queue.hpp"
#include "bench_matrix.hpp"
#include "eph/utils/cpu.hpp"

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
// BoundedQueue_Throughput<PayloadSize, BufSize>
// 场景：SPSC 饱和吞吐量测试
// ============================================================================

template <size_t PayloadSize, size_t BufSize>
static void BM_BoundedQueue_Throughput(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    static BoundedQueue<T, BufSize> q;
    static auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (topology.size() < 2) {
        state.SkipWithError("Need at least 2 cores");
        return;
    }

    if (state.thread_index() == 0) {
        // 生产者线程
        (void)set_thread_affinity(topology[0].hw_thread_id);
        T val;
        for ([[maybe_unused]] auto _ : state) {
            q.push(val);
            benchmark::ClobberMemory();
        }
    } else {
        // 消费者线程
        (void)set_thread_affinity(topology[1].hw_thread_id);
        T out;
        for ([[maybe_unused]] auto _ : state) {
            q.pop(out);
            benchmark::DoNotOptimize(out);
        }
    }

    state.SetItemsProcessed(state.iterations());
}

template <size_t PayloadSize, size_t BufSize>
static void BM_BoundedQueue_ZeroCopy_Throughput(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    static BoundedQueue<T, BufSize> q;
    static auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (topology.size() < 2) {
        state.SkipWithError("Need at least 2 cores");
        return;
    }

    if (state.thread_index() == 0) {
        (void)set_thread_affinity(topology[0].hw_thread_id);
        auto writer = [](T& slot) {
            if constexpr (PayloadSize == 8)
                slot.v = 1;
            else
                slot.data[0] = 1;
        };
        for ([[maybe_unused]] auto _ : state) {
            q.produce(writer);
        }
    } else {
        (void)set_thread_affinity(topology[1].hw_thread_id);
        auto reader = [](const T& slot) {
            if constexpr (PayloadSize == 8) {
                auto v = slot.v;
                benchmark::DoNotOptimize(v);
            } else {
                auto d = slot.data[0];
                benchmark::DoNotOptimize(d);
            }
        };
        for ([[maybe_unused]] auto _ : state) {
            q.consume(reader);
        }
    }
    state.SetItemsProcessed(state.iterations());
}


// 多线程 SPSC 吞吐量
REGISTER_MATRIX(BM_BoundedQueue_Throughput, ->Threads(2)->UseRealTime());
REGISTER_MATRIX(
    BM_BoundedQueue_ZeroCopy_Throughput, ->Threads(2)->UseRealTime());

BENCHMARK_MAIN();
