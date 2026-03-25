/// @file simple_hft_dpdk.cpp
/// Minimal HFT-style example: Binance market data + WebSocket ping/pong
/// latency measurement using the DPDK (kernel-bypass) backend.
///
/// Demonstrates:
///   - Transport with custom Probe for pipeline latency measurement
///   - EvictingQueue as RxQueue (market data is droppable)
///   - Manual WebSocket ping for simulated order round-trip
///   - Four latency metrics (same as simple_hft.cpp)
///   - DPDK kernel-bypass networking
///
/// Usage:
///   sudo ./simple_hft_dpdk -a 0000:28:00.0 -- \
///       --local-ip 172.31.23.112 --gateway-ip 172.31.16.1
///
///   sudo ./simple_hft_dpdk -a 0000:28:00.0 -- \
///       --local-ip 172.31.23.112 --gateway-ip 172.31.16.1 \
///       --symbol ethusdt --count 500 --ping-interval 200
///
/// EAL args (e.g. -a <pci_addr>) go BEFORE the '--' separator;
/// application args go AFTER it.
///
/// For the socket backend, see simple_hft.cpp.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include <rte_bus.h>

#include "eph/containers/evicting_queue.hpp"
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/utils/time.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Transport type alias: DPDK backend + timestamps + EvictingQueue
// ─────────────────────────────────────────────────────────────────────────────

using HftDpdkTransport = eph::net::Transport<
    eph::dpdk::TcpSession<>,
    eph::net::WsFramer,
    512,   // MaxPayload
    1024,  // QueueDepth
    true,  // EnableTimestamps
    eph::containers::EvictingQueue
>;

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct AppConfig {
    std::string host          = "fstream.binance.com";
    uint16_t    port          = 443;
    std::string symbol        = "btcusdt";
    int         count         = 100;
    int         ping_interval = 1000;    // ms
    bool        use_tls       = true;
    bool        verify        = false;
    int         tx_cpu        = -1;
    int         rx_cpu        = -1;

    // DPDK-specific
    std::string local_ip{};
    std::string gateway_ip{};
    uint16_t    dpdk_port    = 0;
    uint16_t    local_port   = 32768;
};

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

static void print_usage(const char* prog) {
    std::cerr << std::format(
        "Usage: {} [EAL args] -- [app args]\n"
        "\n"
        "App options:\n"
        "  --host <hostname>       Binance stream host (default: fstream.binance.com)\n"
        "  --port <port>           Server port (default: 443)\n"
        "  --symbol <symbol>       Trading pair (default: btcusdt)\n"
        "  --count <n>             Number of pings, 0=infinite (default: 100)\n"
        "  --ping-interval <ms>    Milliseconds between pings (default: 1000)\n"
        "  --local-ip <ip>         Local IP address (required)\n"
        "  --gateway-ip <ip>       Gateway IP address (required)\n"
        "  --dpdk-port <id>        DPDK port ID (default: 0)\n"
        "  --local-port <port>     Local TCP source port (default: 32768)\n"
        "  --tx-cpu <id>           Pin TX thread to CPU\n"
        "  --rx-cpu <id>           Pin RX thread to CPU\n"
        "  --no-tls                Disable TLS\n"
        "  --help                  Show this help\n", prog);
}

