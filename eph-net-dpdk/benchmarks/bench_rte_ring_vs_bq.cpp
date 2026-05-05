/// @file bench_rte_ring_vs_bq.cpp
/// Compare DPDK rte_ring vs BoundedQueue in SPSC mode across 5 scenarios.
///
/// Both queues run in the same process to eliminate systematic bias.
/// rte_ring is configured with RING_F_SP_ENQ | RING_F_SC_DEQ (SPSC mode).
/// BoundedQueue is already SPSC by design.
///
/// Scenarios:
///   1. PushPop    -- single-thread push+pop round-trip latency
///   2. Throughput  -- 2-thread saturated producer/consumer throughput
///   3. PingPong    -- 2-thread cross-core RTT (cache-line bounce)
///   4. Batch       -- bulk enqueue/dequeue amortised cost
///   5. QueueFull   -- failure-path latency when queue is full

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include <rte_config.h>
#include <rte_eal.h>
#include <rte_ring.h>

#include "bench_helpers.hpp"
#include "eph/containers/bounded_queue.hpp"
#include "eph/utils/cpu.hpp"

#include "bench_matrix.hpp"

using eph::containers::BoundedQueue;
using namespace eph::utils;

// ============================================================================
// EAL initialisation -- minimal, no hugepages, single lcore
// ============================================================================

static struct EalInit {
    EalInit() {
        const char* argv[] = {"bench", "--no-huge", "--no-shconf", "-l", "0", nullptr};
        rte_eal_init(5, const_cast<char**>(argv));
    }
} g_eal_init;

// ============================================================================
// Payload -- fixed-size data block, matching existing bench conventions
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
// rte_ring helpers
// ============================================================================

/// Unique counter to produce distinct ring names (rte_ring_create requires unique names).
static std::atomic<uint64_t> g_ring_counter{0};

/// Create a SPSC rte_ring with power-of-two capacity.
/// rte_ring internally rounds up to next power of two, but our BufSize
/// template parameters are already powers of two.
static rte_ring* create_spsc_ring(unsigned capacity) {
    auto name = std::string("bm_ring_") + std::to_string(g_ring_counter.fetch_add(1));
    // rte_ring actual usable slots = requested_count - 1 for the classic
    // implementation, so request capacity+1 to get exactly `capacity` usable
    // slots. However, rte_ring_create rounds up to next power of two anyway.
    // To keep things comparable (both queues have the same effective depth),
    // we request 2*capacity which guarantees >= capacity usable slots.
    auto* ring = rte_ring_create(name.c_str(), capacity * 2,
                                  SOCKET_ID_ANY,
                                  RING_F_SP_ENQ | RING_F_SC_DEQ);
    return ring;
}

// ============================================================================
// 1. PushPop -- single-thread push+pop round-trip
// ============================================================================

