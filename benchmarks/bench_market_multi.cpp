/// @file bench_market_multi.cpp
/// Multi-symbol market data pipeline latency benchmark — socket (kernel) backend.
///
/// Connects to Binance combined bookTicker stream and measures:
///   RX pipeline: rx_burst → market data frame decoded
///
/// Combined stream delivers all symbols in one connection.
/// LastOnlyDeliver=false ensures every message reaches the app.
///
/// Usage (all threads on isolated, non-overlapping cores):
///   # Queue mode (default):
///   ./bench_market_multi --rx-cpu 0 --tx-cpu 1 --main-cpu 2 --duration 30
///
///   # on_message mode (bypass queue):
///   ./bench_market_multi --rx-cpu 0 --tx-cpu 1 --main-cpu 2 --on-message --duration 30
///
///   # Custom symbols:
///   ./bench_market_multi --rx-cpu 0 --tx-cpu 1 --main-cpu 2
///       --symbols btcusdt,ethusdt,solusdt,bnbusdt --duration 60
///
///   # With proxy:
///   ./bench_market_multi --rx-cpu 0 --tx-cpu 1 --main-cpu 2
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
#include "eph/transport/transport.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

#include "bench_common.hpp"

// Multi-symbol: larger payload for combined stream wrapper, deliver all frames.
using BenchTransport = eph::net::Transport<
    eph::net::SocketTransport,
    eph::net::WsFramer,
    16384, 1024,
    eph::containers::BoundedQueue,
    false  // LastOnlyDeliver — every symbol's update matters
>;

/// Extract symbol hash from Binance combined stream JSON payload.
/// Format: {"stream":"<symbol>@<channel>","data":{...}}
/// Scans the first ~64 bytes for "stream":" prefix, hashes until '@'.
/// Returns 0 on parse failure (payload delivered unconditionally).
static uint32_t binance_symbol_hash_multi(const uint8_t* data, size_t len) {
    constexpr size_t kPrefixLen = 11;  // {"stream":"
    if (len < kPrefixLen + 2) return 0;

    if (data[0] != '{' || data[1] != '"' || data[9] != '"') return 0;

    const uint8_t* p = data + kPrefixLen;
    const uint8_t* end = data + std::min(len, size_t{64});
    uint32_t hash = 2166136261u;  // FNV-1a offset basis
    while (p < end && *p != '@' && *p != '"') {
        hash ^= *p;
        hash *= 16777619u;  // FNV-1a prime
        ++p;
    }
    return (p > data + kPrefixLen) ? hash : 0;
}

