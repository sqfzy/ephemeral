/// @file bench_market_multi.cpp
/// Multi-symbol market data pipeline latency benchmark — socket (kernel) backend.
///
/// Connects to Binance combined bookTicker stream and measures:
///   RX pipeline: rx_burst → market data frame decoded
///
/// Combined stream delivers all symbols in one connection.
/// LastOnlyDeliver=false ensures every message reaches the app.
///
/// Usage:
///   ./bench_market_multi
///   ./bench_market_multi --symbols btcusdt,ethusdt,solusdt --duration 60
///   ./bench_market_multi --proxy socks5://127.0.0.1:7890

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/net/proxy.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

// Multi-symbol: larger payload for combined stream wrapper, deliver all frames.
using BenchTransport = eph::net::Transport<
    eph::net::SocketTransport,
    eph::net::WsFramer,
    4096, 1024,
    eph::containers::EvictingQueue,
    false  // LastOnlyDeliver — every symbol's update matters
>;

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
    int  main_cpu        = -1;
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
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")      c.host      = next(a);
        else if (a == "--port")      c.port      = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbols")   c.symbols   = split(next(a), ',');
        else if (a == "--proxy")     c.proxy_url = next(a);
        else if (a == "--duration")  c.duration  = std::atoi(next(a));
        else if (a == "--no-tls")    c.use_tls   = false;
        else if (a == "--on-message") c.use_on_message = true;
        else if (a == "--no-verify") c.verify    = false;
        else if (a == "--tx-cpu")    c.tx_cpu    = std::atoi(next(a));
        else if (a == "--rx-cpu")    c.rx_cpu    = std::atoi(next(a));
        else if (a == "--main-cpu") c.main_cpu = std::atoi(next(a));
        else if (a == "--help") {
            std::cerr << std::format(
                "Usage: {} [--host H] [--port P] [--symbols S1,S2,S3] [--proxy URL]\n"
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
    if (cfg.main_cpu >= 0) eph::utils::set_thread_affinity(cfg.main_cpu, "main");

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
    };

    static volatile uint64_t on_msg_sink = 0;
    if (cfg.use_on_message) {
        tc.on_message = [](const uint8_t* data, uint16_t len, uint8_t) {
            on_msg_sink = *reinterpret_cast<const uint64_t*>(data);
        };
    }

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

    spdlog::info("Connecting to wss://{}:{}{} ({} symbols)",
                 cfg.host, cfg.port, ws_path, cfg.symbols.size());
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
        if (cfg.use_on_message) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        } else {
            bool got = tp.recv([&](const uint8_t* data, size_t len) {
                ++msgs;
                if ((msgs & 0xFF) == 1) {
                    std::string_view json(reinterpret_cast<const char*>(data), len);
                    spdlog::debug("[MKT #{:>6}] {:.80}", msgs, json);
                }
            });
            if (!got) eph::utils::cpu_relax();
        }
    }

    // ── Report ──────────────────────────────────────────────────────────────
    tp.stop();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    auto stats = tp.stats();
    spdlog::info("=== Multi-Symbol Market Data Benchmark (Socket) ===");
    spdlog::info("Symbols: {} | Duration: {:.1f}s | Messages: {}",
                 cfg.symbols.size(), elapsed_ms / 1000.0, msgs);
    spdlog::info("Transport stats:\n{}", stats.dump());

    auto& rx = stats.rx_latency;
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