static AppConfig parse_args(int argc, char** argv) {
    AppConfig cfg;
    // app_argv starts at the first app arg (after '--'), not the program name,
    // so loop from i=0 instead of i=1.
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << std::format("Error: {} requires a value\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if      (arg == "--host")          cfg.host          = next("--host");
        else if (arg == "--port")          cfg.port          = static_cast<uint16_t>(std::atoi(next("--port")));
        else if (arg == "--symbol")        cfg.symbol        = next("--symbol");
        else if (arg == "--count")         cfg.count         = std::atoi(next("--count"));
        else if (arg == "--ping-interval") cfg.ping_interval = std::atoi(next("--ping-interval"));
        else if (arg == "--local-ip")      cfg.local_ip      = next("--local-ip");
        else if (arg == "--gateway-ip")    cfg.gateway_ip    = next("--gateway-ip");
        else if (arg == "--dpdk-port")     cfg.dpdk_port     = static_cast<uint16_t>(std::atoi(next("--dpdk-port")));
        else if (arg == "--local-port")    cfg.local_port    = static_cast<uint16_t>(std::atoi(next("--local-port")));
        else if (arg == "--tx-cpu")        cfg.tx_cpu        = std::atoi(next("--tx-cpu"));
        else if (arg == "--rx-cpu")        cfg.rx_cpu        = std::atoi(next("--rx-cpu"));
        else if (arg == "--no-tls")        cfg.use_tls       = false;
        else if (arg == "--proxy") {
            (void)next("--proxy");  // consume the value
            std::cerr << "Error: --proxy is not supported with DPDK backend "
                         "(kernel-bypass has no OS network stack for proxying).\n"
                         "Use the socket backend (simple_hft) for proxy support.\n";
            std::exit(1);
        }
        else if (arg == "--help")        { print_usage(argv[0]); std::exit(0); }
        else {
            std::cerr << std::format("Unknown argument: {}\n", arg);
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    spdlog::set_level(spdlog::level::info);

    // ── Split EAL args from app args at '--' ──────────────────────────────
    int eal_argc = argc;
    char** eal_argv = argv;
    int app_argc = 0;
    char** app_argv = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            eal_argc = i;
            app_argc = argc - i - 1;
            app_argv = argv + i + 1;
            break;
        }
    }

    auto cfg = parse_args(app_argc, app_argv);

    if (cfg.local_ip.empty() || cfg.gateway_ip.empty()) {
        spdlog::error("--local-ip and --gateway-ip are required for DPDK backend");
        return 1;
    }

    // ── Step 1: Calibrate TSC ─────────────────────────────────────────────
    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed");
        return 1;
    }
    spdlog::info("TSC calibrated: {:.4f} ns/cycle",
                 eph::utils::TSC::get_ns_per_cycle().value());

    // ── Step 2: Initialize DPDK EAL ──────────────────────────────────────
    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(eal_argc, eal_argv);
    if (!eal) {
        spdlog::error("EAL init failed: {}", eal.error());
        return 1;
    }
    spdlog::info("EAL initialized ({} args consumed)", eal->args_consumed());

    // ── Step 3: Build WebSocket path and connect ──────────────────────────
    auto ws_path = std::format("/ws/{}@bookTicker", cfg.symbol);
    spdlog::info("Subscribing to {}", ws_path);

    eph::net::TransportConfig transport_cfg{
        .remote_host = cfg.host,
        .remote_port = cfg.port,
        .ws_path     = ws_path,
        .use_tls     = cfg.use_tls,
        .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{0},  // manual pings
        .skip_utf8_validation = true,
        .tx_cpu = cfg.tx_cpu,
        .rx_cpu = cfg.rx_cpu,

        .on_state_change = [](eph::net::TransportEvent event,
                              std::string_view detail) {
            spdlog::info("[STATE] {} — {}",
                         eph::net::transport_event_name(event), detail);
        },

        .on_pong = [](const uint8_t*, uint16_t) {
            SPDLOG_DEBUG("Pong received");
        },
    };

    spdlog::info("Connecting via DPDK to wss://{}:{}{}", cfg.host, cfg.port, ws_path);
    spdlog::info("Local={}:{}, gateway={}", cfg.local_ip, cfg.local_port, cfg.gateway_ip);

    eph::dpdk::DpdkEndpoint endpoint{
        .local_ip   = cfg.local_ip,
        .gateway_ip = cfg.gateway_ip,
    };

    eph::dpdk::ConnectorOptions opts{
        .platform   = {.port_id = cfg.dpdk_port},
        .local_port = cfg.local_port,
    };

    auto conn = eph::dpdk::connect<HftDpdkTransport>(
        endpoint, transport_cfg, opts);
    if (!conn) {
        spdlog::error("DPDK connect failed: {}", conn.error());
        return 1;
    }
    auto& tp = *conn->transport;
    spdlog::info("Connected via DPDK!");

    // ── Step 4: Main loop — receive market data + periodic pings ──────────
    int pings_sent = 0;
    uint64_t market_msgs = 0;
    auto last_ping = std::chrono::steady_clock::now();
    auto ping_interval = std::chrono::milliseconds(cfg.ping_interval);
    auto start_time = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_ping >= ping_interval) {
            if (cfg.count == 0 || pings_sent < cfg.count) {
                auto rc = tp.send_ping();
                if (rc == eph::net::SendError::kOk) {
                    ++pings_sent;
                    SPDLOG_DEBUG("Ping #{} sent", pings_sent);
                } else {
                    spdlog::warn("Ping send failed: {}",
                                 eph::net::send_error_name(rc));
                }
                last_ping = now;
            } else if (pings_sent >= cfg.count) {
                spdlog::info("All {} pings sent, collecting remaining data...",
                             cfg.count);
                std::this_thread::sleep_for(std::chrono::milliseconds{500});
                break;
            }
        }

        // Drain market data (EvictingQueue: latest-value semantics).
        // RX pipeline latency measured internally by Transport.
        bool got = tp.recv([&](const uint8_t* data, size_t len) {
            ++market_msgs;
            if ((market_msgs & 0xFF) == 1) {
                std::string_view json(reinterpret_cast<const char*>(data), len);
                spdlog::info("[MKT #{:>6}] {:.80}", market_msgs, json);
            }
        });

        if (!got) {
            eph::utils::cpu_relax();
        }
    }

    // ── Step 5: Report ────────────────────────────────────────────────────
    tp.stop();
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    spdlog::info("=== Session Summary ===");
    spdlog::info("Duration: {:.1f}s", static_cast<double>(elapsed_ms) / 1000.0);
    spdlog::info("Market data messages: {}", market_msgs);
    spdlog::info("Pings sent: {}", pings_sent);

    auto stats = tp.stats();
    spdlog::info("Transport stats:\n{}", stats.dump());

    // Pipeline latency reports (all from TransportStats)
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

    print_latency("Metric 1: TX Queue (enqueue → flush)", stats.tx_latency);
    print_latency("Metric 2: RX Pipeline (rx_burst → deliver)", stats.rx_latency);
    print_latency("Metric 3: Ping/Pong RTT (end-to-end)", stats.rtt);

    // EalGuard RAII handles eal_cleanup()
    return 0;
}
