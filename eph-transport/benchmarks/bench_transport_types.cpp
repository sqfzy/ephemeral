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
#include "eph/transport/detail/websocket.hpp"

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

// ---------------------------------------------------------------------------
// TransportStats::dump — human-readable stats output (monitoring/logging path)
// ---------------------------------------------------------------------------

static void BM_StatsDump(benchmark::State& state) {
    TransportStats s{};
    s.tx_packets = 1000000;
    s.tx_bytes = 500000000;
    s.rx_packets = 2000000;
    s.rx_bytes = 1000000000;
    s.uptime_ns = 60'000'000'000;
    s.remote_ip = "10.0.1.42";
    s.rtt = {.count = 100, .min_ns = 500, .max_ns = 50000,
             .mean_ns = 5000.0, .p50_ns = 4000, .p99_ns = 40000, .p999_ns = 48000};
    for (auto _ : state) {
        auto d = s.dump();
        benchmark::DoNotOptimize(d);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StatsDump);

// ---------------------------------------------------------------------------
// TransportConfig::to_url — URL serialization (startup/logging path)
// ---------------------------------------------------------------------------

static void BM_ConfigToUrl(benchmark::State& state) {
    TransportConfig cfg;
    cfg.remote_host = "stream.example.com";
    cfg.remote_port = 8443;
    cfg.ws_path = "/ws/v2";
    cfg.use_tls = true;
    for (auto _ : state) {
        auto url = cfg.to_url();
        benchmark::DoNotOptimize(url);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConfigToUrl);

// ---------------------------------------------------------------------------
// TransportConfig::dump — config dump (startup/logging path)
// ---------------------------------------------------------------------------

static void BM_ConfigDump(benchmark::State& state) {
    TransportConfig cfg;
    cfg.remote_host = "stream.example.com";
    cfg.remote_port = 8443;
    cfg.ws_path = "/ws/v2";
    cfg.ws_subprotocol = "graphql-ws";
    for (auto _ : state) {
        auto d = cfg.dump();
        benchmark::DoNotOptimize(d);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConfigDump);

// ---------------------------------------------------------------------------
// RttStats::to_json — RTT stats serialization (monitoring path)
// ---------------------------------------------------------------------------

static void BM_RttStatsToJson(benchmark::State& state) {
    RttStats r{.count = 1000, .min_ns = 500, .max_ns = 100000,
               .mean_ns = 10000.0, .p50_ns = 8000, .p99_ns = 80000, .p999_ns = 95000};
    for (auto _ : state) {
        auto j = r.to_json();
        benchmark::DoNotOptimize(j);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RttStatsToJson);

// ---------------------------------------------------------------------------
// TransportConfig::warnings — advisory diagnostics (startup path)
// ---------------------------------------------------------------------------

static void BM_Warnings_Clean(benchmark::State& state) {
    TransportConfig cfg;
    cfg.remote_host = "stream.example.com";
    cfg.remote_port = 443;
    cfg.ws_path = "/ws";
    cfg.skip_utf8_validation = false;  // avoid default warning
    for (auto _ : state) {
        auto w = cfg.warnings();
        benchmark::DoNotOptimize(w);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Warnings_Clean);

static void BM_Warnings_WithIssues(benchmark::State& state) {
    TransportConfig cfg;
    cfg.remote_host = "stream.example.com";
    cfg.remote_port = 443;
    cfg.ws_path = "/ws";
    cfg.use_tls = false;
    cfg.verify_peer = true;  // contradicts use_tls=false
    cfg.ca_cert_path = "/some/ca.pem";  // unused without TLS
    cfg.tx_burst_size = 2000;  // unusually large
    cfg.skip_utf8_validation = true;  // generates a warning
    for (auto _ : state) {
        auto w = cfg.warnings();
        benchmark::DoNotOptimize(w);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Warnings_WithIssues);

// ---------------------------------------------------------------------------
// ConnectionInfo::to_json — connection metadata serialization
// ---------------------------------------------------------------------------

static void BM_ConnectionInfoToJson(benchmark::State& state) {
    ConnectionInfo ci{
        .tls_version = "TLSv1.3",
        .cipher_name = "TLS_AES_256_GCM_SHA384",
        .ws_subprotocol = "graphql-ws",
        .remote_ip = "10.0.1.42",
        .remote_port = 443,
        .use_tls = true
    };
    for (auto _ : state) {
        auto j = ci.to_json();
        benchmark::DoNotOptimize(j);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConnectionInfoToJson);

// ---------------------------------------------------------------------------
// TransportStats::operator- — windowed metrics delta (monitoring path)
// ---------------------------------------------------------------------------

static void BM_StatsDelta(benchmark::State& state) {
    TransportStats a{};
    a.tx_packets = 2'000'000;
    a.tx_bytes = 1'000'000'000;
    a.rx_packets = 4'000'000;
    a.rx_bytes = 2'000'000'000;
    a.tcp_rx_packets = 5'000'000;
    a.tcp_rx_bursts = 500'000;
    a.uptime_ns = 120'000'000'000;
    a.remote_ip = "10.0.1.42";
    a.rtt = {.count = 200, .min_ns = 500, .max_ns = 50000,
             .mean_ns = 5000.0, .p50_ns = 4000, .p99_ns = 40000, .p999_ns = 48000};
    a.tx_latency = a.rtt;
    a.rx_latency = a.rtt;

    TransportStats b{};
    b.tx_packets = 1'000'000;
    b.tx_bytes = 500'000'000;
    b.rx_packets = 2'000'000;
    b.rx_bytes = 1'000'000'000;
    b.tcp_rx_packets = 2'500'000;
    b.tcp_rx_bursts = 250'000;
    b.uptime_ns = 60'000'000'000;

    for (auto _ : state) {
        auto d = a - b;
        benchmark::DoNotOptimize(d);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StatsDelta);

// ---------------------------------------------------------------------------
// RttStats::dump — RTT stats human-readable output
// ---------------------------------------------------------------------------

static void BM_RttStatsDump(benchmark::State& state) {
    RttStats r{.count = 1000, .min_ns = 500, .max_ns = 100000,
               .mean_ns = 10000.0, .p50_ns = 8000, .p99_ns = 80000, .p999_ns = 95000};
    for (auto _ : state) {
        auto d = r.dump();
        benchmark::DoNotOptimize(d);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RttStatsDump);

// ---------------------------------------------------------------------------
// ws::is_valid_utf8 — UTF-8 validation (text frame hot path)
// ---------------------------------------------------------------------------

static void BM_Utf8Validate_AsciiOnly(benchmark::State& state) {
    // 256 bytes of pure ASCII — fast path (all bytes < 0x80)
    std::string text(256, 'A');
    auto* data = reinterpret_cast<const uint8_t*>(text.data());
    for (auto _ : state) {
        auto ok = eph::net::ws::is_valid_utf8(data, text.size());
        benchmark::DoNotOptimize(ok);
    }
    state.SetBytesProcessed(state.iterations() * text.size());
}
BENCHMARK(BM_Utf8Validate_AsciiOnly);

static void BM_Utf8Validate_Mixed(benchmark::State& state) {
    // Mix of ASCII and multi-byte UTF-8 (typical JSON with some unicode)
    std::string text;
    for (int i = 0; i < 32; ++i) {
        text += "{\"symbol\":\"BTC\xC3\xB6USD\",\"price\":42000}";  // ö is 2-byte
    }
    auto* data = reinterpret_cast<const uint8_t*>(text.data());
    for (auto _ : state) {
        auto ok = eph::net::ws::is_valid_utf8(data, text.size());
        benchmark::DoNotOptimize(ok);
    }
    state.SetBytesProcessed(state.iterations() * text.size());
}
BENCHMARK(BM_Utf8Validate_Mixed);

static void BM_Utf8Validate_Invalid(benchmark::State& state) {
    // Invalid UTF-8 at byte 128 — measures early-exit performance
    std::string text(256, 'A');
    text[128] = static_cast<char>(0xFF);  // invalid byte
    auto* data = reinterpret_cast<const uint8_t*>(text.data());
    for (auto _ : state) {
        auto ok = eph::net::ws::is_valid_utf8(data, text.size());
        benchmark::DoNotOptimize(ok);
    }
    state.SetBytesProcessed(state.iterations() * text.size());
}
BENCHMARK(BM_Utf8Validate_Invalid);

// ---------------------------------------------------------------------------
// ws::opcode_name — opcode-to-string lookup (logging path)
// ---------------------------------------------------------------------------

static void BM_OpcodeName_Known(benchmark::State& state) {
    for (auto _ : state) {
        auto s = eph::net::ws::opcode_name(0x02);  // BINARY
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OpcodeName_Known);

static void BM_OpcodeName_Unknown(benchmark::State& state) {
    for (auto _ : state) {
        auto s = eph::net::ws::opcode_name(0x0F);  // unknown
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_OpcodeName_Unknown);

BENCHMARK_MAIN();
