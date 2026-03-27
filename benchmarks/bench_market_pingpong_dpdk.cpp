/// @file bench_market_pingpong_dpdk.cpp
/// Combined market data + ping/pong latency benchmark — DPDK backend.
///
/// Subscribes to Binance combined bookTicker stream (market data) while
/// simultaneously sending periodic WebSocket pings. Measures both:
///   1) RX market data pipeline latency (rx_burst → frame decoded)
///   2) RTT ping/pong round-trip time (ping tx → pong rx)
///
/// This simulates a real trading scenario: receiving market data while
/// maintaining heartbeat/order-path latency on the same connection.
///
/// Usage:
///   sudo ./bench_market_pingpong_dpdk -a 0000:28:00.0 -l 4-7
///       -- --local-ip 172.31.23.112 --gateway-ip 172.31.16.1
///       --rx-cpu 8 --tx-cpu 9 --main-cpu 10
///       --ping-interval 200 --payload-size 125 --duration 30

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

using BenchTransport = eph::net::Transport<
    eph::dpdk::TcpSession<>,
    eph::net::WsFramer,
    16384, 1024,
    eph::containers::EvictingQueue,
    false  // LastOnlyDeliver=false — deliver all market data frames
>;

/// 4-byte direct hash for Binance combined stream symbol extraction.
static uint32_t binance_symbol_hash(const uint8_t* data, size_t len) {
    if (len < 15) [[unlikely]] return 0;
    if (data[0] != '{') [[unlikely]] return 0;
    uint32_t h;
    std::memcpy(&h, data + 11, 4);
    return h;
}

struct Config {
    std::string host       = "fstream.binance.com";
    uint16_t    port       = 443;
    std::vector<std::string> symbols = {"btcusdt", "ethusdt", "solusdt"};
    std::string local_ip{};
    std::string gateway_ip{};
    uint16_t    dpdk_port  = 0;
    int  duration          = 30;
    int  ping_interval     = 200;   // ms between pings
    int  payload_size      = 125;   // ping payload bytes (0-125)
    bool use_tls           = true;
    bool verify            = false;
    bool use_twophase      = true;
    int  tx_cpu            = -1;
    int  rx_cpu            = -1;
    int  main_cpu          = -1;
};