template <size_t PayloadSize, size_t BufSize>
static void BM_BQ_PushPop(benchmark::State& state) {
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
static void BM_RteRing_PushPop(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    auto* ring = create_spsc_ring(BufSize);
    if (!ring) {
        state.SkipWithError("rte_ring_create failed");
        return;
    }

    // Pre-allocated payload pool to avoid malloc on hot path
    T pool[BufSize > 0 ? BufSize : 1];
    size_t pool_idx = 0;

    for ([[maybe_unused]] auto _ : state) {
        T* p = &pool[pool_idx & (BufSize - 1)];
        ++pool_idx;
        rte_ring_sp_enqueue(ring, static_cast<void*>(p));

        void* out_ptr = nullptr;
        rte_ring_sc_dequeue(ring, &out_ptr);
        benchmark::DoNotOptimize(*static_cast<T*>(out_ptr));
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * 2);
    rte_ring_free(ring);
}

// ============================================================================
// 2. Throughput -- 2-thread SPSC saturated throughput
// ============================================================================

template <size_t PayloadSize, size_t BufSize>
static void BM_BQ_Throughput(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    static BoundedQueue<T, BufSize> q;
    static auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (topology.size() < 2) {
        state.SkipWithError("Need at least 2 cores");
        return;
    }

    if (state.thread_index() == 0) {
        (void)pin_thread(topology[0].hw_thread_id);
        T val;
        for ([[maybe_unused]] auto _ : state) {
            q.push(val);
            benchmark::ClobberMemory();
        }
    } else {
        (void)pin_thread(topology[1].hw_thread_id);
        T out;
        for ([[maybe_unused]] auto _ : state) {
            q.pop(out);
            benchmark::DoNotOptimize(out);
        }
    }
    state.SetItemsProcessed(state.iterations());
}

template <size_t PayloadSize, size_t BufSize>
static void BM_RteRing_Throughput(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    // Use a static ring so both threads see the same instance.
    // Leak intentionally -- cleaned up at process exit.
    static rte_ring* ring = create_spsc_ring(BufSize);
    static T pool[BufSize > 0 ? BufSize : 1];
    static auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (!ring) {
        state.SkipWithError("rte_ring_create failed");
        return;
    }
    if (topology.size() < 2) {
        state.SkipWithError("Need at least 2 cores");
        return;
    }

    if (state.thread_index() == 0) {
        // Producer
        (void)pin_thread(topology[0].hw_thread_id);
        size_t idx = 0;
        for ([[maybe_unused]] auto _ : state) {
            T* p = &pool[idx & (BufSize - 1)];
            ++idx;
            // Spin until enqueue succeeds (queue may be full)
            while (rte_ring_sp_enqueue(ring, static_cast<void*>(p)) != 0) {
                cpu_relax();
            }
            benchmark::ClobberMemory();
        }
    } else {
        // Consumer
        (void)pin_thread(topology[1].hw_thread_id);
        for ([[maybe_unused]] auto _ : state) {
            void* out_ptr = nullptr;
            while (rte_ring_sc_dequeue(ring, &out_ptr) != 0) {
                cpu_relax();
            }
            benchmark::DoNotOptimize(*static_cast<T*>(out_ptr));
        }
    }
    state.SetItemsProcessed(state.iterations());
}

// ============================================================================
// 3. PingPong -- cross-core RTT (2 queues, 2 threads)
// ============================================================================

template <typename T, size_t BufSize>
struct alignas(64) BQPingPongCtx {
    BoundedQueue<T, BufSize> q1;  // T1 -> T2
    char pad[64];                  // prevent false sharing
    BoundedQueue<T, BufSize> q2;  // T2 -> T1
};

template <size_t PayloadSize, size_t BufSize>
static void BM_BQ_PingPong(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    static auto* ctx = new BQPingPongCtx<T, BufSize>();
    static auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (topology.size() < 2) {
        state.SkipWithError("Need at least 2 cores");
        return;
    }

    if (state.thread_index() == 0) {
        (void)pin_thread(topology[0].hw_thread_id);
        T send_val, recv_val;
        for ([[maybe_unused]] auto _ : state) {
            ctx->q1.push(send_val);
            ctx->q2.pop(recv_val);
        }
    } else {
        (void)pin_thread(topology[1].hw_thread_id);
        T val;
        for ([[maybe_unused]] auto _ : state) {
            ctx->q1.pop(val);
            ctx->q2.push(val);
        }
    }
}

template <size_t PayloadSize, size_t BufSize>
static void BM_RteRing_PingPong(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    // Static rings for cross-thread visibility. Leak intentionally.
    static rte_ring* q1 = create_spsc_ring(BufSize);
    static rte_ring* q2 = create_spsc_ring(BufSize);
    static T pool[BufSize > 0 ? BufSize : 1];
    static auto topology = get_cpu_topology().value_or(std::vector<CpuTopologyInfo>{});

    if (!q1 || !q2) {
        state.SkipWithError("rte_ring_create failed");
        return;
    }
    if (topology.size() < 2) {
        state.SkipWithError("Need at least 2 cores");
        return;
    }

    if (state.thread_index() == 0) {
        // Ping: send on q1, receive on q2
        (void)pin_thread(topology[0].hw_thread_id);
        size_t idx = 0;
        for ([[maybe_unused]] auto _ : state) {
            T* p = &pool[idx & (BufSize - 1)];
            ++idx;
            while (rte_ring_sp_enqueue(q1, static_cast<void*>(p)) != 0) {
                cpu_relax();
            }
            void* out_ptr = nullptr;
            while (rte_ring_sc_dequeue(q2, &out_ptr) != 0) {
                cpu_relax();
            }
            benchmark::DoNotOptimize(*static_cast<T*>(out_ptr));
        }
    } else {
        // Pong: receive on q1, send on q2
        (void)pin_thread(topology[1].hw_thread_id);
        for ([[maybe_unused]] auto _ : state) {
            void* in_ptr = nullptr;
            while (rte_ring_sc_dequeue(q1, &in_ptr) != 0) {
                cpu_relax();
            }
            // Echo the same pointer back
            while (rte_ring_sp_enqueue(q2, in_ptr) != 0) {
                cpu_relax();
            }
        }
    }
}

// ============================================================================
// 4. Batch -- bulk enqueue/dequeue amortised cost
// ============================================================================

template <size_t PayloadSize, size_t BufSize>
static void BM_BQ_Batch(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    BoundedQueue<T, BufSize> q;
    T items[BufSize];
    size_t consumed = 0;

    for ([[maybe_unused]] auto _ : state) {
        // Push a full batch
        [[maybe_unused]] bool ok = q.try_push_n(std::span<const T>(items, BufSize));

        // Pop all via try_consume_all
        consumed = 0;
        [[maybe_unused]] auto n_consumed = q.try_consume_all([&consumed](const T& val, size_t) {
            auto copy = val;
            benchmark::DoNotOptimize(copy);
            ++consumed;
        });
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * BufSize * 2);
}

template <size_t PayloadSize, size_t BufSize>
static void BM_RteRing_Batch(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    auto* ring = create_spsc_ring(BufSize);
    if (!ring) {
        state.SkipWithError("rte_ring_create failed");
        return;
    }

    // Pre-allocated pointer array for burst enqueue/dequeue
    T pool[BufSize];
    void* enq_ptrs[BufSize];
    void* deq_ptrs[BufSize];
    for (size_t i = 0; i < BufSize; ++i) {
        enq_ptrs[i] = static_cast<void*>(&pool[i]);
    }

    for ([[maybe_unused]] auto _ : state) {
        // Burst enqueue
        unsigned enqueued = rte_ring_sp_enqueue_burst(
            ring, enq_ptrs, static_cast<unsigned>(BufSize), nullptr);
        benchmark::DoNotOptimize(enqueued);

        // Burst dequeue
        unsigned dequeued = rte_ring_sc_dequeue_burst(
            ring, deq_ptrs, static_cast<unsigned>(BufSize), nullptr);

        // Touch dequeued data to prevent optimisation
        for (unsigned i = 0; i < dequeued; ++i) {
            benchmark::DoNotOptimize(*static_cast<T*>(deq_ptrs[i]));
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * BufSize * 2);
    rte_ring_free(ring);
}

// ============================================================================
// 5. QueueFull -- failure-path latency when queue is full
// ============================================================================

template <size_t PayloadSize, size_t BufSize>
static void BM_BQ_TryPushFull(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    BoundedQueue<T, BufSize> q;
    T val;

    // Fill the queue completely
    for (size_t i = 0; i < BufSize; ++i) {
        q.push(val);
    }

    // Measure the cost of a failed try_push on a full queue
    for ([[maybe_unused]] auto _ : state) {
        bool ok = q.try_push(val);
        benchmark::DoNotOptimize(ok);
    }
}

template <size_t PayloadSize, size_t BufSize>
static void BM_RteRing_TryEnqueueFull(benchmark::State& state) {
    using T = Payload<PayloadSize>;
    auto* ring = create_spsc_ring(BufSize);
    if (!ring) {
        state.SkipWithError("rte_ring_create failed");
        return;
    }

    // Pre-fill the ring
    T pool[BufSize];
    for (size_t i = 0; i < BufSize; ++i) {
        // Keep enqueuing until the ring reports full
        if (rte_ring_sp_enqueue(ring, static_cast<void*>(&pool[i])) != 0) {
            break;
        }
    }
    // Try to squeeze in more until definitely full
    T extra;
    while (rte_ring_sp_enqueue(ring, static_cast<void*>(&extra)) == 0) {}

    // Measure the cost of a failed enqueue on a full ring
    for ([[maybe_unused]] auto _ : state) {
        int ret = rte_ring_sp_enqueue(ring, static_cast<void*>(&extra));
        benchmark::DoNotOptimize(ret);
    }

    rte_ring_free(ring);
}

// ============================================================================
// Matrix registration
// ============================================================================

// --- 1. PushPop (single-thread) ---
REGISTER_MATRIX(BM_BQ_PushPop);
REGISTER_MATRIX(BM_RteRing_PushPop);

// --- 2. Throughput (2 threads, skip BufSize=1) ---
// BufSize=1 is too small for sustained 2-thread SPSC (producer blocks
// immediately), so we register only BufSize >= 8 manually.
#define REGISTER_THREADED(FUNC)                                                \
    BENCHMARK_TEMPLATE(FUNC, 8, 8)                                             \
        ->Name(#FUNC "/P:8/B:8")                                               \
        ->Threads(2)                                                           \
        ->UseRealTime();                                                       \
    BENCHMARK_TEMPLATE(FUNC, 8, 64)                                            \
        ->Name(#FUNC "/P:8/B:64")                                              \
        ->Threads(2)                                                           \
        ->UseRealTime();                                                       \
    BENCHMARK_TEMPLATE(FUNC, 8, 512)                                           \
        ->Name(#FUNC "/P:8/B:512")                                             \
        ->Threads(2)                                                           \
        ->UseRealTime();                                                       \
    BENCHMARK_TEMPLATE(FUNC, 64, 8)                                            \
        ->Name(#FUNC "/P:64/B:8")                                              \
        ->Threads(2)                                                           \
        ->UseRealTime();                                                       \
    BENCHMARK_TEMPLATE(FUNC, 64, 64)                                           \
        ->Name(#FUNC "/P:64/B:64")                                             \
        ->Threads(2)                                                           \
        ->UseRealTime();                                                       \
    BENCHMARK_TEMPLATE(FUNC, 64, 512)                                          \
        ->Name(#FUNC "/P:64/B:512")                                            \
        ->Threads(2)                                                           \
        ->UseRealTime();                                                       \
    BENCHMARK_TEMPLATE(FUNC, 512, 8)                                           \
        ->Name(#FUNC "/P:512/B:8")                                             \
        ->Threads(2)                                                           \
        ->UseRealTime();                                                       \
    BENCHMARK_TEMPLATE(FUNC, 512, 64)                                          \
        ->Name(#FUNC "/P:512/B:64")                                            \
        ->Threads(2)                                                           \
        ->UseRealTime();                                                       \
    BENCHMARK_TEMPLATE(FUNC, 512, 512)                                         \
        ->Name(#FUNC "/P:512/B:512")                                           \
        ->Threads(2)                                                           \
        ->UseRealTime()

REGISTER_THREADED(BM_BQ_Throughput);
REGISTER_THREADED(BM_RteRing_Throughput);

// --- 3. PingPong (2 threads, skip BufSize=1) ---
REGISTER_THREADED(BM_BQ_PingPong);
REGISTER_THREADED(BM_RteRing_PingPong);

// --- 4. Batch (single-thread) ---
REGISTER_MATRIX(BM_BQ_Batch);
REGISTER_MATRIX(BM_RteRing_Batch);

// --- 5. QueueFull (single-thread) ---
REGISTER_MATRIX(BM_BQ_TryPushFull);
REGISTER_MATRIX(BM_RteRing_TryEnqueueFull);

// Opt-in NUMA pin via `EPH_BENCH_NUMA_NODE` (T3.6 from the 2026-05-05
// action list).
int main(int argc, char** argv) {
    if (auto pinned = ::eph::dpdk::bench::apply_env_numa_pin()) {
        std::fprintf(stderr,
            "[bench_rte_ring_vs_bq] pinned to NUMA node %d via "
            "EPH_BENCH_NUMA_NODE\n", *pinned);
    }
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
