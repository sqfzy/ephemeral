/// @file bench_socket_config.cpp
/// Micro-benchmarks for SocketConfig parsing, validation, and serialization.
///
/// SocketConfig::from_url() is called during connection setup (not hot path),
/// but validate() and to_json() may be called frequently for monitoring.

#include <benchmark/benchmark.h>

#include "eph/net/socket_config.hpp"

using namespace eph::net;

// ---------------------------------------------------------------------------
// SocketConfig::from_url() — URL parsing
// ---------------------------------------------------------------------------

/// Parse a simple "host:port" URL (common case).
static void BM_SocketConfig_FromUrl_Simple(benchmark::State& state) {
    for (auto _ : state) {
        auto cfg = SocketConfig::from_url("api.binance.com:9443");
        benchmark::DoNotOptimize(cfg);
    }
}
BENCHMARK(BM_SocketConfig_FromUrl_Simple);

/// Parse a "tcp://host:port" URL with scheme prefix.
static void BM_SocketConfig_FromUrl_TcpScheme(benchmark::State& state) {
    for (auto _ : state) {
        auto cfg = SocketConfig::from_url("tcp://stream.exchange.io:8443");
        benchmark::DoNotOptimize(cfg);
    }
}
BENCHMARK(BM_SocketConfig_FromUrl_TcpScheme);

/// Parse an IPv6 URL with bracket notation.
static void BM_SocketConfig_FromUrl_IPv6(benchmark::State& state) {
    for (auto _ : state) {
        auto cfg = SocketConfig::from_url("tcp://[::1]:9443");
        benchmark::DoNotOptimize(cfg);
    }
}
BENCHMARK(BM_SocketConfig_FromUrl_IPv6);

// ---------------------------------------------------------------------------
// SocketConfig::to_url() — URL serialization
// ---------------------------------------------------------------------------

static void BM_SocketConfig_ToUrl(benchmark::State& state) {
    SocketConfig cfg{.host = "api.binance.com", .port = 9443};

    for (auto _ : state) {
        auto url = cfg.to_url();
        benchmark::DoNotOptimize(url);
    }
}
BENCHMARK(BM_SocketConfig_ToUrl);

// ---------------------------------------------------------------------------
// SocketConfig::validate() — configuration validation
// ---------------------------------------------------------------------------

static void BM_SocketConfig_Validate(benchmark::State& state) {
    SocketConfig cfg{
        .host = "api.binance.com", .port = 9443,
        .tcp_nodelay = true, .tcp_keepalive = true,
        .keepalive_idle = 60, .keepalive_interval = 10,
        .keepalive_count = 3};

    for (auto _ : state) {
        auto err = cfg.validate();
        benchmark::DoNotOptimize(err);
    }
}
BENCHMARK(BM_SocketConfig_Validate);

// ---------------------------------------------------------------------------
// SocketConfig::to_json() — JSON serialization for monitoring
// ---------------------------------------------------------------------------

static void BM_SocketConfig_ToJson(benchmark::State& state) {
    SocketConfig cfg{
        .host = "api.binance.com", .port = 9443,
        .tcp_nodelay = true, .bind_device = "ens35"};

    for (auto _ : state) {
        auto j = cfg.to_json();
        benchmark::DoNotOptimize(j);
    }
}
BENCHMARK(BM_SocketConfig_ToJson);

// ---------------------------------------------------------------------------
// SocketConfig::dump() — human-readable serialization
// ---------------------------------------------------------------------------

static void BM_SocketConfig_Dump(benchmark::State& state) {
    SocketConfig cfg{
        .host = "api.binance.com", .port = 9443,
        .tcp_nodelay = true, .bind_device = "ens35"};

    for (auto _ : state) {
        auto d = cfg.dump();
        benchmark::DoNotOptimize(d);
    }
}
BENCHMARK(BM_SocketConfig_Dump);

BENCHMARK_MAIN();
