/// @file socket_wss_client.cpp
/// Socket-based WebSocket (WSS) client example — no DPDK required.
///
/// Demonstrates the ephemeral stack over standard POSIX sockets:
///   SocketTransport (TCP) → TLS 1.3 → WebSocket Upgrade → send/recv loop.
///
/// Usage:
///   ./socket_wss_client --host echo.websocket.org
///   ./socket_wss_client --host echo.websocket.org --port 443 --path / \
///       --msg "hello world" --count 5 --interval 1000
///   ./socket_wss_client --host echo.websocket.org --no-verify

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/net/socket_transport.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct AppConfig {
    std::string host     = "echo.websocket.org";
    uint16_t    port     = 443;
    std::string ws_path  = "/";
    std::string message  = "hello ephemeral";
    int         count    = 3;      // Number of messages to send
    int         interval = 1000;   // Milliseconds between sends
    bool        verify   = true;   // TLS peer verification
};

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument parsing
// ─────────────────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --host <hostname>     WebSocket server hostname (default: echo.websocket.org)\n"
              << "  --port <port>         Server port (default: 443)\n"
              << "  --path <path>         WebSocket upgrade path (default: /)\n"
              << "  --msg <message>       Message to send (default: \"hello ephemeral\")\n"
              << "  --count <n>           Number of messages (default: 3, 0 = infinite)\n"
              << "  --interval <ms>       Milliseconds between sends (default: 1000)\n"
              << "  --no-verify           Disable TLS certificate verification\n"
              << "  --help                Show this help\n";
}

static AppConfig parse_args(int argc, char* argv[]) {
    AppConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--path" && i + 1 < argc) {
            cfg.ws_path = argv[++i];
        } else if (arg == "--msg" && i + 1 < argc) {
            cfg.message = argv[++i];
        } else if (arg == "--count" && i + 1 < argc) {
            cfg.count = std::atoi(argv[++i]);
        } else if (arg == "--interval" && i + 1 < argc) {
            cfg.interval = std::atoi(argv[++i]);
        } else if (arg == "--no-verify") {
            cfg.verify = false;
        } else if (arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    // Set log level: DEBUG for verbose handshake tracing, INFO for production
    spdlog::set_level(spdlog::level::info);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Connecting to wss://" << cfg.host << ":" << cfg.port
              << cfg.ws_path << "\n";

    // -- Configure the socket transport --
    eph::net::SocketConfig sock_cfg{
        .host         = cfg.host,
        .port         = cfg.port,
        .tcp_nodelay  = true,
        .tcp_keepalive = true,
    };

    // -- Configure the WSS transport --
    eph::net::TransportConfig transport_cfg{
        .remote_host = cfg.host,
        .remote_port = cfg.port,
        .ws_path     = cfg.ws_path,
        .verify_peer = cfg.verify,
        .max_reconnect_attempts = 3,
        .ping_interval = std::chrono::seconds{30},
    };

    // TCP factory: creates a new connected SocketTransport on each call.
    // Used for initial connection and auto-reconnection.
    auto tcp_factory = [sock_cfg]()
        -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        auto tcp = std::make_unique<eph::net::SocketTransport>(sock_cfg);
        auto result = tcp->connect(std::chrono::milliseconds{5000});
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    // -- Create transport (performs TCP + TLS + WS handshake) --
    auto result = eph::net::SocketWssTransport::create(
        std::move(tcp_factory), transport_cfg);

    if (!result) {
        std::cerr << "Failed to connect: " << result.error() << "\n";
        return 1;
    }

    auto& transport = *result;

    std::cout << "Connected! TLS: " << transport->tls_version()
              << ", cipher: " << transport->cipher_name() << "\n";

    // -- Send/receive loop --
    int sent = 0;
    int received = 0;

    while (g_running.load(std::memory_order_acquire) &&
           transport->is_running()) {
        // Send a message
        if (cfg.count == 0 || sent < cfg.count) {
            auto msg = std::format("[{}] {}", sent + 1, cfg.message);
            auto rc = transport->send_text(msg);
            if (rc == eph::net::SendError::kOk) {
                std::cout << "TX: " << msg << "\n";
                ++sent;
            } else {
                std::cerr << "Send failed: " << eph::net::send_error_name(rc) << "\n";
            }
        }

        // Wait for echo response (blocking, up to 2x interval)
        auto timeout = std::chrono::milliseconds{cfg.interval * 2};
        transport->wait_recv(
            [&](const uint8_t* data, uint16_t len) {
                std::string_view reply(
                    reinterpret_cast<const char*>(data), len);
                std::cout << "RX: " << reply << "\n";
                ++received;
            },
            timeout);

        // Check if we've sent all messages and received all replies
        if (cfg.count > 0 && sent >= cfg.count && received >= cfg.count) {
            std::cout << "All " << cfg.count
                      << " messages sent and echoed.\n";
            break;
        }

        // Inter-message delay
        if (cfg.count == 0 || sent < cfg.count) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds{cfg.interval});
        }
    }

    // -- Graceful shutdown --
    transport->stop();

    auto stats = transport->stats();
    std::cout << "\nStats:\n" << stats.dump() << "\n";

    return 0;
}
