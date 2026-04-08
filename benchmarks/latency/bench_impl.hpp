/// @file bench_impl.hpp
/// Shared benchmark logic parameterized by TcpTransport backend.
///
/// Both scenarios use DirectTransport (no threads, no SPSC queues).
/// The bench templates are pure clients — they connect to an already-running
/// mock server. Callers are responsible for starting the mock server
/// (either externally via bench_mock_server, or in-process via start_mock()).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/tcp_concept.hpp"
#include "eph/transport/direct_transport.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"
#include "bench_common.hpp"
#include "mock/mock_ws_server.hpp"

namespace bench {

struct BenchConfig {
    std::string server_ip;  // required: IP of mock server
    uint16_t server_port = 9999;
    std::vector<std::string> symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT"};
    std::chrono::seconds duration{10};
    std::chrono::microseconds tick_interval{100};
    std::chrono::microseconds order_interval{1000};
    int poll_cpu = 2;
    int mock_cpu = 4;
};

/// Parse a named TSC field from JSON. Returns 0 if not found.
inline uint64_t parse_json_tsc(std::string_view json, std::string_view key) {
    auto pos = json.find(key);
    if (pos == std::string_view::npos) return 0;
    pos += key.size();
    while (pos < json.size() && json[pos] == ' ') ++pos;
    uint64_t val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + static_cast<uint64_t>(json[pos] - '0');
        ++pos;
    }
    return val;
}

/// Parse "T" field (raw TSC cycles) from JSON.
/// Searches for ,"T": to avoid matching "T_send" or "T_recv".
/// Also matches {"T": at the start of JSON.
inline uint64_t parse_tsc_field(const uint8_t* data, size_t len) {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    // Try ,"T": first (most common, mid-object)
    uint64_t val = parse_json_tsc(json, ",\"T\":");
    if (val > 0) return val;
    // Fallback: {"T": (start of object)
    return parse_json_tsc(json, "{\"T\":");
}

/// Parse "T_recv" field from ExecutionReport JSON.
/// Server stamps this at the moment it receives the client's order.
inline uint64_t parse_tsc_recv_field(const uint8_t* data, size_t len) {
    std::string_view json(reinterpret_cast<const char*>(data), len);
    return parse_json_tsc(json, "\"T_recv\":");
}

/// Pin current thread to cpu. Exits process on failure.
inline void pin_or_die(int cpu, const char* name) {
    auto r = eph::utils::set_thread_affinity(cpu, name);
    if (!r) {
        spdlog::error("Failed to pin {} to core {}: {}", name, cpu, r.error());
        std::exit(1);
    }
    spdlog::info("Pinned {} to core {}", name, cpu);
}

// ── In-process mock server (used by DPDK bench) ───────────────────────────

struct MockHandle {
    std::thread thread;
    std::unique_ptr<std::atomic<bool>> running = std::make_unique<std::atomic<bool>>(true);
};

