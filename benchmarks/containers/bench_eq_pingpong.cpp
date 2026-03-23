#include <benchmark/benchmark.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "../common.h"
#include "eph/containers/evicting_queue.hpp"
#include "eph/utils/cpu.hpp"

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
// 多线程 Ping-Pong
// ============================================================================

template <typename T, size_t BufSize>
struct alignas(64) PingPongContext {
    EvictingQueue<T, BufSize> q1;  // Thread 1 -> Thread 2
    char padding[64];
    EvictingQueue<T, BufSize> q2;  // Thread 2 -> Thread 1
};

template <size_t PayloadSize, size_t BufSize>
static void BM_EvictingQueue_PingPong(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    static PingPongContext<T, BufSize>* ctx = new PingPongContext<T, BufSize>();
    static auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (topology.size() < 2) {
        state.SkipWithError("Need at least 2 cores");
        return;
    }

    if (state.thread_index() == 0) {
        set_thread_affinity(topology[0].hw_thread_id);
        T send_val;
        T recv_val;
        for ([[maybe_unused]] auto _ : state) {
            ctx->q1.push(send_val);
            ctx->q2.pop_latest(recv_val);
        }
    } else {
        set_thread_affinity(topology[1].hw_thread_id);
        T val;
        for ([[maybe_unused]] auto _ : state) {
            ctx->q1.pop_latest(val);
            ctx->q2.push(val);
        }
    }
}

template <size_t PayloadSize, size_t BufSize>
static void BM_EvictingQueue_ZeroCopy_PingPong(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    static PingPongContext<T, BufSize>* ctx = new PingPongContext<T, BufSize>();
    static auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (topology.size() < 2) {
        state.SkipWithError("Need at least 2 cores");
        return;
    }

    auto writer = [](T& slot) {
        if constexpr (PayloadSize == 8)
            slot.v = 1;
        else
            slot.data[0] = 1;
    };

    auto reader = [](const T& slot) {
        if constexpr (PayloadSize == 8) {
            auto v = slot.v;
            benchmark::DoNotOptimize(v);
        } else {
            auto d = slot.data[0];
            benchmark::DoNotOptimize(d);
        }
    };

    if (state.thread_index() == 0) {
        set_thread_affinity(topology[0].hw_thread_id);
        for ([[maybe_unused]] auto _ : state) {
            ctx->q1.produce(writer);
            ctx->q2.consume_latest(reader);  // 阻塞式 Read
        }
    } else {
        set_thread_affinity(topology[1].hw_thread_id);
        for ([[maybe_unused]] auto _ : state) {
            ctx->q1.consume_latest(reader);
            ctx->q2.produce(writer);
        }
    }
}


// 多线程 Ping-Pong 延迟
REGISTER_MATRIX(BM_EvictingQueue_PingPong, ->Threads(2)->UseRealTime());
REGISTER_MATRIX(
    BM_EvictingQueue_ZeroCopy_PingPong, ->Threads(2)->UseRealTime());

BENCHMARK_MAIN();
