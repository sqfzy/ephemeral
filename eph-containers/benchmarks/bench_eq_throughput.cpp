#include <benchmark/benchmark.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "bench_matrix.hpp"
#include "eph/containers/evicting_queue.hpp"
#include "eph/utils/cpu.hpp"

using eph::containers::EvictingQueue;
using namespace eph::utils;

// ============================================================================
// 辅助工具
// ============================================================================
template <size_t Bytes>
struct alignas(64) Payload {
    uint8_t data[Bytes];
    Payload() { std::memset(data, 0, Bytes); }
};

// ============================================================================
// 多线程吞吐量与交付率测试
// ============================================================================
template <size_t PayloadSize, size_t BufSize>
void Throughput(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    static std::unique_ptr<EvictingQueue<T, BufSize>> q;
    static std::atomic<uint64_t> total_pops;

    auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (state.thread_index() == 0) {
        q = std::make_unique<EvictingQueue<T, BufSize>>();
        total_pops = 0;
    }

    if (state.thread_index() == 0) {
        // --- 生产者 ---
        (void)pin_thread(topology[0].hw_thread_id);
        T val;
        for (auto _ : state) {
            q->push(val);  //
        }
    } else {
        // --- 消费者 ---
        (void)pin_thread(topology[1].hw_thread_id);
        T out;
        uint64_t local_pops = 0;
        for (auto _ : state) {
            if (q->try_pop_latest(out)) {
                local_pops++;
                benchmark::DoNotOptimize(out.data[0]);
            } else {
                cpu_relax();
            }
        }
        total_pops.store(local_pops, std::memory_order_relaxed);
    }

    // 2. 统计上报
    if (state.thread_index() == 0) {
        uint64_t iterations = state.iterations();
        state.counters["ActualPops"] =
            benchmark::Counter(total_pops.load(), benchmark::Counter::kIsRate);
        state.counters["Pushes"] =
            benchmark::Counter(iterations, benchmark::Counter::kIsRate);
        state.counters["Delivery%"] =
            (iterations > 0) ? (double)total_pops.load() / iterations * 100.0
                             : 0.0;
    }
}

template <size_t PayloadSize, size_t BufSize>
void ZeroCopy(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    static std::unique_ptr<EvictingQueue<T, BufSize>> q;
    static std::atomic<uint64_t> total_pops;

    auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (state.thread_index() == 0) {
        q = std::make_unique<EvictingQueue<T, BufSize>>();
        total_pops = 0;
    }

    if (state.thread_index() == 0) {
        (void)pin_thread(topology[0].hw_thread_id);
        auto writer = [](T& slot) { slot.data[0] = 1; };
        for (auto _ : state) {
            q->produce(writer);
        }
    } else {
        (void)pin_thread(topology[1].hw_thread_id);
        uint64_t local_pops = 0;
        auto reader = [](const T& slot) {
            uint8_t d = slot.data[0];
            benchmark::DoNotOptimize(d);
        };

        for (auto _ : state) {
            if (q->try_consume_latest(reader)) {
                local_pops++;
            } else {
                cpu_relax();
            }
        }
        total_pops.store(local_pops, std::memory_order_relaxed);
    }

    if (state.thread_index() == 0) {
        uint64_t iterations = state.iterations();
        state.counters["ActualPops"] =
            benchmark::Counter(total_pops.load(), benchmark::Counter::kIsRate);
        state.counters["Pushes"] =
            benchmark::Counter(iterations, benchmark::Counter::kIsRate);
        state.counters["Delivery%"] =
            (iterations > 0) ? (double)total_pops.load() / iterations * 100.0
                             : 0.0;
    }
}

REGISTER_MATRIX(Throughput, ->Threads(2)->UseRealTime());
REGISTER_MATRIX(ZeroCopy, ->Threads(2)->UseRealTime());

BENCHMARK_MAIN();
