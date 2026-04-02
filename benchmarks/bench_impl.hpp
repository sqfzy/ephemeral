/// @file bench_impl.hpp
/// Shared benchmark logic parameterized by TcpTransport backend.
///
/// Both scenarios use DirectTransport (no threads, no SPSC queues) for
/// the most accurate latency measurement. The app thread calls poll()
/// directly: TCP rx → WS decode → on_message callback.
///
/// Requires an external bench_mock_server to be running on server_ip:port.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/tcp_concept.hpp"
#include "eph/transport/direct_transport.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"
#include "bench_common.hpp"

namespace bench {

struct BenchConfig {
    std::string server_ip = "10.0.0.1";
    uint16_t server_port = 9999;
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    std::chrono::seconds duration{10};
    std::chrono::microseconds order_interval{1000};
    int poll_cpu = 2;   // CPU core for poll loop (default: core 2)
};

/// Pin current thread to poll_cpu. Exits on failure.
inline void pin_or_die(int cpu, const char* name) {
    auto r = eph::utils::set_thread_affinity(cpu, name);
    if (!r) {
        spdlog::error("Failed to pin {} to core {}: {}", name, cpu, r.error());
        std::exit(1);
    }
    spdlog::info("Pinned {} to core {}", name, cpu);
}

/// Parse "T" field (raw TSC cycles) from JSON.
inline uint64_t parse_tsc_field(const uint8_t* data, size_t len) {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    auto pos = json.find("\"T\":");
    if (pos == std::string_view::npos) return 0;
    pos += 4;
    while (pos < len && json[pos] == ' ') ++pos;
    uint64_t val = 0;
    while (pos < len && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        ++pos;
    }
    return val;
}

// ── Market Data Benchmark ──────────────────────────────────────────────────

/// Single-connection multi-symbol market data latency benchmark.
/// Uses DirectTransport — app thread polls directly, no queues.
/// Measures: mock sendmsg() TSC → app on_message TSC.
template <eph::net::TcpTransport TcpImpl>
void run_market_bench(
    std::function<std::expected<std::unique_ptr<TcpImpl>, std::string>()> tcp_factory,
    const BenchConfig& cfg)
{
    using Transport = eph::net::DirectTransport<TcpImpl, eph::net::WsFramer, 4096>;

    eph::net::TransportConfig tc;
    tc.remote_host = cfg.server_ip;
    tc.remote_port = cfg.server_port;
    tc.ws_path = "/ws";
    tc.use_tls = false;
    tc.ping_interval = std::chrono::seconds{0};
    tc.max_reconnect_attempts = 0;
    tc.skip_utf8_validation = true;

    eph::utils::HdrHistogram latency_hist{10, 1'000'000'000ULL, 3};
    uint64_t msg_count = 0;

    tc.on_message = [&](const uint8_t* data, uint16_t len, uint8_t) {
        uint64_t t_send = parse_tsc_field(data, len);
        if (t_send > 0) {
            uint64_t now = eph::utils::TSC::now();
            if (now > t_send) {
                auto ns = eph::utils::TSC::to_ns(now - t_send);
                if (ns) latency_hist.record(static_cast<uint64_t>(*ns));
            }
        }
        ++msg_count;
    };

    auto result = Transport::create(std::move(tcp_factory), tc);
    if (!result) {
        spdlog::error("Transport create failed: {}", result.error().message());
        return;
    }
    auto& transport = *result;

    pin_or_die(cfg.poll_cpu, "bench-poll");

    spdlog::info("Market bench started: {} symbols, duration={}s",
                 cfg.symbols.size(), cfg.duration.count());

    auto start = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_acquire)
           && transport->is_running()) {
        if (std::chrono::steady_clock::now() - start >= cfg.duration) break;
        auto r = transport->poll();
        if (!r && !transport->is_running()) break;
    }

    transport->stop();

    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== Market Data Benchmark Results ===");
    spdlog::info("Duration: {}s, Messages: {}, Rate: {} msg/s",
                 duration_s, msg_count,
                 duration_s > 0 ? msg_count / static_cast<uint64_t>(duration_s) : 0);
    print_latency("Pipeline Latency (mock send -> app recv)", hdr_to_stats(latency_hist));
}

