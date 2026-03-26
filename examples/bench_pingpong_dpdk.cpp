/// @file bench_pingpong_dpdk.cpp
/// Ping/pong latency benchmark — DPDK (kernel-bypass) backend.
///
/// Connects to Binance WITHOUT subscribing to any stream, then sends
/// periodic WebSocket pings. Measures three metrics:
///   1) TX Queue:  ping enqueue → tx_burst
///   2) RX Pong:   rx_burst → pong frame decoded
///   3) RTT:       ping tx_burst → pong rx_burst
///
/// No market data is subscribed — all rx_latency samples are pure pong frames.
///
/// Usage:
///   sudo ./bench_pingpong_dpdk -l 0-3 -- --local-ip 10.0.0.2 --gateway-ip 10.0.0.1
///   sudo ./bench_pingpong_dpdk -l 0-3 -- --local-ip 10.0.0.2 --gateway-ip 10.0.0.1 --count 500

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/utils/time.hpp"

using BenchTransport = eph::net::Transport<
    eph::dpdk::TcpSession<>,
    eph::net::WsFramer,
    512, 1024,
    eph::containers::EvictingQueue,
    true  // LastOnlyDeliver
>;

struct Config {
    std::string host          = "fstream.binance.com";
    uint16_t    port          = 443;
    std::string local_ip{};
    std::string gateway_ip{};
    uint16_t    dpdk_port     = 0;
    uint16_t    local_port    = 32768;
    int  count                = 200;
    int  ping_interval        = 500;   // ms
    int  payload_size         = 0;     // ping payload bytes (0-125, simulates order data)
    bool use_tls              = true;
    bool verify               = false;
    int  tx_cpu               = -1;
    int  rx_cpu               = -1;
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
        if      (a == "--host")          c.host          = next(a);
        else if (a == "--port")          c.port          = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--local-ip")      c.local_ip      = next(a);
        else if (a == "--gateway-ip")    c.gateway_ip    = next(a);
        else if (a == "--dpdk-port")     c.dpdk_port     = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--local-port")    c.local_port    = static_cast<uint16_t>(std::atoi(next(a)));
        else if (a == "--count")         c.count         = std::atoi(next(a));
        else if (a == "--ping-interval") c.ping_interval = std::atoi(next(a));
        else if (a == "--payload-size")  c.payload_size  = std::clamp(std::atoi(next(a)), 0, 125);
        else if (a == "--tx-cpu")        c.tx_cpu        = std::atoi(next(a));
        else if (a == "--rx-cpu")        c.rx_cpu        = std::atoi(next(a));
        else if (a == "--no-tls")        c.use_tls       = false;
        else if (a == "--help") {
            std::cerr << std::format(
                "Usage: {} [EAL args] -- [--host H] [--port P]\n"
                "       [--local-ip IP] [--gateway-ip IP] [--dpdk-port N] [--local-port N]\n"
                "       [--count N] [--ping-interval MS] [--payload-size BYTES]\n"
                "       [--tx-cpu N] [--rx-cpu N] [--no-tls]\n",
                "bench_pingpong_dpdk");
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

    // Bare WS endpoint — no stream subscription, pings only
    eph::net::TransportConfig tc{
        .remote_host = cfg.host, .remote_port = cfg.port,
        .ws_path = "/ws",  // no subscription
        .use_tls = cfg.use_tls, .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{0},  // manual pings only
        .skip_utf8_validation = true,
        .tx_cpu = cfg.tx_cpu, .rx_cpu = cfg.rx_cpu,
        .on_state_change = [](eph::net::TransportEvent e, std::string_view d) {
            spdlog::info("[STATE] {} — {}", eph::net::transport_event_name(e), d);
        },
        .on_pong = [](const uint8_t*, uint16_t) {
            SPDLOG_DEBUG("Pong received");
        },
    };

    spdlog::info("Connecting via DPDK to wss://{}:{}/ws (ping-only)", cfg.host, cfg.port);
    auto conn = eph::dpdk::connect<BenchTransport>(
        eph::dpdk::DpdkEndpoint{.local_ip = cfg.local_ip, .gateway_ip = cfg.gateway_ip},
        tc, eph::dpdk::ConnectorOptions{.platform = {.port_id = cfg.dpdk_port}, .local_port = cfg.local_port});
    if (!conn) { spdlog::error("DPDK connect failed: {}", conn.error()); return 1; }
    auto& tp = *conn->transport;
    spdlog::info("Connected via DPDK!");

    // ── Prepare ping payload (simulates order-sized data) ──────────────────
    std::array<uint8_t, 125> ping_payload{};
    if (cfg.payload_size > 0) {
        for (int i = 0; i < cfg.payload_size; ++i)
            ping_payload[static_cast<size_t>(i)] = static_cast<uint8_t>('A' + (i % 26));
        spdlog::info("Ping payload: {} bytes", cfg.payload_size);
    }

    // ── Main loop: send pings ───────────────────────────────────────────────
    int pings_sent = 0;
    auto start = std::chrono::steady_clock::now();
    auto last_ping = start;
    auto interval = std::chrono::milliseconds(cfg.ping_interval);

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_ping >= interval) {
            if (cfg.count == 0 || pings_sent < cfg.count) {
                auto rc = cfg.payload_size > 0
                    ? tp.send_ping(ping_payload.data(), static_cast<size_t>(cfg.payload_size))
                    : tp.send_ping();
                if (rc == eph::net::SendError::kOk) {
                    ++pings_sent;
                    SPDLOG_DEBUG("Ping #{} sent", pings_sent);
                } else {
                    spdlog::warn("Ping failed: {}", eph::net::send_error_name(rc));
                }
                last_ping = now;
            } else {
                spdlog::info("All {} pings sent, waiting for final pongs...", cfg.count);
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
                break;
            }
        }

        tp.recv([](const uint8_t*, size_t) {});
        eph::utils::cpu_relax();
    }

    // ── Report ──────────────────────────────────────────────────────────────
    tp.stop();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    auto stats = tp.stats();
    spdlog::info("=== Ping/Pong Benchmark (DPDK) ===");
    spdlog::info("Duration: {:.1f}s | Pings sent: {}", elapsed_ms / 1000.0, pings_sent);
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

    print_latency("Metric 1: TX Queue (ping enqueue → flush)", stats.tx_latency);
    print_latency("Metric 2: RX Pong Pipeline (rx_burst → pong decoded)", stats.rx_latency);
    print_latency("Metric 3: RTT (ping flush → pong arrive)", stats.rtt);

    return 0;
}
