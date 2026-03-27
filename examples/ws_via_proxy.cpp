/// @file ws_via_proxy.cpp
/// WebSocket connection through a SOCKS5 or HTTP CONNECT proxy.
///
/// Demonstrates using make_proxied_factory() to tunnel a WebSocket/TLS
/// connection through a proxy server. The proxy handshake happens during
/// TCP establishment — the rest of the pipeline (TLS, WS upgrade, data)
/// is identical to a direct connection.
///
/// Usage:
///   # Via SOCKS5 proxy (default)
///   ./ws_via_proxy --proxy-host 127.0.0.1 --proxy-port 1080
///
///   # Via HTTP CONNECT proxy
///   ./ws_via_proxy --proxy-host proxy.corp.com --proxy-port 3128 --http-connect
///
///   # With SOCKS5 authentication
///   ./ws_via_proxy --proxy-host proxy.corp.com --proxy-user alice --proxy-pass secret
///
/// Related examples:
///   - minimal_ws_client.cpp  — direct connection (no proxy)
///   - production_client.cpp  — reconnection + latency measurement

#include <cstdlib>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/net/proxy.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/net/transport.hpp"

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::info);

    // --- Parse arguments ---
    std::string target_host = "echo.websocket.org";
    uint16_t    target_port = 443;
    std::string proxy_host  = "127.0.0.1";
    uint16_t    proxy_port  = 1080;
    std::string proxy_user{};
    std::string proxy_pass{};
    bool        http_connect = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--host" && i + 1 < argc)       target_host = argv[++i];
        else if (arg == "--port" && i + 1 < argc)   target_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--proxy-host" && i + 1 < argc) proxy_host = argv[++i];
        else if (arg == "--proxy-port" && i + 1 < argc) proxy_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--proxy-user" && i + 1 < argc) proxy_user = argv[++i];
        else if (arg == "--proxy-pass" && i + 1 < argc) proxy_pass = argv[++i];
        else if (arg == "--http-connect") http_connect = true;
        else if (arg == "--help") {
            std::cerr << std::format(
                "Usage: {} [options]\n"
                "  --host HOST         Target WebSocket server (default: echo.websocket.org)\n"
                "  --port PORT         Target port (default: 443)\n"
                "  --proxy-host HOST   Proxy server address (default: 127.0.0.1)\n"
                "  --proxy-port PORT   Proxy port (default: 1080)\n"
                "  --proxy-user USER   SOCKS5 username (optional)\n"
                "  --proxy-pass PASS   SOCKS5 password (optional)\n"
                "  --http-connect      Use HTTP CONNECT instead of SOCKS5\n",
                argv[0]);
            return 0;
        }
    }

    // --- Configure proxy ---
    eph::net::proxy::ProxyConfig proxy_cfg{
        .host     = proxy_host,
        .port     = proxy_port,
        .type     = http_connect ? eph::net::proxy::ProxyType::kHttpConnect
                                 : eph::net::proxy::ProxyType::kSocks5,
        .username = proxy_user,
        .password = proxy_pass,
        .timeout  = std::chrono::milliseconds{5000},
    };

    // --- Create proxied TCP factory ---
    // make_proxied_factory() returns a TcpFactory that:
    //   1. Connects to the proxy server
    //   2. Performs SOCKS5/HTTP CONNECT handshake to reach target
    //   3. Returns the tunneled socket for TLS/WS handshake
    eph::net::SocketConfig sock_cfg{.tcp_nodelay = true};

    auto factory = eph::net::proxy::make_proxied_factory(
        sock_cfg, proxy_cfg, target_host, target_port);

    // --- Transport config (identical to direct connection) ---
    eph::net::TransportConfig transport_cfg{
        .remote_host = target_host,
        .remote_port = target_port,
        .use_tls     = true,
        .verify_peer = false,  // Relaxed for demo
    };

    // --- Connect through proxy ---
    auto proxy_type = http_connect ? "HTTP CONNECT" : "SOCKS5";
    spdlog::info("Connecting to wss://{}:{} via {} proxy {}:{}",
                 target_host, target_port, proxy_type, proxy_host, proxy_port);

    auto result = eph::net::Transport<eph::net::SocketTransport>::create(
        std::move(factory), transport_cfg);
    if (!result) {
        spdlog::error("Connection failed: {}", result.error().message());
        return 1;
    }
    auto& tp = **result;
    spdlog::info("Connected through proxy!");

    // --- Send and receive (same as direct connection) ---
    std::string msg = "hello through proxy";
    auto rc = tp.send_text(msg.data(), msg.size());
    if (rc != eph::net::SendError::kOk) {
        spdlog::error("Send failed: {}", eph::net::send_error_name(rc));
        tp.stop();
        return 1;
    }
    spdlog::info(">> {}", msg);

    for (int attempts = 0; attempts < 50; ++attempts) {
        bool got = tp.recv([](const uint8_t* data, size_t len) {
            spdlog::info("<< {}",
                std::string_view(reinterpret_cast<const char*>(data), len));
        });
        if (got) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    tp.stop();
    spdlog::info("Done.");
    return 0;
}
