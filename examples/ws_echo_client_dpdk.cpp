/// @file ws_echo_client_dpdk.cpp
/// WebSocket echo client using the DPDK (kernel-bypass) backend.
///
/// Usage:
///   sudo ./ws_echo_client_dpdk [EAL args] -- [app args]
///
///   sudo ./ws_echo_client_dpdk -a 0000:28:00.0 -- \
///       --host echo.websocket.org --local-ip 172.31.23.112 \
///       --gateway-ip 172.31.16.1 --msg "hello dpdk"
///
/// EAL args (e.g. -a <pci_addr>) go BEFORE the '--' separator;
/// application args go AFTER it.
///
/// For the socket backend, see ws_echo_client.cpp.

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include <rte_bus.h>

#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/eal.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct AppConfig {
    std::string host        = "echo.websocket.org";
    uint16_t    port        = 443;
    std::string ws_path     = "/";
    std::string message     = "hello dpdk";
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
        "Usage: {} [EAL args] -- [app args]\n"
        "\n"
        "App options:\n"
        "  --host <hostname>        WebSocket server hostname (default: echo.websocket.org)\n"
        "  --port <port>            Server port (default: 443)\n"
        "  --path <path>            WebSocket upgrade path (default: /)\n"
        "  --msg <message>          Message to send (default: \"hello dpdk\")\n"
        "  --count <n>              Number of messages, 0=infinite (default: 3)\n"
        "  --interval <ms>          Milliseconds between sends (default: 1000)\n"
        "  --no-tls                 Use plain WebSocket (ws://) instead of WSS\n"
        "  --no-verify              Disable TLS certificate verification\n"
        "  --ca-cert <path>         CA certificate file\n"
        "\n"
        "DPDK options:\n"
        "  --local-ip <ip>          Local IPv4 address on DPDK port (required)\n"
        "  --gateway-ip <ip>        Gateway IPv4 for ARP resolution (required)\n"
        "  --local-port <port>      Local TCP source port (default: random)\n"
        "  --dpdk-port <n>          DPDK port ID (default: 0)\n"
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
        else if (arg == "--local-ip")   cfg.local_ip   = std::string(next_val("--local-ip"));
        else if (arg == "--gateway-ip") cfg.gateway_ip = std::string(next_val("--gateway-ip"));
        else if (arg == "--local-port") cfg.local_port = static_cast<uint16_t>(parse_int(next_val("--local-port"), "--local-port"));
        else if (arg == "--dpdk-port")  cfg.dpdk_port  = static_cast<uint16_t>(parse_int(next_val("--dpdk-port"), "--dpdk-port"));
        else if (arg == "--help") { print_usage("ws_echo_client_dpdk"); std::exit(0); }
        else {
            std::cerr << std::format("Unknown argument: {}\n", arg);
            print_usage("ws_echo_client_dpdk");
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

    auto [eal_argc, eal_argv, app_argc, app_argv] = split_args(argc, argv);
    int parse_argc = (app_argv != nullptr) ? app_argc : argc - 1;
    char** parse_argv = (app_argv != nullptr) ? app_argv : argv + 1;
    auto cfg = parse_app_args(parse_argc, parse_argv);

    if (cfg.local_ip.empty()) {
        spdlog::error("--local-ip is required");
        return 1;
    }
    if (cfg.gateway_ip.empty()) {
        spdlog::error("--gateway-ip is required");
        return 1;
    }

    auto scheme = cfg.use_tls ? "wss" : "ws";
    spdlog::info("Connecting to {}://{}:{}{} (DPDK)", scheme, cfg.host, cfg.port, cfg.ws_path);
    spdlog::info("Local={}:{}, gateway={}", cfg.local_ip, cfg.local_port, cfg.gateway_ip);

    // EAL init
    auto eal = eph::dpdk::EalGuard::init(eal_argc, eal_argv);
    if (!eal) {
        spdlog::error("EAL init failed: {}", eal.error());
        return 1;
    }
    spdlog::info("EAL initialized ({} args consumed)", eal->args_consumed());

    // Require a real PCI NIC
    if (rte_eth_dev_count_avail() == 0) {
        spdlog::error("No DPDK ports available — bind a physical NIC with -a <pci_addr>");
        return 1;
    }
    {
        rte_eth_dev_info dev_info{};
        if (rte_eth_dev_info_get(cfg.dpdk_port, &dev_info) != 0) {
            spdlog::error("Failed to query DPDK port {}", cfg.dpdk_port);
            return 1;
        }
        const char* bus_name = dev_info.device
            ? rte_bus_name(rte_bus_find_by_device(dev_info.device))
            : nullptr;
        if (!bus_name || std::strcmp(bus_name, "pci") != 0) {
            spdlog::error("DPDK port {} is not a PCI device (bus={}) — "
                          "this example requires a real NIC",
                          cfg.dpdk_port, bus_name ? bus_name : "none");
            return 1;
        }
        spdlog::info("DPDK port {}: driver={}, bus=pci",
                     cfg.dpdk_port, dev_info.driver_name);
    }

    // Connect
    eph::dpdk::DpdkEndpoint endpoint{
        .local_ip   = cfg.local_ip,
        .gateway_ip = cfg.gateway_ip,
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

    auto conn = eph::dpdk::connect(endpoint, transport_cfg,
                                   {.platform = {.port_id = cfg.dpdk_port},
                                    .local_port = cfg.local_port});
    if (!conn) {
        spdlog::error("DPDK connect failed: {}", conn.error());
        return 1;
    }

    auto& tp = *conn->transport;
    spdlog::info("{} connection established (DPDK)", scheme);

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
