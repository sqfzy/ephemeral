/// @file bench_market.cpp
/// Market data pipeline latency benchmark — socket (kernel) backend.
///
/// Connects to Binance bookTicker stream and measures the single metric:
///   RX pipeline: rx_burst → market data frame decoded
///
/// No pings are sent — all rx_latency samples are pure data frames.
///
/// Usage (all threads on isolated, non-overlapping cores):
///   ./bench_market --rx-cpu 0 --tx-cpu 1 --main-cpu 2 --duration 30
///   ./bench_market --rx-cpu 0 --tx-cpu 1 --main-cpu 2 --symbol ethusdt
///   ./bench_market --rx-cpu 0 --tx-cpu 1 --main-cpu 2 --proxy socks5://127.0.0.1:7890

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/net/proxy.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

// Single-symbol: last-only deliver (only latest bookTicker matters)
using BenchTransport = eph::net::Transport<
    eph::net::SocketTransport,
    eph::net::WsFramer,
    512, 1024,
    eph::containers::EvictingQueue,
    true  // LastOnlyDeliver
>;

struct Config {
    std::string host   = "fstream.binance.com";
    uint16_t    port   = 443;
    std::string symbol = "btcusdt";
    std::string proxy_url{};
    int  duration      = 30;   // seconds, 0 = infinite
    bool use_tls       = true;
    bool verify        = false;
    int  tx_cpu        = -1;
    int  rx_cpu        = -1;
    int  main_cpu        = -1;
};

static std::atomic<bool> g_running{true};
static void sig(int) { g_running.store(false, std::memory_order_release); }

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")     c.host     = next(a);
        else if (a == "--port")     c.port     = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbol")   c.symbol   = next(a);
        else if (a == "--proxy")    c.proxy_url = next(a);
        else if (a == "--duration") c.duration = std::atoi(next(a));
        else if (a == "--no-tls")   c.use_tls  = false;
        else if (a == "--no-verify") c.verify  = false;
        else if (a == "--tx-cpu")   c.tx_cpu   = std::atoi(next(a));
        else if (a == "--rx-cpu")   c.rx_cpu   = std::atoi(next(a));
        else if (a == "--main-cpu") c.main_cpu = std::atoi(next(a));
        else if (a == "--help") {
            std::cerr << std::format(
                "Usage: {} [--host H] [--port P] [--symbol S] [--proxy URL]\n"
                "       [--duration SEC] [--no-tls] [--no-verify] [--tx-cpu N] [--rx-cpu N]\n", argv[0]);
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

    auto ws_path = std::format("/ws/{}@bookTicker", cfg.symbol);

    eph::net::TransportConfig tc{
        .remote_host = cfg.host, .remote_port = cfg.port,
        .ws_path = ws_path, .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{0},  // no pings — pure data measurement
        .skip_utf8_validation = true,
        .tx_cpu = cfg.tx_cpu, .rx_cpu = cfg.rx_cpu,
        .on_state_change = [](eph::net::TransportEvent e, std::string_view d) {
            spdlog::info("[STATE] {} — {}", eph::net::transport_event_name(e), d);
        },
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

    spdlog::info("Connecting to wss://{}:{}{}", cfg.host, cfg.port, ws_path);
    auto result = BenchTransport::create(std::move(factory), tc);
    if (!result) { spdlog::error("Connect failed: {}", result.error().message()); return 1; }
    auto& tp = **result;
    spdlog::info("Connected (handshake {:.2f} ms)", tp.stats().handshake_ms());

    // ── Main loop: drain market data ────────────────────────────────────────
    uint64_t msgs = 0;
    auto start = std::chrono::steady_clock::now();
    auto deadline = cfg.duration > 0
        ? start + std::chrono::seconds(cfg.duration)
        : std::chrono::steady_clock::time_point::max();

    while (g_running.load(std::memory_order_acquire) && tp.is_running()
           && std::chrono::steady_clock::now() < deadline) {
        bool got = tp.recv([&](const uint8_t* data, size_t len) {
            ++msgs;
            if ((msgs & 0xFF) == 1) {
                std::string_view json(reinterpret_cast<const char*>(data), len);
                spdlog::debug("[MKT #{:>6}] {:.80}", msgs, json);
            }
        });
        if (!got) eph::utils::cpu_relax();
    }

    // ── Report ──────────────────────────────────────────────────────────────
    tp.stop();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    auto stats = tp.stats();
    auto& rx = stats.rx_latency;
    double avg_bytes_per_burst = rx.count > 0
        ? static_cast<double>(stats.rx_bytes) / rx.count : 0.0;

    spdlog::info("=== Market Data Pipeline Benchmark (Socket) ===");
    spdlog::info("Duration: {:.1f}s | Messages: {}", elapsed_ms / 1000.0, msgs);
    double avg_frames = rx.count > 0
        ? static_cast<double>(stats.rx_packets) / rx.count : 0.0;
    spdlog::info("RX totals: {} bytes, {} WS frames, {} TLS records",
                 stats.rx_bytes, stats.rx_packets, rx.count);
    spdlog::info("Per TLS record: {:.0f} bytes, {:.1f} WS frames",
                 avg_bytes_per_burst, avg_frames);
    spdlog::info("Transport stats:\n{}", stats.dump());
    spdlog::info("--- RX Pipeline (rx_burst → data decoded) ---");
    if (rx.count > 0) {
        spdlog::info("  samples: {}", rx.count);
        spdlog::info("  min:     {:.0f} ns", static_cast<double>(rx.min_ns));
        spdlog::info("  p50:     {:.0f} ns", static_cast<double>(rx.p50_ns));
        spdlog::info("  p99:     {:.0f} ns", static_cast<double>(rx.p99_ns));
        spdlog::info("  p99.9:   {:.0f} ns", static_cast<double>(rx.p999_ns));
        spdlog::info("  max:     {:.0f} ns", static_cast<double>(rx.max_ns));
    } else {
        spdlog::info("  (no samples)");
    }

    return 0;
}
