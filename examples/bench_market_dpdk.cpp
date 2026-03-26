/// @file bench_market_dpdk.cpp
/// Market data pipeline latency benchmark — DPDK (kernel-bypass) backend.
///
/// Connects to Binance bookTicker stream and measures the single metric:
///   RX pipeline: rx_burst → market data frame decoded
///
/// No pings are sent — all rx_latency samples are pure data frames.
///
/// Usage:
///   sudo ./bench_market_dpdk -l 0-3 -a 0000:xx:00.0 -- --local-ip 10.0.0.2 --gateway-ip 10.0.0.1
///   sudo ./bench_market_dpdk -l 0-3 -- --local-ip 10.0.0.2 --gateway-ip 10.0.0.1 --duration 60

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/utils/time.hpp"
#include "eph/utils/cpu.hpp"

// Single-symbol: last-only deliver (only latest bookTicker matters)
using BenchTransport = eph::net::Transport<
    eph::dpdk::TcpSession<>,
    eph::net::WsFramer,
    512, 1024,
    eph::containers::EvictingQueue,
    true  // LastOnlyDeliver
>;

struct Config {
    std::string host       = "fstream.binance.com";
    uint16_t    port       = 443;
    std::string symbol     = "btcusdt";
    std::string local_ip{};
    std::string gateway_ip{};
    uint16_t    dpdk_port  = 0;
    uint16_t    local_port = 32768;
    int  duration          = 30;
    bool use_tls           = true;
    bool verify            = false;
    int  tx_cpu            = -1;
    int  rx_cpu            = -1;
    int  main_cpu        = -1;
};

static std::atomic<bool> g_running{true};
static void sig(int) { g_running.store(false, std::memory_order_release); }

static Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        auto next = [&](std::string_view n) -> const char* {
            if (i + 1 >= argc) { std::cerr << std::format("{} requires a value\n", n); std::exit(1); }
            return argv[++i];
        };
        if      (a == "--host")       c.host       = next(a);
        else if (a == "--port")       c.port       = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--symbol")     c.symbol     = next(a);
        else if (a == "--local-ip")   c.local_ip   = next(a);
        else if (a == "--gateway-ip") c.gateway_ip = next(a);
        else if (a == "--dpdk-port")  c.dpdk_port  = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--local-port") c.local_port = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--duration")   c.duration   = std::atoi(next(a));
        else if (a == "--tx-cpu")     c.tx_cpu     = std::atoi(next(a));
        else if (a == "--rx-cpu")     c.rx_cpu     = std::atoi(next(a));
        else if (a == "--main-cpu") c.main_cpu = std::atoi(next(a));
        else if (a == "--no-tls")     c.use_tls    = false;
        else if (a == "--help") {
            std::cerr << std::format(
                "Usage: {} [EAL args] -- [--host H] [--port P] [--symbol S]\n"
                "       [--local-ip IP] [--gateway-ip IP] [--dpdk-port N] [--local-port N]\n"
                "       [--duration SEC] [--tx-cpu N] [--rx-cpu N] [--no-tls]\n", "bench_market_dpdk");
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

    // Split EAL args from app args at '--'
    int eal_argc = argc; char** eal_argv = argv;
    int app_argc = 0;    char** app_argv = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            eal_argc = i; app_argc = argc - i - 1; app_argv = argv + i + 1; break;
        }
    }
    auto cfg = parse_args(app_argc, app_argv);
    if (cfg.main_cpu >= 0) eph::utils::set_thread_affinity(cfg.main_cpu, "main");
    if (cfg.local_ip.empty() || cfg.gateway_ip.empty()) {
        spdlog::error("--local-ip and --gateway-ip are required"); return 1;
    }

    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed"); return 1;
    }
    spdlog::info("TSC: {:.4f} ns/cycle", eph::utils::TSC::get_ns_per_cycle().value());

    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(eal_argc, eal_argv);
    if (!eal) { spdlog::error("EAL init failed: {}", eal.error()); return 1; }

    auto ws_path = std::format("/ws/{}@bookTicker", cfg.symbol);

    eph::net::TransportConfig tc{
        .remote_host = cfg.host, .remote_port = cfg.port,
        .ws_path = ws_path, .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{0},  // no pings
        .skip_utf8_validation = true,
        .tx_cpu = cfg.tx_cpu, .rx_cpu = cfg.rx_cpu,
        .on_state_change = [](eph::net::TransportEvent e, std::string_view d) {
            spdlog::info("[STATE] {} — {}", eph::net::transport_event_name(e), d);
        },
    };

    spdlog::info("Connecting via DPDK to wss://{}:{}{}", cfg.host, cfg.port, ws_path);
    auto conn = eph::dpdk::connect<BenchTransport>(
        eph::dpdk::DpdkEndpoint{.local_ip = cfg.local_ip, .gateway_ip = cfg.gateway_ip},
        tc, eph::dpdk::ConnectorOptions{.platform = {.port_id = cfg.dpdk_port}, .local_port = cfg.local_port});
    if (!conn) { spdlog::error("DPDK connect failed: {}", conn.error()); return 1; }
    auto& tp = *conn->transport;
    spdlog::info("Connected via DPDK!");

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
    spdlog::info("=== Market Data Pipeline Benchmark (DPDK) ===");
    spdlog::info("Duration: {:.1f}s | Messages: {}", elapsed_ms / 1000.0, msgs);
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
