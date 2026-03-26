/// @file dpdk_quickstart.cpp
/// DPDK backend quick start — kernel-bypass WebSocket client.
///
/// Demonstrates the full DPDK connection pipeline:
///   EAL init → Platform (NIC port) → ARP resolution → TCP → TLS → WebSocket
///
/// This is the DPDK counterpart of minimal_ws_client.cpp. It uses the
/// high-level eph::dpdk::connect() helper that collapses the entire
/// pipeline into a single call.
///
/// Usage:
///   sudo ./dpdk_quickstart -a 0000:28:00.0 --
///       --host echo.websocket.org
///       --local-ip 172.31.23.112 --gateway-ip 172.31.16.1
///
/// Arguments BEFORE '--' are DPDK EAL arguments (PCIe device, cores, etc.).
/// Arguments AFTER '--' are application arguments.
///
/// Prerequisites:
///   - DPDK-compatible NIC bound to a DPDK driver (vfio-pci, igb_uio, etc.)
///   - Huge pages configured: echo 512 > /proc/sys/vm/nr_hugepages
///   - Root permissions (or CAP_NET_ADMIN + CAP_SYS_RAWIO)
///
/// Related examples:
///   - minimal_ws_client.cpp  — same thing with socket backend
///   - ws_echo_client.cpp     — unified client supporting both backends

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/eal.hpp"

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// Argument parsing (application args only — after '--')
// ─────────────────────────────────────────────────────────────────────────────

struct AppConfig {
    std::string host        = "echo.websocket.org";
    std::string local_ip{};
    std::string gateway_ip{};
    uint16_t    local_port  = 0;  // 0 = random ephemeral
    uint16_t    dpdk_port   = 0;
    std::string message     = "hello dpdk";
    int         count       = 3;
    int         interval_ms = 1000;
};

struct SplitArgs { int eal_argc; char** eal_argv; int app_argc; char** app_argv; };

static SplitArgs split_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            return { i, argv, argc - i - 1, argv + i + 1 };
        }
    }
    return { argc, argv, 0, nullptr };
}

static AppConfig parse_app_args(int argc, char** argv) {
    AppConfig cfg;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << std::format("Error: {} requires a value\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if      (arg == "--host")       cfg.host       = next("--host");
        else if (arg == "--local-ip")   cfg.local_ip   = next("--local-ip");
        else if (arg == "--gateway-ip") cfg.gateway_ip = next("--gateway-ip");
        else if (arg == "--local-port") cfg.local_port = static_cast<uint16_t>(std::atoi(next("--local-port")));
        else if (arg == "--dpdk-port")  cfg.dpdk_port  = static_cast<uint16_t>(std::atoi(next("--dpdk-port")));
        else if (arg == "--msg")        cfg.message    = next("--msg");
        else if (arg == "--count")      cfg.count      = std::atoi(next("--count"));
        else if (arg == "--interval")   cfg.interval_ms = std::atoi(next("--interval"));
        else if (arg == "--help") {
            std::cerr << std::format(
                "Usage: {} [EAL args] -- [app args]\n"
                "\n"
                "App options:\n"
                "  --host <hostname>     WebSocket server (default: echo.websocket.org)\n"
                "  --local-ip <ip>       Local IPv4 on DPDK port (required)\n"
                "  --gateway-ip <ip>     Gateway for ARP resolution (required)\n"
                "  --local-port <port>   Local TCP source port (default: random)\n"
                "  --dpdk-port <n>       DPDK port ID (default: 0)\n"
                "  --msg <message>       Message to send (default: \"hello dpdk\")\n"
                "  --count <n>           Number of messages (default: 3)\n"
                "  --interval <ms>       Milliseconds between sends (default: 1000)\n",
                "dpdk_quickstart");
            std::exit(0);
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

    // ── Step 1: Split EAL args from application args ──────────────────────
    auto [eal_argc, eal_argv, app_argc, app_argv] = split_args(argc, argv);
    int parse_argc = (app_argv != nullptr) ? app_argc : argc - 1;
    char** parse_argv = (app_argv != nullptr) ? app_argv : argv + 1;
    auto cfg = parse_app_args(parse_argc, parse_argv);

    if (cfg.local_ip.empty() || cfg.gateway_ip.empty()) {
        spdlog::error("--local-ip and --gateway-ip are required for DPDK backend");
        return 1;
    }

    // ── Step 2: Initialize DPDK EAL ──────────────────────────────────────
    // EAL (Environment Abstraction Layer) manages huge pages, memory pools,
    // and NIC drivers. EalGuard provides RAII cleanup.
    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(eal_argc, eal_argv);
    if (!eal) {
        spdlog::error("EAL init failed: {}", eal.error());
        return 1;
    }
    spdlog::info("EAL initialized ({} args consumed)", eal->args_consumed());

    // ── Step 3: Connect using the simplest overload ─────────────────────
    // connect(host, endpoint) resolves DNS, inits Platform, ARP, TCP, TLS, WS
    // in one call. Uses defaults: port 443, TLS on, verify_peer on.
    // Required fields (local_ip, gateway_ip) are enforced at compile time —
    // DpdkEndpoint has no defaults, so omitting them is a compile error.
    spdlog::info("Connecting to wss://{} (DPDK)", cfg.host);
    spdlog::info("Local={}:{}, gateway={}", cfg.local_ip, cfg.local_port, cfg.gateway_ip);

    eph::dpdk::DpdkEndpoint endpoint{
        .local_ip   = cfg.local_ip,
        .gateway_ip = cfg.gateway_ip,
    };

    // Simplest form: just hostname + required endpoint
    // For custom port/TLS/path, use connect(endpoint, TransportConfig{...}).
    eph::dpdk::ConnectorOptions opts{
        .platform   = {.port_id = cfg.dpdk_port},
        .local_port = cfg.local_port,
    };
    auto conn = eph::dpdk::connect(cfg.host, endpoint, opts);
    if (!conn) {
        spdlog::error("DPDK connect failed: {}", conn.error());
        return 1;
    }
    auto& tp = *conn->transport;
    spdlog::info("Connected via DPDK!");

    // ── Step 4: Send/receive echo ────────────────────────────────────────
    int sent = 0, received = 0;
    auto interval = std::chrono::milliseconds(cfg.interval_ms);

    while (g_running.load(std::memory_order_acquire) && tp.is_running()) {
        if (sent < cfg.count) {
            auto msg = std::format("[{}] {}", sent + 1, cfg.message);
            auto rc = tp.send_text(msg.data(), msg.size());
            if (rc == eph::net::SendError::kOk) {
                spdlog::info(">> {}", msg);
                ++sent;
            } else {
                spdlog::error("Send failed: {}", eph::net::send_error_name(rc));
                break;
            }
        }

        auto deadline = std::chrono::steady_clock::now() + interval;
        while (std::chrono::steady_clock::now() < deadline &&
               g_running.load(std::memory_order_acquire)) {
            bool got = tp.recv([&](const uint8_t* data, size_t len) {
                std::string_view echo(reinterpret_cast<const char*>(data), len);
                spdlog::info("<< {}", echo);
                ++received;
            });
            if (!got) {
                std::this_thread::sleep_for(std::chrono::microseconds{100});
            }
        }

        if (sent >= cfg.count && received >= cfg.count) {
            spdlog::info("All {} messages exchanged.", cfg.count);
            break;
        }
    }

    // ── Step 5: Shutdown ─────────────────────────────────────────────────
    tp.stop();
    auto stats = tp.stats();
    spdlog::info("Stats:\n{}", stats.dump());
    // EalGuard RAII handles eal_cleanup()
    return 0;
}