static std::atomic<bool> g_running{true};
static void sig(int) { g_running.store(false, std::memory_order_release); }

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> tokens;
    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, delim))
        if (!token.empty()) tokens.push_back(token);
    return tokens;
}

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")           c.host           = next(a);
        else if (a == "--port")           c.port           = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbols")        c.symbols        = split(next(a), ',');
        else if (a == "--local-ip")       c.local_ip       = next(a);
        else if (a == "--gateway-ip")     c.gateway_ip     = next(a);
        else if (a == "--dpdk-port")      c.dpdk_port      = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--duration")       c.duration       = std::atoi(next(a));
        else if (a == "--ping-interval")  c.ping_interval  = std::atoi(next(a));
        else if (a == "--payload-size")   c.payload_size   = std::clamp(std::atoi(next(a)), 0, 125);
        else if (a == "--tx-cpu")         c.tx_cpu         = std::atoi(next(a));
        else if (a == "--rx-cpu")         c.rx_cpu         = std::atoi(next(a));
        else if (a == "--main-cpu")       c.main_cpu       = std::atoi(next(a));
        else if (a == "--no-tls")         c.use_tls        = false;
        else if (a == "--no-twophase")    c.use_twophase   = false;
    }
    return c;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, sig);
    std::signal(SIGTERM, sig);
    spdlog::set_level(spdlog::level::info);

    int eal_argc = argc; char** eal_argv = argv;
    int app_argc = 0;    char** app_argv = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            eal_argc = i; app_argc = argc - i - 1; app_argv = argv + i + 1; break;
        }
    }
    auto cfg = parse_args(app_argc, app_argv);
    if (cfg.main_cpu >= 0) (void)eph::utils::set_thread_affinity(cfg.main_cpu, "main");
    if (cfg.local_ip.empty() || cfg.gateway_ip.empty()) {
        spdlog::error("--local-ip and --gateway-ip are required"); return 1;
    }

    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed"); return 1;
    }

    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(eal_argc, eal_argv);
    if (!eal) { spdlog::error("EAL init failed: {}", eal.error()); return 1; }

    // Build stream path using /ws/<stream1>/<stream2> format.
    // Unlike /stream?streams=, the /ws/ endpoint responds to client pings,
    // which is required for RTT measurement.
    std::string ws_path = "/ws";
    for (auto& sym : cfg.symbols)
        ws_path += "/" + sym + "@bookTicker";

    eph::net::TransportConfig tc{
        .remote_host = cfg.host, .remote_port = cfg.port,
        .ws_path = ws_path, .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{0},  // manual pings
        .skip_utf8_validation = true,
        .tx_cpu = cfg.tx_cpu, .rx_cpu = cfg.rx_cpu,
        .on_state_change = [](eph::net::TransportEvent e, std::string_view d) {
            spdlog::info("[STATE] {} — {}", eph::net::transport_event_name(e), d);
        },
        .on_frame_filter = cfg.use_twophase
            ? eph::net::make_twophase_filter(binance_symbol_hash)
            : eph::net::FrameFilterFn{},
    };

    spdlog::info("Connecting via DPDK to wss://{}:{}{}", cfg.host, cfg.port, ws_path);
    spdlog::info("  ping: every {}ms, {} byte payload", cfg.ping_interval, cfg.payload_size);
    auto conn = eph::dpdk::connect<BenchTransport>(
        eph::dpdk::DpdkEndpoint{.local_ip = cfg.local_ip, .gateway_ip = cfg.gateway_ip},
        tc, eph::dpdk::ConnectorOptions{.platform = {.port_id = cfg.dpdk_port}});
    if (!conn) { spdlog::error("DPDK connect failed: {}", conn.error()); return 1; }
    auto& tp = *conn->transport;
    spdlog::info("Connected via DPDK ({} symbols + ping)!", cfg.symbols.size());

    // ── Order simulation setup ─────────────────────────────────────────────
    // Send a JSON "order" every ping_interval ms. The mock server (or any
    // echo-like server) responds with a JSON containing the same "id" field.
    // We measure order RTT = time from send to matching response in recv().
    eph::utils::HdrHistogram order_rtt_hist{100, 1'000'000'000ULL, 3};  // 100ns-1s
    uint64_t order_send_tsc = 0;  // TSC when last order was sent
    int orders_sent = 0;
    int orders_received = 0;
    int order_id_counter = 0;
    bool order_pending = false;  // waiting for response?

    // ── Main loop: recv market data + send periodic orders ──────────────────
    uint64_t msgs = 0;
    auto start = std::chrono::steady_clock::now();
    auto deadline = cfg.duration > 0
        ? start + std::chrono::seconds(cfg.duration)
        : std::chrono::steady_clock::time_point::max();
    auto last_order = start;
    auto interval = std::chrono::milliseconds(cfg.ping_interval);

    // Per-second status line
    auto prev_hist = tp.rx_latency_histogram_snapshot();
    uint64_t prev_msgs = 0;
    auto window_start = start;
    int window_idx = 0;

    spdlog::info("{:>8} {:>10} {:>10} {:>10} {:>12} {:>8}",
                 "time", "msg/s", "p50(ns)", "p99(ns)", "p99.9(ns)", "orders");

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        // Send periodic simulated order (JSON text frame)
        if (now - last_order >= interval && !order_pending) {
            ++order_id_counter;
            auto order_json = std::format(
                R"({{"id":"ord_{}","method":"order.place","params":{{"symbol":"BTCUSDT","side":"BUY","type":"LIMIT","quantity":"0.001","price":"67000.00"}}}})",
                order_id_counter);
            auto rc = tp.send_text(order_json);
            if (rc == eph::net::SendError::kOk) {
                order_send_tsc = eph::utils::TSC::now();
                order_pending = true;
                ++orders_sent;
            }
            last_order = now;
        }

        // Drain messages: market data + order responses
        bool got = tp.recv([&](const uint8_t* data, size_t len) {
            // Check if this is an order response (contains "id":"ord_")
            std::string_view sv(reinterpret_cast<const char*>(data), len);
            if (sv.find("\"id\":\"ord_") != std::string_view::npos) {
                // Order response — measure RTT
                if (order_pending && order_send_tsc > 0) {
                    uint64_t now_tsc = eph::utils::TSC::now();
                    auto rtt_ns = eph::utils::TSC::to_ns(now_tsc - order_send_tsc);
                    if (rtt_ns) {
                        order_rtt_hist.record(static_cast<uint64_t>(*rtt_ns));
                    }
                    order_pending = false;
                    ++orders_received;
                }
            } else {
                // Market data
                ++msgs;
            }
        });
        if (!got) eph::utils::cpu_relax();

        // Per-second window
        if (std::chrono::steady_clock::now() - window_start >= std::chrono::seconds(1)) {
            auto snap_time = std::chrono::steady_clock::now();
            ++window_idx;
            auto curr_hist = tp.rx_latency_histogram_snapshot();
            auto delta = curr_hist;
            (void)delta.subtract(prev_hist);

            uint64_t delta_msgs = msgs - prev_msgs;
            double elapsed_s = std::chrono::duration<double>(snap_time - window_start).count();
            double rate = delta_msgs / elapsed_s;

            auto ws = delta.get_total_count() > 0 ? eph::net::RttStats{
                .count = delta.get_total_count(),
                .p50_ns = delta.get_value_at_percentile(50.0),
                .p99_ns = delta.get_value_at_percentile(99.0),
                .p999_ns = delta.get_value_at_percentile(99.9),
            } : eph::net::RttStats{};

            spdlog::info("[T+{:>3}s] {:>8.0f} {:>10} {:>10} {:>12} {:>8}",
                         window_idx, rate, ws.p50_ns, ws.p99_ns, ws.p999_ns, orders_sent);

            prev_hist = std::move(curr_hist);
            prev_msgs = msgs;
            window_start = snap_time;
        }
    }

    // ── Report ──────────────────────────────────────────────────────────────
    tp.stop();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    auto stats = tp.stats();
    auto& rx = stats.rx_latency;
    double avg_bytes_per_burst = stats.tcp_rx_bursts > 0
        ? static_cast<double>(stats.rx_bytes) / stats.tcp_rx_bursts : 0.0;

    spdlog::info("=== Market Data + Order Simulation Benchmark (DPDK) ===");
    spdlog::info("Symbols: {} | Duration: {:.1f}s | Messages: {} | Orders: {}/{}",
                 cfg.symbols.size(), elapsed_ms / 1000.0, msgs,
                 orders_received, orders_sent);
    spdlog::info("RX totals: {} bytes, {} WS frames, {} TLS records, {} TCP pkts, {} bursts",
                 stats.rx_bytes, stats.rx_packets, rx.count,
                 stats.tcp_rx_packets, stats.tcp_rx_bursts);
    if (stats.tcp_rx_bursts > 0) {
        spdlog::info("Per rx_burst: {:.0f} bytes, {:.1f} WS frames, {:.1f} TLS records, {:.1f} TCP pkts",
                     avg_bytes_per_burst,
                     static_cast<double>(stats.rx_packets) / stats.tcp_rx_bursts,
                     static_cast<double>(rx.count) / stats.tcp_rx_bursts,
                     static_cast<double>(stats.tcp_rx_packets) / stats.tcp_rx_bursts);
    }
    spdlog::info("Transport stats:\n{}", stats.dump());

    auto print_latency = [](std::string_view label, const eph::net::RttStats& s) {
        spdlog::info("--- {} ---", label);
        if (s.count > 0) {
            spdlog::info("  samples: {}", s.count);
            spdlog::info("  min:     {:.0f} ns", static_cast<double>(s.min_ns));
            spdlog::info("  p50:     {:.0f} ns", static_cast<double>(s.p50_ns));
            spdlog::info("  p99:     {:.0f} ns", static_cast<double>(s.p99_ns));
            spdlog::info("  p99.9:   {:.0f} ns", static_cast<double>(s.p999_ns));
            spdlog::info("  max:     {:.0f} ns", static_cast<double>(s.max_ns));
        } else {
            spdlog::info("  (no samples)");
        }
    };

    print_latency("RX Market Data (rx_burst → frame decoded)", stats.rx_latency);
    print_latency("TX Order (send_text enqueue → flush)", stats.tx_latency);

    // Order RTT from our own histogram
    auto order_rtt = order_rtt_hist.get_total_count() > 0 ? eph::net::RttStats{
        .count   = order_rtt_hist.get_total_count(),
        .min_ns  = order_rtt_hist.get_min_value(),
        .max_ns  = order_rtt_hist.get_max_value(),
        .mean_ns = order_rtt_hist.get_mean(),
        .p50_ns  = order_rtt_hist.get_value_at_percentile(50.0),
        .p99_ns  = order_rtt_hist.get_value_at_percentile(99.0),
        .p999_ns = order_rtt_hist.get_value_at_percentile(99.9),
    } : eph::net::RttStats{};
    print_latency("Order RTT (send → response received)", order_rtt);

    return 0;
}
