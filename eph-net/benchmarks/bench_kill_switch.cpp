/// @file bench_kill_switch.cpp
/// Micro-benchmarks for KillSwitch registration, shutdown, and query paths.
///
/// KillSwitch is used in signal-handler-adjacent code. While shutdown itself
/// is not latency-sensitive, registration and the is_shutdown_requested() query
/// are called from application hot paths and should be fast.

#include <benchmark/benchmark.h>

#include "eph/net/kill_switch.hpp"

using namespace eph::net;

namespace {

/// Minimal mock transport satisfying Stoppable.
struct BenchTransport {
    bool running = true;
    void stop() noexcept { running = false; }
    [[nodiscard]] bool is_running() const noexcept { return running; }
};

} // namespace

// ---------------------------------------------------------------------------
// is_shutdown_requested() — hot-path query (lock-free atomic load)
// ---------------------------------------------------------------------------

static void BM_KillSwitch_IsShutdownRequested(benchmark::State& state) {
    KillSwitch ks;

    for (auto _ : state) {
        bool r = ks.is_shutdown_requested();
        benchmark::DoNotOptimize(r);
    }
}
BENCHMARK(BM_KillSwitch_IsShutdownRequested);

// ---------------------------------------------------------------------------
// register_transport() — spinlock-protected add
// ---------------------------------------------------------------------------

static void BM_KillSwitch_Register(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        KillSwitch ks;
        BenchTransport tp;
        state.ResumeTiming();

        bool ok = ks.register_transport(&tp);
        benchmark::DoNotOptimize(ok);
    }
}
BENCHMARK(BM_KillSwitch_Register);

// ---------------------------------------------------------------------------
// unregister_transport() — spinlock-protected remove
// ---------------------------------------------------------------------------

static void BM_KillSwitch_Unregister(benchmark::State& state) {
    KillSwitch ks;
    BenchTransport tp;
    ks.register_transport(&tp);

    for (auto _ : state) {
        state.PauseTiming();
        // Re-register after each unregister for the next iteration
        ks.register_transport(&tp);
        state.ResumeTiming();

        ks.unregister_transport(&tp);
    }
}
BENCHMARK(BM_KillSwitch_Unregister);

// ---------------------------------------------------------------------------
// transport_count() — atomic load
// ---------------------------------------------------------------------------

static void BM_KillSwitch_TransportCount(benchmark::State& state) {
    KillSwitch ks;
    BenchTransport tp1, tp2, tp3;
    ks.register_transport(&tp1);
    ks.register_transport(&tp2);
    ks.register_transport(&tp3);

    for (auto _ : state) {
        auto n = ks.transport_count();
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_KillSwitch_TransportCount);

// ---------------------------------------------------------------------------
// running_count() — spinlock-protected scan
// ---------------------------------------------------------------------------

static void BM_KillSwitch_RunningCount(benchmark::State& state) {
    KillSwitch ks;
    BenchTransport tp1, tp2, tp3;
    ks.register_transport(&tp1);
    ks.register_transport(&tp2);
    ks.register_transport(&tp3);

    for (auto _ : state) {
        auto n = ks.running_count();
        benchmark::DoNotOptimize(n);
    }
}
BENCHMARK(BM_KillSwitch_RunningCount);

// ---------------------------------------------------------------------------
// request_shutdown() — signal-safe atomic store
// ---------------------------------------------------------------------------

static void BM_KillSwitch_RequestShutdown(benchmark::State& state) {
    KillSwitch ks;

    for (auto _ : state) {
        ks.request_shutdown();
    }
}
BENCHMARK(BM_KillSwitch_RequestShutdown);

// ---------------------------------------------------------------------------
// to_json() — monitoring snapshot
// ---------------------------------------------------------------------------

static void BM_KillSwitch_ToJson(benchmark::State& state) {
    KillSwitch ks;
    BenchTransport tp1, tp2;
    ks.register_transport(&tp1);
    ks.register_transport(&tp2);

    for (auto _ : state) {
        auto j = ks.to_json();
        benchmark::DoNotOptimize(j);
    }
}
BENCHMARK(BM_KillSwitch_ToJson);

BENCHMARK_MAIN();
