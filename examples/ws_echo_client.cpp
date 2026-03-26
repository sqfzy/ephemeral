/// @file ws_echo_client.cpp
/// WebSocket echo client using the socket (kernel) backend.
///
/// Usage:
///   ./ws_echo_client --host echo.websocket.org
///   ./ws_echo_client --host echo.websocket.org --port 443 --path /ws
///       --msg "hello" --count 5 --interval 1000 --no-tls
///
/// For the DPDK backend, see ws_echo_client_dpdk.cpp.

#include <atomic>
#include <charconv>
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

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct AppConfig {
    std::string host        = "echo.websocket.org";
    uint16_t    port        = 443;
    std::string ws_path     = "/";
    std::string message     = "hello ephemeral";
    int         count       = 3;       // 0 = infinite
    int         interval_ms = 1000;
    bool        use_tls     = true;
    bool        verify      = true;
    std::string ca_cert{};
};

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument parsing
// ─────────────────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cerr << std::format(
        "Usage: {} [options]\n"
        "\n"
        "Options:\n"
        "  --host <hostname>        WebSocket server hostname (default: echo.websocket.org)\n"
        "  --port <port>            Server port (default: 443)\n"
        "  --path <path>            WebSocket upgrade path (default: /)\n"
        "  --msg <message>          Message to send (default: \"hello ephemeral\")\n"
        "  --count <n>              Number of messages, 0=infinite (default: 3)\n"
        "  --interval <ms>          Milliseconds between sends (default: 1000)\n"
        "  --no-tls                 Use plain WebSocket (ws://) instead of WSS\n"
        "  --no-verify              Disable TLS certificate verification\n"
        "  --ca-cert <path>         CA certificate file\n"
        "  --help                   Show this help\n",
        prog);
}

static int parse_int(std::string_view sv, std::string_view flag) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) {
        std::cerr << std::format("Error: {} requires a valid integer, got '{}'\n", flag, sv);
        std::exit(1);
    }
    return value;
}

static AppConfig parse_args(int argc, char** argv) {
    AppConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next_val = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << std::format("Error: {} requires a value\n", name);
                std::exit(1);
            }
            return argv[++i];
        };

        if      (arg == "--host")       cfg.host       = std::string(next_val("--host"));
        else if (arg == "--port")       cfg.port       = static_cast<uint16_t>(parse_int(next_val("--port"), "--port"));
        else if (arg == "--path")       cfg.ws_path    = std::string(next_val("--path"));
        else if (arg == "--msg")        cfg.message    = std::string(next_val("--msg"));
        else if (arg == "--count")      cfg.count      = parse_int(next_val("--count"), "--count");
        else if (arg == "--interval")   cfg.interval_ms = parse_int(next_val("--interval"), "--interval");
        else if (arg == "--no-tls")     cfg.use_tls    = false;
        else if (arg == "--no-verify")  cfg.verify     = false;
        else if (arg == "--ca-cert")    cfg.ca_cert    = std::string(next_val("--ca-cert"));
        else if (arg == "--help") { print_usage(argv[0]); std::exit(0); }
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
    auto scheme = cfg.use_tls ? "wss" : "ws";
    spdlog::info("Connecting to {}://{}:{}{}", scheme, cfg.host, cfg.port, cfg.ws_path);

    eph::net::TransportConfig transport_cfg{
        .remote_host = cfg.host,
        .remote_port = cfg.port,
        .ws_path     = cfg.ws_path,
        .use_tls     = cfg.use_tls,
        .ca_cert_path = cfg.ca_cert,
        .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{30},
    };

    eph::net::SocketConfig sock_cfg{
        .host        = cfg.host,
        .port        = cfg.port,
        .tcp_nodelay = true,
        .tcp_keepalive = true,
    };

    auto tcp_factory = [sock_cfg]()
        -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        auto tcp = std::make_unique<eph::net::SocketTransport>(sock_cfg);
        auto result = tcp->connect(std::chrono::milliseconds{5000});
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    auto result = eph::net::Transport<eph::net::SocketTransport>::create(
        std::move(tcp_factory), transport_cfg);

    if (!result) {
        spdlog::error("Failed to connect: {}", result.error().message());
        return 1;
    }

    auto& tp = **result;
    spdlog::info("Connected ({})", scheme);

    // Echo loop
    int sent = 0, received = 0;
    auto interval = std::chrono::milliseconds(cfg.interval_ms);

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        if (cfg.count == 0 || sent < cfg.count) {
            auto msg = (cfg.count != 1)
                ? std::format("[{}] {}", sent + 1, cfg.message)
                : cfg.message;

            auto rc = tp.send_text(msg.data(), msg.size());
            if (rc == eph::net::SendError::kOk) {
                spdlog::info(">> {}", msg);
                ++sent;
            } else if (rc == eph::net::SendError::kQueueFull) {
                spdlog::warn("TX queue full, retrying...");
            } else {
                spdlog::error("Send failed: {}", eph::net::send_error_name(rc));
                break;
            }
        }

        auto deadline = std::chrono::steady_clock::now() + interval;
        while (std::chrono::steady_clock::now() < deadline &&
               g_running.load(std::memory_order_acquire)) {
            bool got = tp.recv([&](const uint8_t* data, uint16_t len) {
                std::string_view echo(reinterpret_cast<const char*>(data), len);
                spdlog::info("<< {}", echo);
                ++received;
            });
            if (!got) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        if (cfg.count > 0 && sent >= cfg.count && received >= cfg.count) {
            spdlog::info("All {} messages sent and echoed.", cfg.count);
            break;
        }
    }

    tp.stop();
    auto stats = tp.stats();
    spdlog::info("Stats:\n{}", stats.dump());
    return 0;
}
