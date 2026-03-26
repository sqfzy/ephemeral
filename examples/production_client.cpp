/// @file production_client.cpp
/// Production-grade WebSocket client with latency measurement, automatic
/// reconnection, and state change callbacks.
///
/// Demonstrates:
///   - TSC-based nanosecond latency measurement on the send→recv round-trip
///   - HdrHistogram percentile report (p50/p90/p99/p999) at exit
///   - TransportStateCallback for connection lifecycle events
///   - Automatic reconnection with configurable attempts
///   - Optional CPU affinity pinning for TX/RX threads via CLI
///
/// Usage:
///   ./production_client --host echo.websocket.org --count 100
///   ./production_client --host echo.websocket.org --count 1000
///       --tx-cpu 2 --rx-cpu 3 --interval 10
///
/// Prerequisites:
///   - perf_tuning_basics.cpp — understand TSC calibration and CPU pinning
///
/// Related examples:
///   - minimal_ws_client.cpp  — simpler starting point
///   - ws_echo_client.cpp     — full-featured with DPDK support

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/net/socket_transport.hpp"
#include "eph/utils/recorder.hpp"
#include "eph/utils/time.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct AppConfig {
    std::string host        = "echo.websocket.org";
    uint16_t    port        = 443;
    std::string ws_path     = "/";
    std::string message     = "hello ephemeral";
    int         count       = 10;
    int         interval_ms = 100;
    bool        use_tls     = true;
    bool        verify      = false;
    int         reconnect   = 3;
    int         tx_cpu      = -1;  // -1 = no pinning
    int         rx_cpu      = -1;
};

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

static void print_usage(const char* prog) {
    std::cerr << std::format(
        "Usage: {} [options]\n"
        "\n"
        "Options:\n"
        "  --host <hostname>    WebSocket server (default: echo.websocket.org)\n"
        "  --port <port>        Server port (default: 443)\n"
        "  --path <path>        WebSocket path (default: /)\n"
        "  --msg <message>      Message to send (default: \"hello ephemeral\")\n"
        "  --count <n>          Number of messages (default: 10)\n"
        "  --interval <ms>      Milliseconds between sends (default: 100)\n"
        "  --no-tls             Disable TLS\n"
        "  --reconnect <n>      Max reconnection attempts (default: 3)\n"
        "  --tx-cpu <id>        Pin TX thread to CPU (default: no pinning)\n"
        "  --rx-cpu <id>        Pin RX thread to CPU (default: no pinning)\n"
        "  --help               Show this help\n", prog);
}

