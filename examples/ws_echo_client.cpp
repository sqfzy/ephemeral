/// @file ws_echo_client.cpp
/// Unified WebSocket echo client — supports both socket and DPDK backends.
///
/// Usage (socket backend, default):
///   ./ws_echo_client --host echo.websocket.org
///   ./ws_echo_client --host echo.websocket.org --port 443 --path / \
///       --msg "hello" --count 5 --interval 1000 --no-tls
///
/// Usage (DPDK backend, requires DPDK build):
///   sudo ./ws_echo_client [EAL args] -- --backend dpdk \
///       --host echo.websocket.org --local-ip 172.31.23.112 \
///       --gateway-ip 172.31.16.1 --msg "hello dpdk"
///
/// For DPDK mode, EAL args (e.g. -a 0000:28:00.0) go BEFORE the '--' separator;
/// application args go AFTER it.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/net/socket_transport.hpp"

#ifdef EPH_HAS_DPDK
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/eal.hpp"
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

enum class Backend { Socket, Dpdk };

struct AppConfig {
    Backend     backend     = Backend::Socket;

    // Common
    std::string host        = "echo.websocket.org";
    uint16_t    port        = 443;
    std::string ws_path     = "/";
    std::string message     = "hello ephemeral";
    int         count       = 3;       // 0 = infinite
    int         interval_ms = 1000;
    bool        use_tls     = true;
    bool        verify      = true;
    std::string ca_cert{};

    // DPDK-specific
    std::string local_ip{};
    std::string gateway_ip{};
    uint16_t    local_port  = 0;       // 0 = random ephemeral
    uint16_t    dpdk_port   = 0;
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
        "       {} [EAL args] -- --backend dpdk [options]\n"
        "\n"
        "Common options:\n"
        "  --backend <socket|dpdk>  Transport backend (default: socket)\n"
        "  --host <hostname>        WebSocket server hostname (default: echo.websocket.org)\n"
        "  --port <port>            Server port (default: 443)\n"
        "  --path <path>            WebSocket upgrade path (default: /)\n"
        "  --msg <message>          Message to send (default: \"hello ephemeral\")\n"
        "  --count <n>              Number of messages, 0=infinite (default: 3)\n"
        "  --interval <ms>          Milliseconds between sends (default: 1000)\n"
        "  --no-tls                 Use plain WebSocket (ws://) instead of WSS\n"
        "  --no-verify              Disable TLS certificate verification\n"
        "  --ca-cert <path>         CA certificate file\n"
        "  --help                   Show this help\n"
#ifdef EPH_HAS_DPDK
        "\n"
        "DPDK-specific options (--backend dpdk):\n"
        "  --local-ip <ip>          Local IPv4 address on DPDK port (required)\n"
        "  --gateway-ip <ip>        Gateway IPv4 for ARP resolution (required)\n"
        "  --local-port <port>      Local TCP source port (default: random)\n"
        "  --dpdk-port <n>          DPDK port ID (default: 0)\n"
#endif
        , prog, prog);
}

/// Parse integer from string, exit on failure.
static int parse_int(std::string_view sv, std::string_view flag) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) {
        std::cerr << std::format("Error: {} requires a valid integer, got '{}'\n", flag, sv);
        std::exit(1);
    }
    return value;
}

static AppConfig parse_app_args(int argc, char** argv) {
    AppConfig cfg;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];

        auto next_val = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << std::format("Error: {} requires a value\n", name);
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--backend") {
            auto val = next_val("--backend");
            if (val == "socket")    cfg.backend = Backend::Socket;
            else if (val == "dpdk") cfg.backend = Backend::Dpdk;
            else {
                std::cerr << std::format("Error: unknown backend '{}', use 'socket' or 'dpdk'\n", val);
                std::exit(1);
            }
        }
        else if (arg == "--host")       cfg.host       = std::string(next_val("--host"));
        else if (arg == "--port")       cfg.port       = static_cast<uint16_t>(parse_int(next_val("--port"), "--port"));
        else if (arg == "--path")       cfg.ws_path    = std::string(next_val("--path"));
        else if (arg == "--msg")        cfg.message    = std::string(next_val("--msg"));
        else if (arg == "--count")      cfg.count      = parse_int(next_val("--count"), "--count");
        else if (arg == "--interval")   cfg.interval_ms = parse_int(next_val("--interval"), "--interval");
        else if (arg == "--no-tls")     cfg.use_tls    = false;
        else if (arg == "--no-verify")  cfg.verify     = false;
        else if (arg == "--ca-cert")    cfg.ca_cert    = std::string(next_val("--ca-cert"));
#ifdef EPH_HAS_DPDK
        else if (arg == "--local-ip")   cfg.local_ip   = std::string(next_val("--local-ip"));
        else if (arg == "--gateway-ip") cfg.gateway_ip = std::string(next_val("--gateway-ip"));
        else if (arg == "--local-port") cfg.local_port = static_cast<uint16_t>(parse_int(next_val("--local-port"), "--local-port"));
        else if (arg == "--dpdk-port")  cfg.dpdk_port  = static_cast<uint16_t>(parse_int(next_val("--dpdk-port"), "--dpdk-port"));
