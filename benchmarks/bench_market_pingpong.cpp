/// @file bench_market_pingpong.cpp
/// Combined market data + order simulation benchmark — socket (kernel) backend.
///
/// Subscribes to Binance combined bookTicker stream while simultaneously
/// sending periodic simulated orders (JSON text frames). Measures:
///   1) RX Pipeline:   rx_burst → market data frame decoded
///   2) TX Pipeline:   order enqueue → tx_burst
///   3) Order RTT:     order send → response received (application-level TSC)
///   4) Feed Latency:  server event timestamp → app recv
///
/// Uses /ws/<sym>@bookTicker path (not /stream?streams=) because the /ws
/// endpoint echoes client text frames, enabling order RTT measurement.
///
/// Usage (all threads on isolated, non-overlapping cores):
///   ./bench_market_pingpong --rx-cpu 0 --tx-cpu 1 --main-cpu 2 --duration 30
///   ./bench_market_pingpong --rx-cpu 0 --tx-cpu 1 --main-cpu 2 \
///       --symbols btcusdt,ethusdt --ping-interval 200 --duration 60
///   ./bench_market_pingpong --rx-cpu 0 --tx-cpu 1 --main-cpu 2 \
///       --proxy socks5://127.0.0.1:7890

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/containers/bounded_queue.hpp"
#include "eph/net/proxy.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

#include "bench_common.hpp"

using BenchTransport = eph::net::Transport<
    eph::net::SocketTransport,
    eph::net::WsFramer,
    16384, 1024,
    eph::containers::BoundedQueue,
    false  // LastOnlyDeliver=false — deliver all market data frames (FIFO)
>;

