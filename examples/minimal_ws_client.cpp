/// @file minimal_ws_client.cpp
/// Minimal WebSocket echo client — the simplest way to use ephemeral.
///
/// Connects to a WebSocket server, sends one message, prints the echo,
/// and exits. This is the first example to read if you're new to ephemeral.
///
/// Usage:
///   ./minimal_ws_client                             # default: wss://echo.websocket.org/
///   ./minimal_ws_client --host myserver.com --port 8080 --no-tls
///
/// Related examples:
///   - production_client.cpp  — latency measurement, reconnection, state callbacks
///   - ws_echo_client.cpp     — full-featured client with DPDK support

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
#include "eph/transport/transport.hpp"

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    // Minimal argument parsing
    std::string host    = "echo.websocket.org";
    uint16_t    port    = 443;
    bool        use_tls = true;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc)  host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--no-tls") use_tls = false;
        else if (arg == "--help") {
            std::cerr << std::format("Usage: {} [--host H] [--port P] [--no-tls]\n", argv[0]);
            return 0;
        }
    }

    // 1. Configure the transport
    eph::net::SocketConfig sock_cfg{ .host = host, .port = port, .tcp_nodelay = true };
    eph::net::TransportConfig transport_cfg{
        .remote_host = host,
        .remote_port = port,
        .use_tls     = use_tls,
        .verify_peer = false,   // Relaxed for demo — enable in production
    };

    // 2. Create TCP factory (how to establish the underlying TCP connection)
    auto tcp_factory = [&sock_cfg]()
        -> std::expected<std::unique_ptr<eph::net::SocketTransport>, std::string> {
        auto tcp = std::make_unique<eph::net::SocketTransport>(sock_cfg);
        auto result = tcp->connect(std::chrono::milliseconds{5000});
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    // 3. Connect (TCP → TLS handshake → WebSocket upgrade)
    auto scheme = use_tls ? "wss" : "ws";
    spdlog::info("Connecting to {}://{}:{}/", scheme, host, port);

    auto result = eph::net::Transport<eph::net::SocketTransport>::create(
        std::move(tcp_factory), transport_cfg);
    if (!result) {
        spdlog::error("Connection failed: {}", result.error().message());
        return 1;
    }
    auto& tp = **result;
    spdlog::info("Connected!");

    // 4. Send a message
    std::string msg = "hello ephemeral";
    auto rc = tp.send_text(msg.data(), msg.size());
    if (rc != eph::net::SendError::kOk) {
        spdlog::error("Send failed: {}", eph::net::send_error_name(rc));
        tp.stop();
        return 1;
    }
    spdlog::info(">> {}", msg);

    // 5. Wait for echo
    for (int attempts = 0; attempts < 50; ++attempts) {
        bool got = tp.recv([](const uint8_t* data, size_t len) {
            std::string_view echo(reinterpret_cast<const char*>(data), len);
            spdlog::info("<< {}", echo);
        });
        if (got) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    // 6. Clean shutdown
    tp.stop();
    return 0;
}