struct Config {
    std::string host   = "fstream.binance.com";
    uint16_t    port   = 443;
    std::vector<std::string> symbols = {"btcusdt", "ethusdt", "solusdt"};
    std::string proxy_url{};
    int  duration      = 30;
    bool use_tls       = true;
    bool verify        = false;
    bool use_on_message = false;
    int  tx_cpu        = -1;
    int  rx_cpu        = -1;
    int  main_cpu      = -1;
    bool use_twophase   = false;
};

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")      c.host      = next(a);
        else if (a == "--port")      c.port      = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbols")   c.symbols   = bench::split(next(a), ',');
        else if (a == "--proxy")     c.proxy_url = next(a);
        else if (a == "--duration")  c.duration  = std::atoi(next(a));
        else if (a == "--no-tls")    c.use_tls   = false;
        else if (a == "--on-message") c.use_on_message = true;
        else if (a == "--no-verify") c.verify    = false;
        else if (a == "--tx-cpu")    c.tx_cpu    = std::atoi(next(a));
        else if (a == "--rx-cpu")    c.rx_cpu    = std::atoi(next(a));
        else if (a == "--main-cpu") c.main_cpu = std::atoi(next(a));
        else if (a == "--mode") {
            std::string_view m = next(a);
            if      (m == "all")      c.use_twophase = false;
            else if (m == "twophase") c.use_twophase = true;
            else { std::cerr << std::format("Unknown mode: {} (use all|twophase)\n", m); std::exit(1); }
        }
        else if (a == "--help") {
            std::cerr << std::format(
                "Usage: {} [--host H] [--port P] [--symbols S1,S2,S3] [--proxy URL]\n"
                "       [--duration SEC] [--no-tls] [--no-verify] [--tx-cpu N] [--rx-cpu N]\n"
                "       [--mode all|twophase]\n", argv[0]);
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

    // Build combined stream path
    std::string streams;
    for (size_t i = 0; i < cfg.symbols.size(); ++i) {
        if (i > 0) streams += '/';
        streams += cfg.symbols[i] + "@bookTicker";
    }
    auto ws_path = "/stream?streams=" + streams;

    eph::net::TransportConfig tc{
        .remote_host = cfg.host, .remote_port = cfg.port,
        .ws_path = ws_path, .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{0},
        .skip_utf8_validation = true,
        .tx_cpu = cfg.tx_cpu, .rx_cpu = cfg.rx_cpu,
        .on_state_change = [](eph::net::TransportEvent e, std::string_view d) {
            spdlog::info("[STATE] {} — {}", eph::net::transport_event_name(e), d);
        },
        .on_frame_filter = cfg.use_twophase
            ? eph::net::make_twophase_filter(binance_symbol_hash_multi)
            : eph::net::FrameFilterFn{},
    };

    static volatile uint64_t on_msg_sink = 0;
    if (cfg.use_on_message) {
        tc.on_message = [](const uint8_t* data, uint16_t len, uint8_t) {
            if (len >= 8) [[likely]]
                on_msg_sink = *reinterpret_cast<const uint64_t*>(data);
        };
    }
    (void)on_msg_sink;

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

    const char* mode_name = cfg.use_twophase ? "twophase" : "all";

    spdlog::info("Connecting to wss://{}:{}{} ({} symbols, mode={})",
                 cfg.host, cfg.port, ws_path, cfg.symbols.size(), mode_name);
    auto result = BenchTransport::create(std::move(factory), tc);
    if (!result) { spdlog::error("Connect failed: {}", result.error().message()); return 1; }
    auto& tp = **result;
    spdlog::info("Connected (handshake {:.2f} ms)", tp.stats().handshake_ms());

    // ── Feed latency tracking ─────────────────────────────────────────────
    // Range: 1us to 60s (in nanoseconds) — covers all practical feed latencies.
    eph::utils::HdrHistogram feed_hist{1'000, 60'000'000'000ULL, 3};

    // ── Main loop: drain market data ────────────────────────────────────────
    uint64_t msgs = 0;
    auto start = std::chrono::steady_clock::now();
    auto deadline = cfg.duration > 0
        ? start + std::chrono::seconds(cfg.duration)
        : std::chrono::steady_clock::time_point::max();

    // Per-second status line
    auto prev_hist = tp.rx_latency_histogram_snapshot();
    uint64_t prev_msgs = 0;
    auto window_start = start;
    int window_idx = 0;

    spdlog::info("{:>8} {:>10} {:>10} {:>10} {:>12}",
                 "time", "msg/s", "p50(ns)", "p99(ns)", "p99.9(ns)");

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        if (cfg.use_on_message) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        } else {
            bool got = tp.recv([&](const uint8_t* data, size_t len) {
                ++msgs;
                // Feed latency: server send timestamp vs local wall clock
                bench::record_feed_latency(data, len, feed_hist);
                if ((msgs & 0xFF) == 1) {
                    std::string_view json(reinterpret_cast<const char*>(data), len);
                    spdlog::debug("[MKT #{:>6}] {:.80}", msgs, json);
                }
            });
            if (!got) eph::utils::cpu_relax();
        }

        // Per-second window snapshot
        if (std::chrono::steady_clock::now() - window_start >= std::chrono::seconds(1)) {
            auto snap_time = std::chrono::steady_clock::now();
            ++window_idx;
            auto curr_hist = tp.rx_latency_histogram_snapshot();
            auto delta = curr_hist;
            (void)delta.subtract(prev_hist);
            auto ws = bench::hdr_to_stats(delta);

            uint64_t delta_msgs = msgs - prev_msgs;
            double elapsed_s = std::chrono::duration<double>(snap_time - window_start).count();
            double rate = delta_msgs / elapsed_s;

            spdlog::info("[T+{:>3}s] {:>8.0f} {:>10} {:>10} {:>12}",
                         window_idx, rate, ws.p50_ns, ws.p99_ns, ws.p999_ns);

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

    spdlog::info("=== Multi-Symbol Market Data Benchmark (Socket) ===");
    spdlog::info("Symbols: {} | Duration: {:.1f}s | Messages: {} | Mode: {}",
                 cfg.symbols.size(), elapsed_ms / 1000.0, msgs, mode_name);
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
    bench::print_latency("Feed Latency (server send → app recv)", bench::hdr_to_stats(feed_hist));

    return 0;
}
