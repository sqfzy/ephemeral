/// @file bench_e2e_latency.cpp
/// E2E latency benchmark: measures Transport framework overhead using
/// FakeTcpTransport as the backend.
///
/// Benchmarks:
///   1. Send enqueue throughput (measures SPSC queue + TX thread processing)
///   2. Transport creation overhead (connect + thread start)
///   3. Stats snapshot overhead (atomic reads aggregation)
///
/// These establish the floor overhead of the Transport framework itself,
/// independent of any real network backend.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "eph/core/fake_tcp_transport.hpp"
#include "eph/transport/raw_framer.hpp"
#include "eph/transport/transport.hpp"
#include "eph/transport/transport_types.hpp"

using namespace eph::net;
using eph::net::testing::FakeTcpTransport;

/// Transport type: FakeTcp, RawFramer, 512B max payload, 256-depth queue.
using BenchTransport = Transport<FakeTcpTransport, RawFramer, 512, 256>;

namespace {

TransportConfig make_bench_config() {
    TransportConfig cfg;
    cfg.remote_host = "bench.local";
    cfg.remote_port = 9999;
    cfg.use_tls     = false;
    cfg.verify_peer = false;
    cfg.max_reconnect_attempts = 0;
    cfg.ping_interval = std::chrono::seconds{0};
    return cfg;
}

BenchTransport::TcpFactory make_factory() {
    return []() -> std::expected<std::unique_ptr<FakeTcpTransport>, std::string> {
        auto tcp = std::make_unique<FakeTcpTransport>();
        auto result = tcp->connect(std::chrono::milliseconds{100});
        if (!result) return std::unexpected(result.error());
        return tcp;
    };
}

} // namespace

// ---------------------------------------------------------------------------
// BM_E2E_SendEnqueue — measures send() enqueue latency (SPSC push)
//
// The actual TCP send happens asynchronously on the TX thread. This measures
// how fast the application thread can submit messages to the TX queue.
// ---------------------------------------------------------------------------

static void BM_E2E_SendEnqueue(benchmark::State& state) {
    auto cfg = make_bench_config();
    auto transport = BenchTransport::create(make_factory(), cfg);
    if (!transport) {
        state.SkipWithError("Transport::create failed");
        return;
    }

    const size_t payload_len = static_cast<size_t>(state.range(0));
    std::vector<uint8_t> payload(payload_len, 0xAB);

    // Warm up TX thread
    std::this_thread::sleep_for(std::chrono::milliseconds{5});

    for (auto _ : state) {
        auto result = (*transport)->send(payload.data(), payload_len);
        benchmark::DoNotOptimize(result);
    }

    (*transport)->stop();
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(payload_len));
}
BENCHMARK(BM_E2E_SendEnqueue)
    ->Arg(32)    // small message
    ->Arg(128)   // medium message
    ->Arg(512)   // max payload
    ->Unit(benchmark::kNanosecond);

// ---------------------------------------------------------------------------
// BM_E2E_TransportCreate — measures connect + thread startup overhead
// ---------------------------------------------------------------------------

static void BM_E2E_TransportCreate(benchmark::State& state) {
    auto cfg = make_bench_config();

    for (auto _ : state) {
        auto transport = BenchTransport::create(make_factory(), cfg);
        benchmark::DoNotOptimize(transport);
        if (transport) {
            (*transport)->stop();
        }
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_E2E_TransportCreate)->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------
// BM_E2E_StatsSnapshot — measures stats() aggregation cost
//
// The stats() call reads ~30 atomic counters with relaxed ordering and
// computes derived metrics. This benchmark ensures the monitoring path
// doesn't regress.
// ---------------------------------------------------------------------------

static void BM_E2E_StatsSnapshot(benchmark::State& state) {
    auto cfg = make_bench_config();
    auto transport = BenchTransport::create(make_factory(), cfg);
    if (!transport) {
        state.SkipWithError("Transport::create failed");
        return;
    }

    // Send some data to populate stats
    const uint8_t payload[] = "stats-bench";
    for (int i = 0; i < 100; ++i) {
        (*transport)->send(payload, sizeof(payload) - 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    for (auto _ : state) {
        auto s = (*transport)->stats();
        benchmark::DoNotOptimize(s);
    }

    (*transport)->stop();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_E2E_StatsSnapshot)->Unit(benchmark::kNanosecond);

// ---------------------------------------------------------------------------
// BM_E2E_SendBurst — measures burst send pattern (queue N, let TX drain)
//
// Simulates real-world usage: burst of messages followed by a brief pause.
// Reports items/sec as effective throughput.
// ---------------------------------------------------------------------------

static void BM_E2E_SendBurst(benchmark::State& state) {
    auto cfg = make_bench_config();
    auto transport = BenchTransport::create(make_factory(), cfg);
    if (!transport) {
        state.SkipWithError("Transport::create failed");
        return;
    }

    const int burst_size = static_cast<int>(state.range(0));
    const uint8_t payload[64] = {};

    std::this_thread::sleep_for(std::chrono::milliseconds{5});

    for (auto _ : state) {
        for (int i = 0; i < burst_size; ++i) {
            auto result = (*transport)->send(payload, sizeof(payload));
            benchmark::DoNotOptimize(result);
        }
        // Brief yield to let TX drain
        std::this_thread::yield();
    }

    (*transport)->stop();
    state.SetItemsProcessed(state.iterations() * burst_size);
    state.SetBytesProcessed(
        state.iterations() * burst_size * static_cast<int64_t>(sizeof(payload)));
}
BENCHMARK(BM_E2E_SendBurst)
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