// ── Order RTT Benchmark ────────────────────────────────────────────────────

/// Order send + ExecutionReport response RTT benchmark.
/// Uses DirectTransport — app thread alternates between send and poll.
/// Measures:
///   - Order RTT:        app send() TSC → app recv(executionReport) TSC
///   - Response Latency: mock sendmsg() TSC → app on_message TSC
template <eph::net::TcpTransport TcpImpl>
void run_order_rtt_bench(
    std::function<std::expected<std::unique_ptr<TcpImpl>, std::string>()> tcp_factory,
    const BenchConfig& cfg)
{
    using Transport = eph::net::DirectTransport<TcpImpl, eph::net::WsFramer, 4096>;

    eph::net::TransportConfig tc;
    tc.remote_host = cfg.server_ip;
    tc.remote_port = cfg.server_port;
    tc.ws_path = "/ws";
    tc.use_tls = false;
    tc.ping_interval = std::chrono::seconds{0};
    tc.max_reconnect_attempts = 0;
    tc.skip_utf8_validation = true;

    eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram resp_latency_hist{10, 1'000'000'000ULL, 3};
    uint64_t last_order_tsc = 0;  // single-threaded, no atomic needed
    uint64_t order_count = 0;
    uint64_t response_count = 0;

    tc.on_message = [&](const uint8_t* data, uint16_t len, uint8_t) {
        std::string_view json(reinterpret_cast<const char*>(data), len);
        if (json.find("\"e\":\"executionReport\"") != std::string_view::npos) {
            // Order RTT
            if (last_order_tsc > 0) {
                uint64_t now = eph::utils::TSC::now();
                if (now > last_order_tsc) {
                    auto ns = eph::utils::TSC::to_ns(now - last_order_tsc);
                    if (ns) rtt_hist.record(static_cast<uint64_t>(*ns));
                }
            }
            // Response latency (mock "T" → now)
            uint64_t t_mock = parse_tsc_field(data, len);
            if (t_mock > 0) {
                uint64_t now = eph::utils::TSC::now();
                if (now > t_mock) {
                    auto ns = eph::utils::TSC::to_ns(now - t_mock);
                    if (ns) resp_latency_hist.record(static_cast<uint64_t>(*ns));
                }
            }
            ++response_count;
        }
    };

    auto result = Transport::create(std::move(tcp_factory), tc);
    if (!result) {
        spdlog::error("Transport create failed: {}", result.error().message());
        return;
    }
    auto& transport = *result;

    pin_or_die(cfg.poll_cpu, "bench-poll");

    spdlog::info("Order RTT bench started: interval={}us, duration={}s",
                 cfg.order_interval.count(), cfg.duration.count());

    auto start = std::chrono::steady_clock::now();
    auto next_order = start;

    while (g_running.load(std::memory_order_acquire)
           && transport->is_running()) {
        auto now_tp = std::chrono::steady_clock::now();
        if (now_tp - start >= cfg.duration) break;

        // Send order at configured interval
        if (now_tp >= next_order) {
            last_order_tsc = eph::utils::TSC::now();
            char buf[256];
            int n = std::snprintf(buf, sizeof(buf),
                R"({"method":"order.place","symbol":"BTCUSDT",)"
                R"("side":"BUY","price":"50000.00",)"
                R"("quantity":"0.001","T_send":%llu})",
                static_cast<unsigned long long>(last_order_tsc));
            transport->send_text(std::string_view(buf, static_cast<size_t>(n)));
            ++order_count;
            next_order += cfg.order_interval;
        }

        // Poll for incoming data (market data + order responses)
        transport->poll();
    }

    transport->stop();

    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== Order RTT Benchmark Results ===");
    spdlog::info("Duration: {}s, Orders sent: {}, Responses: {}, Rate: {} order/s",
                 duration_s, order_count, response_count,
                 duration_s > 0 ? order_count / static_cast<uint64_t>(duration_s) : 0);
    print_latency("Order RTT (send -> response recv)", hdr_to_stats(rtt_hist));
    print_latency("Response Latency (mock send -> app recv)", hdr_to_stats(resp_latency_hist));
}

} // namespace bench