inline MockHandle start_mock(const BenchConfig& cfg, bool order_mode) {
    MockHandle h;
    mock::MockServerConfig mock_cfg{
        .bind_ip = cfg.server_ip,
        .port = cfg.server_port,
        .symbols = cfg.symbols,
        .tick_interval = cfg.tick_interval,
        .order_mode = order_mode,
    };
    int mock_cpu = cfg.mock_cpu;
    h.thread = std::thread([mock_cfg, mock_cpu, &running = *h.running] {
        pin_or_die(mock_cpu, "mock-server");
        mock::run_mock_ws_server(mock_cfg, running);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    return h;
}

inline void stop_mock(MockHandle& h) {
    *h.running = false;
    if (h.thread.joinable()) h.thread.join();
}

// ── Shared TransportConfig builder ─────────────────────────────────────────

/// Build a TransportConfig for benchmark (no TLS, no pings, no reconnect).
inline eph::net::TransportConfig make_bench_transport_config(const BenchConfig& cfg) {
    eph::net::TransportConfig tc;
    tc.remote_host = cfg.server_ip;
    tc.remote_port = cfg.server_port;
    tc.ws_path = "/ws";
    tc.use_tls = false;
    tc.ping_interval = std::chrono::seconds{0};
    tc.max_reconnect_attempts = 0;
    tc.skip_utf8_validation = true;
    return tc;
}

// ── Market Data Benchmark ──────────────────────────────────────────────────

/// Run market bench on a transport that supports poll() (DirectTransport).
/// The transport must already be created and connected.
/// Caller is responsible for stopping the transport after this returns.
template <typename TransportT>
void run_market_poll_loop(TransportT& transport, const BenchConfig& cfg,
                          const char* label = "Market Data Benchmark Results") {
    eph::utils::HdrHistogram latency_hist{10, 1'000'000'000ULL, 3};
    uint64_t msg_count = 0;

    // Hijack on_message for latency recording (must be set before first poll)
    auto& tc = const_cast<eph::net::TransportConfig&>(transport.config());
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

    pin_or_die(cfg.poll_cpu, "bench-poll");

    spdlog::info("Market bench started: {} symbols, duration={}s",
                 cfg.symbols.size(), cfg.duration.count());

    auto start = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_acquire) && transport.is_running()) {
        auto now_tp = std::chrono::steady_clock::now();
        if (now_tp - start >= cfg.duration) break;
        [[maybe_unused]] auto poll_result = transport.poll();
    }

    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== {} ===", label);
    spdlog::info("Duration: {}s, Messages: {}, Rate: {} msg/s",
                 duration_s, msg_count,
                 duration_s > 0 ? msg_count / static_cast<uint64_t>(duration_s) : 0);
    print_latency("Pipeline Latency (mock send -> app recv)", hdr_to_stats(latency_hist));
}

/// Socket market bench: create DirectTransport + run poll loop.
template <eph::net::TcpTransport TcpImpl>
void run_market_bench(
    std::function<std::expected<std::unique_ptr<TcpImpl>, std::string>()> tcp_factory,
    const BenchConfig& cfg)
{
    using Transport = eph::net::DirectTransport<TcpImpl, eph::net::WsFramer, 4096>;

    auto tc = make_bench_transport_config(cfg);

    auto result = Transport::create(std::move(tcp_factory), tc);
    if (!result) {
        spdlog::error("Transport create failed: {}", result.error().message());
        return;
    }

    run_market_poll_loop(**result, cfg);
    (*result)->stop();
}

// ── Order RTT Benchmark (pure client) ──────────────────────────────────────

/// Run order RTT poll loop on an already-connected DirectTransport.
template <typename TransportT>
void run_order_rtt_poll_loop(TransportT& transport, const BenchConfig& cfg,
                              const char* label = "Order RTT Benchmark Results") {
    eph::utils::HdrHistogram rtt_hist{10, 1'000'000'000ULL, 3};
    eph::utils::HdrHistogram tx_hist{10, 1'000'000'000ULL, 3};   // client_send → server_recv
    eph::utils::HdrHistogram rx_hist{10, 1'000'000'000ULL, 3};   // server_send → client_recv
    eph::utils::HdrHistogram srv_hist{10, 1'000'000'000ULL, 3};  // server_recv → server_send
    uint64_t last_order_tsc = 0;
    uint64_t order_count = 0, response_count = 0;

    auto& tc = const_cast<eph::net::TransportConfig&>(transport.config());
    tc.on_message = [&](const uint8_t* data, uint16_t len, uint8_t) {
        std::string_view json(reinterpret_cast<const char*>(data), len);
        if (json.find("\"e\":\"executionReport\"") != std::string_view::npos) {
            uint64_t client_recv_tsc = eph::utils::TSC::now();

            // RTT: client_send → client_recv
            if (last_order_tsc > 0 && client_recv_tsc > last_order_tsc) {
                auto ns = eph::utils::TSC::to_ns(client_recv_tsc - last_order_tsc);
                if (ns) rtt_hist.record(static_cast<uint64_t>(*ns));
            }

            // RX: server_send (T field) → client_recv
            uint64_t t_send = parse_tsc_field(data, len);
            if (t_send > 0 && client_recv_tsc > t_send) {
                auto ns = eph::utils::TSC::to_ns(client_recv_tsc - t_send);
                if (ns) rx_hist.record(static_cast<uint64_t>(*ns));
            }

            // TX: client_send → server_recv (T_recv field)
            uint64_t t_recv = parse_tsc_recv_field(data, len);
            if (t_recv > 0 && last_order_tsc > 0 && t_recv > last_order_tsc) {
                auto ns = eph::utils::TSC::to_ns(t_recv - last_order_tsc);
                if (ns) tx_hist.record(static_cast<uint64_t>(*ns));
            }

            // Server processing: server_recv → server_send
            if (t_recv > 0 && t_send > 0 && t_send > t_recv) {
                auto ns = eph::utils::TSC::to_ns(t_send - t_recv);
                if (ns) srv_hist.record(static_cast<uint64_t>(*ns));
            }

            ++response_count;
        }
    };

    pin_or_die(cfg.poll_cpu, "bench-poll");

    spdlog::info("Order RTT bench started: interval={}us, duration={}s",
                 cfg.order_interval.count(), cfg.duration.count());

    auto start = std::chrono::steady_clock::now();
    auto next_order = start;

    while (g_running.load(std::memory_order_acquire) && transport.is_running()) {
        auto now_tp = std::chrono::steady_clock::now();
        if (now_tp - start >= cfg.duration) break;

        if (now_tp >= next_order) {
            last_order_tsc = eph::utils::TSC::now();
            char buf[256];
            int n = std::snprintf(buf, sizeof(buf),
                R"({"method":"order.place","symbol":"BTCUSDT",)"
                R"("side":"BUY","price":"50000.00",)"
                R"("quantity":"0.001","T_send":%llu})",
                static_cast<unsigned long long>(last_order_tsc));
            [[maybe_unused]] auto send_err = transport.send_text(std::string_view(buf, static_cast<size_t>(n)));
            ++order_count;
            next_order += cfg.order_interval;
        }

        [[maybe_unused]] auto poll_result = transport.poll();
    }

    auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    spdlog::info("=== {} ===", label);
    spdlog::info("Duration: {}s, Orders: {}, Responses: {}, Rate: {} order/s",
                 duration_s, order_count, response_count,
                 duration_s > 0 ? order_count / static_cast<uint64_t>(duration_s) : 0);
    print_latency("Order RTT (send -> response recv)", hdr_to_stats(rtt_hist));
    print_latency("TX (client send -> server recv)", hdr_to_stats(tx_hist));
    print_latency("RX (server send -> client recv)", hdr_to_stats(rx_hist));
    print_latency("Server (recv -> send)", hdr_to_stats(srv_hist));
}

/// Socket order RTT bench: create DirectTransport + run poll loop.
template <eph::net::TcpTransport TcpImpl>
void run_order_rtt_bench(
    std::function<std::expected<std::unique_ptr<TcpImpl>, std::string>()> tcp_factory,
    const BenchConfig& cfg)
{
    using Transport = eph::net::DirectTransport<TcpImpl, eph::net::WsFramer, 4096>;

    auto tc = make_bench_transport_config(cfg);

    auto result = Transport::create(std::move(tcp_factory), tc);
    if (!result) {
        spdlog::error("Transport create failed: {}", result.error().message());
        return;
    }

    run_order_rtt_poll_loop(**result, cfg);
    (*result)->stop();
}

} // namespace bench
