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
// BoundedQueue_PingPong<PayloadSize, BufSize>
// 场景：SPSC 跨核心延迟 (Latency)
//
// 结构：T1 Push -> Q1 -> T2 Pop -> Q2 -> T1 Pop
// 测量缓存一致性协议 (MESI) 下的 Cache Miss 和总线通信延迟。
// ============================================================================

template <typename T, size_t BufSize>
struct alignas(64) PingPongContext {
    // BufSize 现在随矩阵参数变化
    BoundedQueue<T, BufSize> q1;  // Thread 1 -> Thread 2
    char padding[64];                  // 强制隔离，防止伪共享 (False Sharing)
    BoundedQueue<T, BufSize> q2;  // Thread 2 -> Thread 1
};

template <size_t PayloadSize, size_t BufSize>
static void BM_BoundedQueue_PingPong(benchmark::State& state) {
    using T = Payload<PayloadSize>;

    // 静态分配，避免栈溢出并在多次迭代间复用
    static PingPongContext<T, BufSize>* ctx = new PingPongContext<T, BufSize>();
    static auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (topology.size() < 2) {
        state.SkipWithError("Need at least 2 cores for PingPong test");
        return;
    }

    // 线程 0：Initiator (Ping)
    if (state.thread_index() == 0) {
        (void)set_thread_affinity(topology[0].hw_thread_id);

        T send_val;
        T recv_val;

        for ([[maybe_unused]] auto _ : state) {
            // Step 1: Send to T2
            ctx->q1.push(send_val);

            // Step 4: Wait for echo from T2
            ctx->q2.pop(recv_val);
        }
    }
    // 线程 1：Echoer (Pong)
    else {
        (void)set_thread_affinity(topology[1].hw_thread_id);

        T val;
        for ([[maybe_unused]] auto _ : state) {
            // Step 2: Wait for data from T1
            ctx->q1.pop(val);

            // Step 3: Echo back to T1
            ctx->q2.push(val);
        }
    }
}

template <size_t PayloadSize, size_t BufSize>
static void BM_BoundedQueue_ZeroCopy_PingPong(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    static PingPongContext<T, BufSize>* ctx = new PingPongContext<T, BufSize>();
    static auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (topology.size() < 2) {
        state.SkipWithError("Need at least 2 cores");
        return;
    }

    // 辅助 Lambda：模拟最小化写入
    auto writer = [](T& slot) {
        if constexpr (PayloadSize == 8)
            slot.v = 1;
        else
            slot.data[0] = 1;
    };
    // 辅助 Lambda：模拟最小化读取
    auto reader = [](const T& slot) {
        if constexpr (PayloadSize == 8)
            benchmark::DoNotOptimize(slot.v);
        else
            benchmark::DoNotOptimize(slot.data[0]);
    };

    if (state.thread_index() == 0) {
        (void)set_thread_affinity(topology[0].hw_thread_id);
        for ([[maybe_unused]] auto _ : state) {
            ctx->q1.produce(writer);  // Block wait
            ctx->q2.consume(reader);  // Block wait
        }
    } else {
        (void)set_thread_affinity(topology[1].hw_thread_id);
        for ([[maybe_unused]] auto _ : state) {
            ctx->q1.consume(reader);
            ctx->q2.produce(writer);
        }
    }
}



// 多线程 Ping-Pong 延迟
REGISTER_MATRIX(BM_BoundedQueue_PingPong, ->Threads(2)->UseRealTime());
REGISTER_MATRIX(BM_BoundedQueue_ZeroCopy_PingPong, ->Threads(2)->UseRealTime());


BENCHMARK_MAIN();