struct Config {
    std::string host       = "fstream.binance.com";
    uint16_t    port       = 443;
    std::vector<std::string> symbols = {"btcusdt", "ethusdt", "solusdt"};
    std::string proxy_url{};
    int  duration          = 30;
    int  ping_interval     = 200;   // ms between orders
    bool use_tls           = true;
    bool verify            = false;
    bool use_twophase      = false;
    int  tx_cpu            = -1;
    int  rx_cpu            = -1;
    int  main_cpu          = -1;
};

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")           c.host           = next(a);
        else if (a == "--port")           c.port           = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbols")        c.symbols        = bench::split(next(a), ',');
        else if (a == "--proxy")          c.proxy_url      = next(a);
        else if (a == "--duration")       c.duration       = std::atoi(next(a));
        else if (a == "--ping-interval")  c.ping_interval  = std::atoi(next(a));
        else if (a == "--no-tls")         c.use_tls        = false;
        else if (a == "--no-verify")      c.verify         = false;
        else if (a == "--tx-cpu")         c.tx_cpu         = std::atoi(next(a));
        else if (a == "--rx-cpu")         c.rx_cpu         = std::atoi(next(a));
        else if (a == "--main-cpu")       c.main_cpu       = std::atoi(next(a));
        else if (a == "--mode") {
            std::string_view m = next(a);
            if      (m == "all")      c.use_twophase = false;
            else if (m == "twophase") c.use_twophase = true;
            else { std::cerr << std::format("Unknown mode: {} (use all|twophase)\n", m); std::exit(1); }
        }
        else if (a == "--help") {
            std::cerr << std::format(
                "Usage: {} [--host H] [--port P] [--symbols S1,S2,S3] [--proxy URL]\n"
                "       [--duration SEC] [--ping-interval MS] [--no-tls] [--no-verify]\n"
                "       [--tx-cpu N] [--rx-cpu N] [--main-cpu N] [--mode all|twophase]\n", argv[0]);
            std::exit(0);
        }
        else { std::cerr << std::format("Unknown: {}\n", a); std::exit(1); }
    }
    return c;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, sig);
    std::signal(SIGTERM, sig);
    spdlog::set_level(spdlog::level::info);

    auto cfg = parse_args(argc, argv);
    if (cfg.main_cpu >= 0) (void)eph::utils::set_thread_affinity(cfg.main_cpu, "main");

    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed");
        return 1;
    }
    spdlog::info("TSC: {:.4f} ns/cycle", eph::utils::TSC::get_ns_per_cycle().value());

    // Build stream path using /ws/<stream1>/<stream2> format.
    // Unlike /stream?streams=, the /ws/ endpoint responds to client text frames
    // (echo), which is required for order RTT measurement.
    std::string ws_path = "/ws";
    for (auto& sym : cfg.symbols)
        ws_path += "/" + sym + "@bookTicker";

    eph::net::TransportConfig tc{
        .remote_host = cfg.host, .remote_port = cfg.port,
        .ws_path = ws_path, .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{0},  // manual order sending only
        .skip_utf8_validation = true,
        .tx_cpu = cfg.tx_cpu, .rx_cpu = cfg.rx_cpu,
        .on_state_change = [](eph::net::TransportEvent e, std::string_view d) {
            spdlog::info("[STATE] {} — {}", eph::net::transport_event_name(e), d);
        },
        .on_frame_filter = cfg.use_twophase
            ? eph::net::make_twophase_filter(bench::binance_symbol_hash)
            : eph::net::FrameFilterFn{},
    };

    eph::net::SocketConfig sc{
        .host = cfg.host, .port = cfg.port,
        .tcp_nodelay = true, .tcp_keepalive = true,
    };

    BenchTransport::TcpFactory factory;
    if (!cfg.proxy_url.empty()) {
        auto pc = eph::net::proxy::parse_proxy_url(cfg.proxy_url);
        if (!pc) { spdlog::error("Invalid proxy: {}", pc.error()); return 1; }
        factory = eph::net::proxy::make_proxied_factory(sc, *pc, cfg.host, cfg.port);
    } else {
        factory = [sc]() -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
            auto tcp = std::make_unique<eph::net::SocketTransport>(sc);
            if (auto r = tcp->connect(std::chrono::milliseconds{5000}); !r) return std::unexpected(r.error());
            return tcp;
        };
    }

    spdlog::info("Connecting to wss://{}:{}{} ({} symbols + orders)", cfg.host, cfg.port, ws_path, cfg.symbols.size());
    spdlog::info("  orders: every {}ms", cfg.ping_interval);
    auto result = BenchTransport::create(std::move(factory), tc);
    if (!result) { spdlog::error("Connect failed: {}", result.error().message()); return 1; }
    auto& tp = **result;
    spdlog::info("Connected (handshake {:.2f} ms)", tp.stats().handshake_ms());

    // ── Order simulation + per-type RX latency + feed latency ─────────────
    eph::utils::HdrHistogram order_rtt_hist{100, 1'000'000'000ULL, 3};    // 100ns–1s
    eph::utils::HdrHistogram feed_hist{1'000, 60'000'000'000ULL, 3};      // 1us–60s
    eph::utils::HdrHistogram market_rx_hist{100, 1'000'000'000ULL, 3};    // per-frame RX: market data
    eph::utils::HdrHistogram order_rx_hist{100, 1'000'000'000ULL, 3};     // per-frame RX: order response
    uint64_t order_send_tsc = 0;
    int orders_sent = 0;
    int orders_received = 0;
    int order_id_counter = 0;
    bool order_pending = false;

    // ── Main loop: recv market data + send periodic orders ─────────────────
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
        // The (data, len, arrival_tsc) overload gives us per-frame RX timing
        // so we can record separate histograms for market data vs order responses.
        bool got = tp.recv([&](const uint8_t* data, size_t len, [[maybe_unused]] uint8_t opcode, uint64_t arrival_tsc) {
            uint64_t now_tsc = eph::utils::TSC::now();

            std::string_view sv(reinterpret_cast<const char*>(data), len);
            if (sv.find("\"id\":\"ord_") != std::string_view::npos) {
                // Order response — RX latency + RTT
                if (arrival_tsc > 0) {
                    auto rx_ns = eph::utils::TSC::to_ns(now_tsc - arrival_tsc);
                    if (rx_ns) order_rx_hist.record(static_cast<uint64_t>(*rx_ns));
                }
                if (order_pending && order_send_tsc > 0) {
                    auto rtt_ns = eph::utils::TSC::to_ns(now_tsc - order_send_tsc);
                    if (rtt_ns) order_rtt_hist.record(static_cast<uint64_t>(*rtt_ns));
                    order_pending = false;
                    ++orders_received;
                }
            } else {
                // Market data — RX latency + feed latency
                ++msgs;
                if (arrival_tsc > 0) {
                    auto rx_ns = eph::utils::TSC::to_ns(now_tsc - arrival_tsc);
                    if (rx_ns) market_rx_hist.record(static_cast<uint64_t>(*rx_ns));
                }
                bench::record_feed_latency(data, len, feed_hist);
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

    spdlog::info("=== Market Data + Order Simulation Benchmark (Socket) ===");
    spdlog::info("Symbols: {} | Duration: {:.1f}s | Messages: {} | Orders: {}/{}",
                 cfg.symbols.size(), elapsed_ms / 1000.0, msgs,
                 orders_received, orders_sent);
    spdlog::info("RX totals: {} bytes, {} WS frames, {} TLS records, {} TCP pkts, {} bursts",
                 stats.rx_bytes, stats.rx_packets, rx.count,
                 stats.tcp_rx_packets, stats.tcp_rx_bursts);
    if (stats.tcp_rx_bursts > 0) {
        spdlog::info("Per rx_burst: {:.0f} bytes, {:.1f} WS frames, {:.1f} TLS records, {:.1f} TCP pkts",
                     static_cast<double>(stats.rx_bytes) / stats.tcp_rx_bursts,
                     static_cast<double>(stats.rx_packets) / stats.tcp_rx_bursts,
                     static_cast<double>(rx.count) / stats.tcp_rx_bursts,
                     static_cast<double>(stats.tcp_rx_packets) / stats.tcp_rx_bursts);
    }
    spdlog::info("Transport stats:\n{}", stats.dump());

    bench::print_latency("RX Pipeline (rx_burst → data decoded)", stats.rx_latency);
    bench::print_latency("Market RX (rx_burst → app)", bench::hdr_to_stats(market_rx_hist));
    bench::print_latency("Order RX (rx_burst → app)", bench::hdr_to_stats(order_rx_hist));
    bench::print_latency("TX Pipeline (enqueue → tx_burst)", stats.tx_latency);
    bench::print_latency("Order RTT (send → response received)", bench::hdr_to_stats(order_rtt_hist));
    bench::print_latency("Feed Latency (server send → app recv)", bench::hdr_to_stats(feed_hist));

    return 0;
}