#endif
        else if (arg == "--help") { print_usage("ws_echo_client"); std::exit(0); }
        else {
            std::cerr << std::format("Unknown argument: {}\n", arg);
            print_usage("ws_echo_client");
            std::exit(1);
        }
    }
    return cfg;
}

/// Split argv at '--' into EAL args and app args (for DPDK mode).
struct SplitArgs { int eal_argc; char** eal_argv; int app_argc; char** app_argv; };

static SplitArgs split_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            return { i, argv, argc - i - 1, argv + i + 1 };
        }
    }
    return { argc, argv, 0, nullptr };
}

// ─────────────────────────────────────────────────────────────────────────────
// Echo loop — backend-agnostic
// ─────────────────────────────────────────────────────────────────────────────

template <typename Transport>
static int echo_loop(Transport& tp, const AppConfig& cfg) {
    int sent = 0, received = 0;
    auto interval = std::chrono::milliseconds(cfg.interval_ms);

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        // Send
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

        // Receive echoes
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

// ─────────────────────────────────────────────────────────────────────────────
// Socket backend
// ─────────────────────────────────────────────────────────────────────────────

static int run_socket(const AppConfig& cfg) {
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

    spdlog::info("Connected ({})", scheme);
    return echo_loop(**result, cfg);
}

// ─────────────────────────────────────────────────────────────────────────────
// DPDK backend
// ─────────────────────────────────────────────────────────────────────────────

#ifdef EPH_HAS_DPDK

static int run_dpdk(const AppConfig& cfg, int eal_argc, char** eal_argv) {
    if (cfg.local_ip.empty()) {
        spdlog::error("--local-ip is required for DPDK backend");
        return 1;
    }
    if (cfg.gateway_ip.empty()) {
        spdlog::error("--gateway-ip is required for DPDK backend");
        return 1;
    }

    auto scheme = cfg.use_tls ? "wss" : "ws";
    spdlog::info("Connecting to {}://{}:{}{} (DPDK)", scheme, cfg.host, cfg.port, cfg.ws_path);
    spdlog::info("Local={}:{}, gateway={}", cfg.local_ip, cfg.local_port, cfg.gateway_ip);

    // EAL lifetime managed by RAII guard
    auto eal = eph::dpdk::EalGuard::init(eal_argc, eal_argv);
    if (!eal) {
        spdlog::error("EAL init failed: {}", eal.error());
        return 1;
    }
    spdlog::info("EAL initialized ({} args consumed)", eal->args_consumed());

    // One-shot connect: DNS → Platform → MAC → ARP → TCP → Transport
    eph::dpdk::ConnectorConfig conn_cfg{
        .platform = { .port_id = cfg.dpdk_port },
        .local_ip   = cfg.local_ip,
        .gateway_ip = cfg.gateway_ip,
        .local_port = cfg.local_port,
    };

    eph::net::TransportConfig transport_cfg{
        .remote_host   = cfg.host,
        .remote_port   = cfg.port,
        .ws_path       = cfg.ws_path,
        .use_tls       = cfg.use_tls,
        .ca_cert_path  = cfg.ca_cert,
        .verify_peer   = cfg.verify,
        .tcp_timeout   = std::chrono::milliseconds{5000},
        .tls_timeout   = std::chrono::milliseconds{5000},
        .ws_timeout    = std::chrono::milliseconds{5000},
        .ping_interval = std::chrono::seconds{30},
    };

    auto conn = eph::dpdk::connect(conn_cfg, transport_cfg);
    if (!conn) {
        spdlog::error("DPDK connect failed: {}", conn.error());
        return 1;
    }

    spdlog::info("{} connection established (DPDK)", scheme);
    return echo_loop(*conn->transport, cfg);
    // EalGuard RAII handles eal_cleanup()
}

#endif // EPH_HAS_DPDK

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    spdlog::set_level(spdlog::level::info);

    // Check for '--' separator (DPDK EAL convention)
    auto [eal_argc, eal_argv, app_argc, app_argv] = split_args(argc, argv);

    // If no '--' separator found, all args are app args
    int parse_argc = (app_argv != nullptr) ? app_argc : argc - 1;
    char** parse_argv = (app_argv != nullptr) ? app_argv : argv + 1;

    auto cfg = parse_app_args(parse_argc, parse_argv);

    switch (cfg.backend) {
    case Backend::Socket:
        return run_socket(cfg);

    case Backend::Dpdk:
#ifdef EPH_HAS_DPDK
        return run_dpdk(cfg, eal_argc, eal_argv);
#else
        spdlog::error("DPDK backend not available — rebuild with DPDK support");
        return 1;
#endif
    }

    return 1;
}
