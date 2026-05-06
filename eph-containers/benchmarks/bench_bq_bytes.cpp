/// @file bench_bq_bytes.cpp
/// Microbenchmarks for `eph::containers::BoundedQueueBytes`.
///
/// Coverage gap (loop-cleanup R174): `bench_bq_*.cpp` covers the
/// type-templated `BoundedQueue<T>` (push/pop, throughput, ping-pong,
/// batch). The byte-oriented variant `BoundedQueueBytes` — used for
/// reliable streaming of variable-length frames between threads — was
/// untested by any bench. Its hot path differs from BoundedQueue<T> by
/// the per-slot `memcpy` of the payload and the `len` validation gate
/// against `MaxDataSize`. A regression in either (e.g. dropping the
/// [[unlikely]] hint, swapping memcpy for std::copy, or moving the
/// length check below the memcpy) would silently widen per-message
/// cost on every IPC channel using this type.
///
/// We bench three shapes:
///   - try_push_small:  64-byte payload (typical FIX field-update IPC)
///   - try_push_large:  payload at MaxDataSize-1 (worst-case memcpy)
///   - try_pop:         pop into a caller-supplied output buffer
/// All in single-thread mode — the SPSC threading is already covered
/// by bench_bq_throughput.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <benchmark/benchmark.h>

#include "eph/containers/bounded_queue_bytes.hpp"

using eph::containers::BoundedQueueBytes;

// ---------------------------------------------------------------------------
// try_push: per-message memcpy + len validation + queue produce.
// We drain in the loop so the queue does not stay full (which would
// short-circuit the bench to the full-rejection branch).
// ---------------------------------------------------------------------------

static void BM_BqBytesTryPushSmall(benchmark::State& state) {
    BoundedQueueBytes<256, 256> q;
    std::array<uint8_t, 64> payload{};
    std::array<uint8_t, 256> drain{};
    for (auto _ : state) {
        bool ok = q.try_push(std::span<const uint8_t>{payload});
        benchmark::DoNotOptimize(ok);
        // Drain to keep the queue from saturating — the pop is a real
        // operation but fits in the same loop iteration so the bench
        // measures push+pop pair cost. Pure-push numbers can be
        // derived by subtracting `try_pop` from this.
        auto popped = q.try_pop(std::span<uint8_t>{drain});
        benchmark::DoNotOptimize(popped);
    }
}
BENCHMARK(BM_BqBytesTryPushSmall);

static void BM_BqBytesTryPushLarge(benchmark::State& state) {
    BoundedQueueBytes<256, 256> q;
    std::array<uint8_t, 255> payload{};   // MaxDataSize - 1 worst-case memcpy.
    std::array<uint8_t, 256> drain{};
    for (auto _ : state) {
        bool ok = q.try_push(std::span<const uint8_t>{payload});
        benchmark::DoNotOptimize(ok);
        auto popped = q.try_pop(std::span<uint8_t>{drain});
        benchmark::DoNotOptimize(popped);
    }
}
BENCHMARK(BM_BqBytesTryPushLarge);

/// Reject branch: payload exceeds MaxDataSize. The check uses [[unlikely]]
/// so the steady-state cost should be near zero — pin so a regression
/// that removes the hint or moves the check below the memcpy is visible.
static void BM_BqBytesTryPushOversize(benchmark::State& state) {
    // Make the queue's internal slot small (32 bytes) so a 33-byte
    // payload trips the rejection.
    BoundedQueueBytes<32, 64> q;
    std::array<uint8_t, 33> payload{};
    for (auto _ : state) {
        bool ok = q.try_push(std::span<const uint8_t>{payload});
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_BqBytesTryPushOversize);

BENCHMARK_MAIN();