static AppConfig parse_args(int argc, char** argv) {
    AppConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << std::format("Error: {} requires a value\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if      (arg == "--host")      cfg.host        = next("--host");
        else if (arg == "--port")      cfg.port        = static_cast<uint16_t>(std::atoi(next("--port")));
        else if (arg == "--path")      cfg.ws_path     = next("--path");
        else if (arg == "--msg")       cfg.message     = next("--msg");
        else if (arg == "--count")     cfg.count       = std::atoi(next("--count"));
        else if (arg == "--interval")  cfg.interval_ms = std::atoi(next("--interval"));
        else if (arg == "--no-tls")    cfg.use_tls     = false;
        else if (arg == "--reconnect") cfg.reconnect   = std::atoi(next("--reconnect"));
        else if (arg == "--tx-cpu")    cfg.tx_cpu      = std::atoi(next("--tx-cpu"));
        else if (arg == "--rx-cpu")    cfg.rx_cpu      = std::atoi(next("--rx-cpu"));
        else if (arg == "--help")    { print_usage(argv[0]); std::exit(0); }
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

    auto cfg = parse_args(argc, argv);

    // ── Step 1: Calibrate TSC for nanosecond-precision timing ──────────────
    spdlog::info("Calibrating TSC...");
    if (!eph::utils::TSC::init(std::chrono::milliseconds{200})) {
        spdlog::error("TSC calibration failed — invariant TSC not available?");
        return 1;
    }
    auto ns_per_cycle = eph::utils::TSC::get_ns_per_cycle().value();
    spdlog::info("TSC calibrated: {:.4f} ns/cycle", ns_per_cycle);

    // ── Step 2: Create a Recorder for round-trip latency measurement ──────
    // Records TSC cycle counts; converts to nanoseconds at report time.
    eph::utils::Recorder rtt_recorder("send_recv_rtt");

    // ── Step 3: Configure Transport with state callbacks ──────────────────
    eph::net::TransportConfig transport_cfg{
        .remote_host = cfg.host,
        .remote_port = cfg.port,
        .ws_path     = cfg.ws_path,
        .use_tls     = cfg.use_tls,
        .verify_peer = cfg.verify,
        .max_reconnect_attempts = cfg.reconnect,
        .ping_interval = std::chrono::seconds{30},
        .tx_cpu = cfg.tx_cpu,
        .rx_cpu = cfg.rx_cpu,

        // State change callback — fires on connect, disconnect, reconnect
        .on_state_change = [](eph::net::TransportEvent event, std::string_view detail) {
            spdlog::info("[STATE] {} — {}", eph::net::transport_event_name(event), detail);
        },
    };

    eph::net::SocketConfig sock_cfg{
        .host        = cfg.host,
        .port        = cfg.port,
        .tcp_nodelay = true,
        .tcp_keepalive = true,
    };

    auto tcp_factory = [&sock_cfg]()
        -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        auto tcp = std::make_unique<eph::net::SocketTransport>(sock_cfg);
        auto result = tcp->connect(std::chrono::milliseconds{5000});
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    // ── Step 4: Connect ───────────────────────────────────────────────────
    auto scheme = cfg.use_tls ? "wss" : "ws";
    spdlog::info("Connecting to {}://{}:{}{}",
                 scheme, cfg.host, cfg.port, cfg.ws_path);
    if (cfg.tx_cpu >= 0) spdlog::info("TX thread pinned to CPU {}", cfg.tx_cpu);
    if (cfg.rx_cpu >= 0) spdlog::info("RX thread pinned to CPU {}", cfg.rx_cpu);

    auto result = eph::net::Transport<eph::net::SocketTransport>::create(
        std::move(tcp_factory), transport_cfg);
    if (!result) {
        spdlog::error("Connection failed: {}", result.error().message());
        return 1;
    }
    auto& tp = **result;
    spdlog::info("Connected (handshake {:.2f} ms)", tp.stats().handshake_ms());

    // ── Step 5: Send/receive loop with latency measurement ────────────────
    int sent = 0, received = 0;
    uint64_t send_tsc = 0;  // TSC timestamp when message was sent
    auto interval = std::chrono::milliseconds(cfg.interval_ms);

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        // Send
        if (sent < cfg.count) {
            auto msg = std::format("[{}] {}", sent + 1, cfg.message);
            send_tsc = eph::utils::TSC::now();

            auto rc = tp.send_text(msg.data(), msg.size());
            if (rc == eph::net::SendError::kOk) {
                SPDLOG_DEBUG(">> {}", msg);
                ++sent;
            } else if (rc == eph::net::SendError::kQueueFull) {
                spdlog::warn("TX queue full, retrying...");
            } else {
                spdlog::error("Send failed: {}", eph::net::send_error_name(rc));
                break;
            }
        }

        // Receive and measure round-trip latency
        auto deadline = std::chrono::steady_clock::now() + interval;
        while (std::chrono::steady_clock::now() < deadline &&
               g_running.load(std::memory_order_acquire)) {
            bool got = tp.recv([&](const uint8_t* data, size_t len) {
                // Measure round-trip time in TSC cycles
                uint64_t recv_tsc = eph::utils::TSC::now();
                uint64_t rtt_cycles = recv_tsc - send_tsc;
                rtt_recorder.record(rtt_cycles);

                [[maybe_unused]] auto rtt_ns = eph::utils::TSC::to_ns(rtt_cycles).value_or(0.0);
                std::string_view echo(reinterpret_cast<const char*>(data), len);
                SPDLOG_DEBUG("<< {} (RTT: {:.0f} ns)", echo, rtt_ns);
                ++received;
            });
            if (!got) {
                std::this_thread::sleep_for(std::chrono::microseconds{50});
            }
        }

        if (sent >= cfg.count && received >= cfg.count) {
            spdlog::info("All {} messages sent and echoed.", cfg.count);
            break;
        }
    }

    // ── Step 6: Print results ─────────────────────────────────────────────
    tp.stop();

    // Transport-level statistics
    auto stats = tp.stats();
    spdlog::info("Transport stats:\n{}", stats.dump());

    // Latency histogram report (p50/p90/p99/p999)
    if (rtt_recorder.has_data()) {
        spdlog::info("Round-trip latency report:");
        rtt_recorder.print_report();
    } else {
        spdlog::warn("No RTT samples recorded.");
    }

    return 0;
}
