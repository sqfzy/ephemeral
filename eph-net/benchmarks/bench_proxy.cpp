/// @file bench_proxy.cpp
/// Micro-benchmarks for proxy URL parsing and config operations.
///
/// Proxy configuration is set up once per connection (not hot path),
/// but benchmarks establish baselines to catch regressions in
/// parse_proxy_url, validate, to_url, to_json, and dump.

#include <benchmark/benchmark.h>

#include "eph/net/proxy.hpp"

using namespace eph::net::proxy;

// ---------------------------------------------------------------------------
// parse_proxy_url — URL parsing
// ---------------------------------------------------------------------------

/// Parse a SOCKS5 proxy URL without auth (common case).
static void BM_ParseProxyUrl_Socks5NoAuth(benchmark::State& state) {
    for (auto _ : state) {
        auto cfg = parse_proxy_url("socks5://proxy.example.com:1080");
        benchmark::DoNotOptimize(cfg);
    }
}
BENCHMARK(BM_ParseProxyUrl_Socks5NoAuth);

/// Parse a SOCKS5 proxy URL with username/password auth.
static void BM_ParseProxyUrl_Socks5WithAuth(benchmark::State& state) {
    for (auto _ : state) {
        auto cfg = parse_proxy_url("socks5://alice:s3cret@proxy.example.com:9050");
        benchmark::DoNotOptimize(cfg);
    }
}
BENCHMARK(BM_ParseProxyUrl_Socks5WithAuth);

/// Parse an HTTP CONNECT proxy URL.
static void BM_ParseProxyUrl_HttpConnect(benchmark::State& state) {
    for (auto _ : state) {
        auto cfg = parse_proxy_url("http://squid.internal:3128");
        benchmark::DoNotOptimize(cfg);
    }
}
BENCHMARK(BM_ParseProxyUrl_HttpConnect);

// ---------------------------------------------------------------------------
// ProxyConfig::validate() — configuration validation
// ---------------------------------------------------------------------------

static void BM_ProxyConfig_Validate(benchmark::State& state) {
    ProxyConfig cfg{
        .host = "proxy.example.com", .port = 1080,
        .type = ProxyType::kSocks5,
        .username = "alice", .password = "s3cret"};

    for (auto _ : state) {
        auto err = cfg.validate();
        benchmark::DoNotOptimize(err);
    }
}
BENCHMARK(BM_ProxyConfig_Validate);

// ---------------------------------------------------------------------------
// ProxyConfig::to_url() — URL serialization
// ---------------------------------------------------------------------------

static void BM_ProxyConfig_ToUrl(benchmark::State& state) {
    ProxyConfig cfg{
        .host = "proxy.example.com", .port = 9050,
        .type = ProxyType::kSocks5,
        .username = "alice", .password = "s3cret"};

    for (auto _ : state) {
        auto url = cfg.to_url();
        benchmark::DoNotOptimize(url);
    }
}
BENCHMARK(BM_ProxyConfig_ToUrl);

// ---------------------------------------------------------------------------
// ProxyConfig::to_json() — JSON serialization
// ---------------------------------------------------------------------------

static void BM_ProxyConfig_ToJson(benchmark::State& state) {
    ProxyConfig cfg{
        .host = "proxy.example.com", .port = 9050,
        .type = ProxyType::kSocks5,
        .username = "alice", .password = "s3cret"};

    for (auto _ : state) {
        auto j = cfg.to_json();
        benchmark::DoNotOptimize(j);
    }
}
BENCHMARK(BM_ProxyConfig_ToJson);

// ---------------------------------------------------------------------------
// ProxyConfig::dump() — human-readable serialization
// ---------------------------------------------------------------------------

static void BM_ProxyConfig_Dump(benchmark::State& state) {
    ProxyConfig cfg{
        .host = "proxy.example.com", .port = 9050,
        .type = ProxyType::kSocks5,
        .username = "alice", .password = "s3cret"};

    for (auto _ : state) {
        auto d = cfg.dump();
        benchmark::DoNotOptimize(d);
    }
}
BENCHMARK(BM_ProxyConfig_Dump);

BENCHMARK_MAIN();
