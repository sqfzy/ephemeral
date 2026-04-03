/// @file bench_transport_types.cpp
/// Benchmarks for transport hot-path utilities:
///   - apply_twophase_filter (frame dedup on RX path)
///   - tls_record::build_nonce (per-record crypto nonce)
///   - TransportConfig::from_url (config parsing)
///   - TransportConfig::validate (config validation)
///
/// These functions are called on the critical path:
///   build_nonce: every TLS record (~100K/sec at HFT rates)
///   apply_twophase_filter: every RX batch (when frame filter is active)
///   from_url/validate: once at startup (included for baseline tracking)

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>

#include <benchmark/benchmark.h>

#include "eph/transport/transport_types.hpp"
#include "eph/transport/detail/tls_constants.hpp"

using namespace eph::net;

// ---------------------------------------------------------------------------
// tls_record::build_nonce — per-record nonce construction (hot path)
// ---------------------------------------------------------------------------

static void BM_BuildNonce(benchmark::State& state) {
    uint8_t iv[tls_const::kTls13NonceLen] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88};
    uint8_t out[tls_const::kTls13NonceLen] = {};
    uint64_t seq = 0;
    for (auto _ : state) {
        tls_record::build_nonce(out, iv, seq++);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BuildNonce);

// ---------------------------------------------------------------------------
// apply_twophase_filter — frame dedup (RX hot path when filter is active)
// ---------------------------------------------------------------------------

// Helper: generate N frames with a specified number of unique symbols.
// Simulates real-world scenarios where multiple updates per symbol arrive
// in a single batch.
static std::vector<FrameView> make_frames(size_t n, size_t unique_symbols,
                                           std::vector<uint8_t>& backing) {
    backing.resize(n);  // 1 byte per frame payload (sufficient for hashing)
    std::vector<FrameView> frames(n);
    for (size_t i = 0; i < n; ++i) {
        backing[i] = static_cast<uint8_t>(i % unique_symbols);
        frames[i] = {.payload = &backing[i], .payload_len = 1, .deliver = true};
    }
    return frames;
}

static void BM_TwophaseFilter_16Frames_4Symbols(benchmark::State& state) {
    std::vector<uint8_t> backing;
    auto frames = make_frames(16, 4, backing);
    auto extractor = [](const uint8_t* data, size_t) -> uint32_t {
        return static_cast<uint32_t>(*data) + 1;  // +1 to avoid hash=0 (unrecognized)
    };
    for (auto _ : state) {
        // Reset deliver flags
        for (auto& f : frames) f.deliver = true;
        filter_detail::apply_twophase_filter(
            std::span<FrameView>(frames), extractor);
        benchmark::DoNotOptimize(frames.data());
    }
    state.SetItemsProcessed(state.iterations() * 16);
}
BENCHMARK(BM_TwophaseFilter_16Frames_4Symbols);

static void BM_TwophaseFilter_64Frames_16Symbols(benchmark::State& state) {
    std::vector<uint8_t> backing;
    auto frames = make_frames(64, 16, backing);
    auto extractor = [](const uint8_t* data, size_t) -> uint32_t {
        return static_cast<uint32_t>(*data) + 1;
    };
    for (auto _ : state) {
        for (auto& f : frames) f.deliver = true;
        filter_detail::apply_twophase_filter(
            std::span<FrameView>(frames), extractor);
        benchmark::DoNotOptimize(frames.data());
    }
    state.SetItemsProcessed(state.iterations() * 64);
}
BENCHMARK(BM_TwophaseFilter_64Frames_16Symbols);

static void BM_TwophaseFilter_128Frames_32Symbols(benchmark::State& state) {
    std::vector<uint8_t> backing;
    auto frames = make_frames(128, 32, backing);
    auto extractor = [](const uint8_t* data, size_t) -> uint32_t {
        return static_cast<uint32_t>(*data) + 1;
    };
    for (auto _ : state) {
        for (auto& f : frames) f.deliver = true;
        filter_detail::apply_twophase_filter(
            std::span<FrameView>(frames), extractor);
        benchmark::DoNotOptimize(frames.data());
    }
    state.SetItemsProcessed(state.iterations() * 128);
}
BENCHMARK(BM_TwophaseFilter_128Frames_32Symbols);

