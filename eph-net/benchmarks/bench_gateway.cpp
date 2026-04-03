/// @file bench_gateway.cpp
/// Micro-benchmarks for Gateway management operations.
///
/// Gateway sits on the control plane, not the hot data path. These benchmarks
/// establish baselines to detect regressions in add/remove/health-check paths,
/// which are called during connection lifecycle events.

#include <benchmark/benchmark.h>

#include "eph/net/gateway.hpp"

using namespace eph::net;

namespace {

/// Minimal mock transport satisfying GatewayManageable.
struct BenchTransport {
    bool running = true;
    void start_threads() noexcept { running = true; }
    void stop() noexcept { running = false; }
    [[nodiscard]] bool is_running() const noexcept { return running; }
    void reconnect_now() noexcept {}
};

} // namespace

// ---------------------------------------------------------------------------
// Gateway::add() — single connection registration
// ---------------------------------------------------------------------------

static void BM_Gateway_Add(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        Gateway gw;
        BenchTransport tp;
        state.ResumeTiming();

        auto id = gw.add("bench-conn", &tp);
        benchmark::DoNotOptimize(id);
    }
}
BENCHMARK(BM_Gateway_Add);

// ---------------------------------------------------------------------------
// Gateway::find_by_tag() — linear scan lookup
// ---------------------------------------------------------------------------

static void BM_Gateway_FindByTag(benchmark::State& state) {
    const auto n = static_cast<size_t>(state.range(0));
    Gateway gw;
    std::vector<BenchTransport> transports(n);

    for (size_t i = 0; i < n; ++i) {
        (void)gw.add("conn-" + std::to_string(i), &transports[i]);
    }

    // Search for the last tag (worst case for linear scan)
    std::string target = "conn-" + std::to_string(n - 1);

    for (auto _ : state) {
        auto id = gw.find_by_tag(target);
        benchmark::DoNotOptimize(id);
    }
}
BENCHMARK(BM_Gateway_FindByTag)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

// ---------------------------------------------------------------------------
// Gateway::health() — single connection health query
// ---------------------------------------------------------------------------

static void BM_Gateway_Health(benchmark::State& state) {
    Gateway gw;
    BenchTransport tp;
    tp.running = true;
    auto id = gw.add("bench-conn", &tp);

    for (auto _ : state) {
        auto h = gw.health(id);
        benchmark::DoNotOptimize(h);
    }
}
BENCHMARK(BM_Gateway_Health);

// ---------------------------------------------------------------------------
// Gateway::healthy_count() — scan all connections
// ---------------------------------------------------------------------------

static void BM_Gateway_HealthyCount(benchmark::State& state) {
    const auto n = static_cast<size_t>(state.range(0));
    Gateway gw;
    std::vector<BenchTransport> transports(n);

    for (size_t i = 0; i < n; ++i) {
        transports[i].running = (i % 2 == 0); // half healthy
        (void)gw.add("conn-" + std::to_string(i), &transports[i]);
    }

    for (auto _ : state) {
        auto count = gw.healthy_count();
        benchmark::DoNotOptimize(count);
    }
}
BENCHMARK(BM_Gateway_HealthyCount)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

// ---------------------------------------------------------------------------
// Gateway::check_health() — full health check cycle
// ---------------------------------------------------------------------------

static void BM_Gateway_CheckHealth(benchmark::State& state) {
    const auto n = static_cast<size_t>(state.range(0));
    Gateway gw;
    std::vector<BenchTransport> transports(n);

    for (size_t i = 0; i < n; ++i) {
        transports[i].running = true;
        (void)gw.add("conn-" + std::to_string(i), &transports[i]);
    }

    for (auto _ : state) {
        gw.check_health();
    }
}
BENCHMARK(BM_Gateway_CheckHealth)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

// ---------------------------------------------------------------------------
// Gateway::for_each() — iteration overhead
// ---------------------------------------------------------------------------

static void BM_Gateway_ForEach(benchmark::State& state) {
    const auto n = static_cast<size_t>(state.range(0));
    Gateway gw;
    std::vector<BenchTransport> transports(n);

    for (size_t i = 0; i < n; ++i) {
        (void)gw.add("conn-" + std::to_string(i), &transports[i]);
    }

    for (auto _ : state) {
        size_t count = 0;
        gw.for_each([&](size_t, std::string_view, ConnHealth) { ++count; });
        benchmark::DoNotOptimize(count);
    }
}
BENCHMARK(BM_Gateway_ForEach)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

// ---------------------------------------------------------------------------
// Gateway::dump() — serialization overhead
// ---------------------------------------------------------------------------

static void BM_Gateway_Dump(benchmark::State& state) {
    const auto n = static_cast<size_t>(state.range(0));
    Gateway gw;
    std::vector<BenchTransport> transports(n);

    for (size_t i = 0; i < n; ++i) {
        transports[i].running = (i % 2 == 0);
        (void)gw.add("conn-" + std::to_string(i), &transports[i]);
    }

    for (auto _ : state) {
        auto s = gw.dump();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Gateway_Dump)->Arg(1)->Arg(4)->Arg(8);

// ---------------------------------------------------------------------------
// Gateway::to_json() — JSON serialization overhead
// ---------------------------------------------------------------------------

static void BM_Gateway_ToJson(benchmark::State& state) {
    const auto n = static_cast<size_t>(state.range(0));
    Gateway gw;
    std::vector<BenchTransport> transports(n);

    for (size_t i = 0; i < n; ++i) {
        transports[i].running = (i % 2 == 0);
        (void)gw.add("conn-" + std::to_string(i), &transports[i]);
    }

    for (auto _ : state) {
        auto s = gw.to_json();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Gateway_ToJson)->Arg(1)->Arg(4)->Arg(8);

BENCHMARK_MAIN();
