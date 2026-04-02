/// @file bench_impl.hpp
/// Shared benchmark logic parameterized by TcpTransport backend.
///
/// Provides two benchmark scenarios as function templates:
///   - run_market_bench:     single-connection multi-symbol market data latency
///   - run_order_rtt_bench:  order send + execution report RTT
///
/// Both scenarios:
///   1. Start a mock WS server in a background thread (kernel TCP on NIC-A)
///   2. Create Transport<TcpImpl> and connect to the mock server
///   3. Measure latency via TSC timestamps embedded in JSON "T" fields
///   4. Record samples in HdrHistogram and print results
///
/// The TcpImpl template parameter selects the backend (SocketTransport vs
/// DPDK TcpSession). Benchmark binaries (bench_market_single.cpp, etc.)
/// instantiate these templates with the concrete backend type.

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
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/tcp_concept.hpp"
#include "eph/transport/transport.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"
#include "bench_common.hpp"
#include "mock/mock_ws_server.hpp"

namespace bench {

// ── Configuration ─────────────────────────────────────────────────────────

struct BenchConfig {
    std::string server_ip = "10.0.0.1";
    uint16_t server_port = 9999;
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    std::chrono::seconds duration{10};
    std::chrono::microseconds tick_interval{100};    // mock server push rate
    std::chrono::microseconds order_interval{1000};  // order send interval
    int rx_cpu = -1;
    int tx_cpu = -1;
    int main_cpu = -1;
    bool start_mock = true;  // false = external mock server (namespace mode)
};

// ── JSON "T" field parser ─────────────────────────────────────────────────

/// Parse "T" field from JSON using fixed pattern scan.
/// Scans for "\"T\":" and parses the unsigned integer value that follows.
/// Returns 0 if not found or on parse failure.
inline uint64_t parse_tsc_field(const uint8_t* data, size_t len) {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    auto pos = json.find("\"T\":");
    if (pos == std::string_view::npos) return 0;
    pos += 4;  // skip past "T":
    // Skip optional whitespace
    while (pos < len && json[pos] == ' ') ++pos;
    uint64_t val = 0;
    while (pos < len && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        ++pos;
    }
    return val;
}

// ── Market data benchmark ─────────────────────────────────────────────────

/// Run single-connection multi-symbol market data benchmark.
///
/// Measures pipeline latency: mock server sendmsg() TSC -> app on_message TSC.
///
/// Steps:
///   1. Start mock WS server in a background thread
///   2. Create Transport<TcpImpl> and connect to mock server
///   3. Receive market data for `duration` seconds
///   4. For each message: read "T" field, compute TSC::now() - T, record in histogram
///   5. Print results
///
/// @tparam TcpImpl  A type satisfying the TcpTransport concept.
/// @param tcp_factory  Callable returning expected<unique_ptr<TcpImpl>, string>.
/// @param cfg          Benchmark configuration.
template <eph::net::TcpTransport TcpImpl>
void run_market_bench(
    std::function<std::expected<std::unique_ptr<TcpImpl>, std::string>()> tcp_factory,
    const BenchConfig& cfg)
{
    // 1. Start mock server (skip if external mock via --no-mock)
    std::atomic<bool> mock_running{true};
    std::thread mock_thread;
    if (cfg.start_mock) {
        bench::mock::MockServerConfig mock_cfg{
            .bind_ip = cfg.server_ip,
            .port = cfg.server_port,
            .symbols = cfg.symbols,
            .tick_interval = cfg.tick_interval,
            .order_mode = false,
        };
        mock_thread = std::thread([mock_cfg, &mock_running] {
            bench::mock::run_mock_ws_server(mock_cfg, mock_running);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    // 2. Create transport config (no TLS, no reconnect, no pings)
    eph::net::TransportConfig tc;
    tc.remote_host = cfg.server_ip;
    tc.remote_port = cfg.server_port;
    tc.ws_path = "/ws";
    tc.use_tls = false;
    tc.ping_interval = std::chrono::seconds{0};  // disable pings
    tc.max_reconnect_attempts = 0;
    tc.skip_utf8_validation = true;

    // Latency histogram: 10ns - 1s, 3 significant digits
    eph::utils::HdrHistogram latency_hist{10, 1'000'000'000ULL, 3};
    uint64_t msg_count = 0;

    // Set on_message callback for latency recording
    tc.on_message = [&](const uint8_t* data, uint16_t len, uint8_t /*opcode*/) {
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

    if (cfg.rx_cpu >= 0) tc.rx_cpu = cfg.rx_cpu;
    if (cfg.tx_cpu >= 0) tc.tx_cpu = cfg.tx_cpu;

    // 3. Create and connect transport
    auto result = eph::net::Transport<TcpImpl>::create(std::move(tcp_factory), tc);
    if (!result) {
        spdlog::error("Transport create failed: {}", result.error().message());
        mock_running = false;
        if (mock_thread.joinable()) mock_thread.join();
        return;
    }
    auto& transport = *result;

    spdlog::info("Market bench started: {} symbols, duration={}s",
                 cfg.symbols.size(), cfg.duration.count());

    // 4. Run for duration (or until SIGINT)
    auto start = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_acquire)) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= cfg.duration) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    // 5. Stop and report
    transport->stop();
    mock_running = false;
    if (mock_thread.joinable()) mock_thread.join();

    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== Market Data Benchmark Results ===");
    spdlog::info("Duration: {}s, Messages: {}, Rate: {} msg/s",
                 duration_s, msg_count,
                 duration_s > 0 ? msg_count / static_cast<uint64_t>(duration_s) : 0);
    print_latency("Pipeline Latency (mock send -> app recv)", hdr_to_stats(latency_hist));
}

// ── Order RTT benchmark ───────────────────────────────────────────────────

/// Run order RTT benchmark.
///
/// Measures two latencies:
///   - Order RTT:          app send() TSC -> app recv(executionReport) TSC
///   - Response Latency:   mock sendmsg() TSC -> app on_message TSC
///
/// Steps:
///   1. Start mock WS server in order mode
///   2. Create Transport, connect
///   3. Periodically send order JSON with T_send = TSC::now()
///   4. on_message: if executionReport, compute RTT and response latency
///   5. Print results
///
/// @tparam TcpImpl  A type satisfying the TcpTransport concept.
/// @param tcp_factory  Callable returning expected<unique_ptr<TcpImpl>, string>.
/// @param cfg          Benchmark configuration.
template <eph::net::TcpTransport TcpImpl>
void run_order_rtt_bench(
    std::function<std::expected<std::unique_ptr<TcpImpl>, std::string>()> tcp_factory,
    const BenchConfig& cfg)
{
    // 1. Start mock server in order mode (skip if external mock via --no-mock)
    std::atomic<bool> mock_running{true};
    std::thread mock_thread;
    if (cfg.start_mock) {
        bench::mock::MockServerConfig mock_cfg{
            .bind_ip = cfg.server_ip,
            .port = cfg.server_port,
            .symbols = cfg.symbols,
            .tick_interval = cfg.tick_interval,
            .order_mode = true,
        };
        mock_thread = std::thread([mock_cfg, &mock_running] {
            bench::mock::run_mock_ws_server(mock_cfg, mock_running);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }

    // 2. Transport config
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
    std::atomic<uint64_t> last_order_tsc{0};
    uint64_t order_count = 0;
    uint64_t response_count = 0;

    tc.on_message = [&](const uint8_t* data, uint16_t len, uint8_t /*opcode*/) {
        std::string_view json(reinterpret_cast<const char*>(data), len);

        // Check if this is an executionReport (order response)
        if (json.find("\"e\":\"executionReport\"") != std::string_view::npos) {
            // Order RTT: TSC::now() - last_order_tsc
            uint64_t send_tsc = last_order_tsc.load(std::memory_order_acquire);
            if (send_tsc > 0) {
                uint64_t now = eph::utils::TSC::now();
                if (now > send_tsc) {
                    auto ns = eph::utils::TSC::to_ns(now - send_tsc);
                    if (ns) rtt_hist.record(static_cast<uint64_t>(*ns));
                }
            }
            // Response latency: mock "T" -> now
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
        // Market data messages are ignored for RTT (but they exercise the pipeline)
    };

    if (cfg.rx_cpu >= 0) tc.rx_cpu = cfg.rx_cpu;
    if (cfg.tx_cpu >= 0) tc.tx_cpu = cfg.tx_cpu;

    auto result = eph::net::Transport<TcpImpl>::create(std::move(tcp_factory), tc);
    if (!result) {
        spdlog::error("Transport create failed: {}", result.error().message());
        mock_running = false;
        if (mock_thread.joinable()) mock_thread.join();
        return;
    }
    auto& transport = *result;

    spdlog::info("Order RTT bench started: interval={}us, duration={}s",
                 cfg.order_interval.count(), cfg.duration.count());

    // 3-4. Send orders periodically
    auto start = std::chrono::steady_clock::now();
    auto next_order = start;

    while (g_running.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        if (now - start >= cfg.duration) break;

        if (now >= next_order) {
            // Build order JSON with T_send TSC
            uint64_t send_tsc = eph::utils::TSC::now();
            last_order_tsc.store(send_tsc, std::memory_order_release);

            char order_buf[256];
            int order_len = std::snprintf(order_buf, sizeof(order_buf),
                R"({"method":"order.place","symbol":"BTCUSDT",)"
                R"("side":"BUY","price":"50000.00",)"
                R"("quantity":"0.001","T_send":%lu})",
                static_cast<unsigned long>(send_tsc));

            transport->send_text(
                std::string_view(order_buf, static_cast<size_t>(order_len)));
            ++order_count;
            next_order += cfg.order_interval;
        }

        std::this_thread::yield();
    }

    // 5. Stop and report
    transport->stop();
    mock_running = false;
    if (mock_thread.joinable()) mock_thread.join();

    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== Order RTT Benchmark Results ===");
    spdlog::info("Duration: {}s, Orders sent: {}, Responses: {}, Rate: {} order/s",
                 duration_s, order_count, response_count,
                 duration_s > 0 ? order_count / static_cast<uint64_t>(duration_s) : 0);
    print_latency("Order RTT (send -> response recv)", hdr_to_stats(rtt_hist));
    print_latency("Response Latency (mock send -> app recv)",
                  hdr_to_stats(resp_latency_hist));
}

} // namespace bench