// All unique hashes (no dedup) — measures overhead of hash table without collisions
static void BM_TwophaseFilter_64Frames_AllUnique(benchmark::State& state) {
    std::vector<uint8_t> backing;
    auto frames = make_frames(64, 64, backing);
    auto extractor = [](const uint8_t* data, size_t) -> uint32_t {
        return static_cast<uint32_t>(*data) + 1;
    };
    for (auto _ : state) {
        for (auto& f : frames) f.deliver = true;
        filter_detail::apply_twophase_filter(
            std::span<FrameView>(frames), extractor);
        benchmark::DoNotOptimize(frames.data());
    }
    state.SetItemsProcessed(state.iterations() * 64);
}
BENCHMARK(BM_TwophaseFilter_64Frames_AllUnique);

// All same hash (maximum dedup) — worst case for hash table (single slot)
static void BM_TwophaseFilter_64Frames_SingleSymbol(benchmark::State& state) {
    std::vector<uint8_t> backing;
    auto frames = make_frames(64, 1, backing);
    auto extractor = [](const uint8_t*, size_t) -> uint32_t {
        return 42;  // all same hash
    };
    for (auto _ : state) {
        for (auto& f : frames) f.deliver = true;
        filter_detail::apply_twophase_filter(
            std::span<FrameView>(frames), extractor);
        benchmark::DoNotOptimize(frames.data());
    }
    state.SetItemsProcessed(state.iterations() * 64);
}
BENCHMARK(BM_TwophaseFilter_64Frames_SingleSymbol);

// ---------------------------------------------------------------------------
// TransportConfig::from_url — URL parsing (startup path, baseline tracking)
// ---------------------------------------------------------------------------

static void BM_FromUrl_Simple(benchmark::State& state) {
    std::string url = "wss://stream.example.com/ws/v2";
    for (auto _ : state) {
        auto result = TransportConfig::from_url(url);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FromUrl_Simple);

static void BM_FromUrl_WithPort(benchmark::State& state) {
    std::string url = "wss://api.example.com:8443/v1/stream?token=abc123";
    for (auto _ : state) {
        auto result = TransportConfig::from_url(url);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FromUrl_WithPort);

static void BM_FromUrl_Ipv6(benchmark::State& state) {
    std::string url = "wss://[2001:db8::1]:9443/stream";
    for (auto _ : state) {
        auto result = TransportConfig::from_url(url);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_FromUrl_Ipv6);

// ---------------------------------------------------------------------------
// TransportConfig::validate — config validation (startup path)
// ---------------------------------------------------------------------------

static void BM_Validate_Valid(benchmark::State& state) {
    TransportConfig cfg;
    cfg.remote_host = "stream.example.com";
    cfg.remote_port = 443;
    cfg.ws_path = "/ws/v2";
    for (auto _ : state) {
        auto result = cfg.validate();
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Validate_Valid);

static void BM_Validate_WithAllOptionalFields(benchmark::State& state) {
    TransportConfig cfg;
    cfg.remote_host = "stream.example.com";
    cfg.remote_port = 8443;
    cfg.ws_path = "/ws/v2";
    cfg.ws_subprotocol = "graphql-ws";
    cfg.extra_headers = "Authorization: Bearer tok123\r\n";
    cfg.client_cert_path = "/etc/ssl/client.pem";
    cfg.client_key_path = "/etc/ssl/client.key";
    cfg.max_reconnect_attempts = 10;
    cfg.reconnect_interval = std::chrono::milliseconds{100};
    cfg.ping_interval = std::chrono::seconds{30};
    cfg.pong_timeout = std::chrono::seconds{10};
    for (auto _ : state) {
        auto result = cfg.validate();
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Validate_WithAllOptionalFields);

// ---------------------------------------------------------------------------
// TransportConfig::to_json — serialization (monitoring path)
// ---------------------------------------------------------------------------

static void BM_ConfigToJson(benchmark::State& state) {
    TransportConfig cfg;
    cfg.remote_host = "stream.example.com";
    cfg.remote_port = 8443;
    cfg.ws_path = "/ws/v2";
    for (auto _ : state) {
        auto j = cfg.to_json();
        benchmark::DoNotOptimize(j);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConfigToJson);

// ---------------------------------------------------------------------------
// TransportStats::to_json — stats serialization (monitoring hot path)
// ---------------------------------------------------------------------------

static void BM_StatsToJson(benchmark::State& state) {
    TransportStats s{};
    s.tx_packets = 1000000;
    s.tx_bytes = 500000000;
    s.rx_packets = 2000000;
    s.rx_bytes = 1000000000;
    s.uptime_ns = 60'000'000'000;
    s.remote_ip = "10.0.1.42";
    for (auto _ : state) {
        auto j = s.to_json();
        benchmark::DoNotOptimize(j);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StatsToJson);

BENCHMARK_MAIN();
