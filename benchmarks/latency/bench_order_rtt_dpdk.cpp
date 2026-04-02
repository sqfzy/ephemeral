/// @file bench_order_rtt_dpdk.cpp
/// DPDK backend: order send + ExecutionReport RTT benchmark.
///
/// Uses DirectTransport (no threads). Mock server runs in-process on NIC-A.
///
/// Usage: bench_order_rtt_dpdk [EAL args] -- --server-ip IP --local-ip IP
///            --gateway-ip IP [--port PORT] [--duration SEC]
///            [--order-interval-us US] [--poll-cpu N] [--mock-cpu N]

#include <csignal>
#include <string>

#include <spdlog/spdlog.h>

#include "bench_impl.hpp"
#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/tcp.hpp"

int main(int argc, char** argv) {
    signal(SIGINT, sig);
    signal(SIGTERM, sig);

    if (!eph::utils::TSC::init()) { spdlog::error("TSC calibration failed"); return 1; }

    int app_argc = 0;
    char** app_argv = nullptr;
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--") {
            app_argc = argc - i - 1;
            app_argv = argv + i + 1;
            break;
        }
    }

    spdlog::info("Initializing DPDK EAL...");
    auto eal = eph::dpdk::EalGuard::init(argc, argv);
    if (!eal) { spdlog::error("EAL init failed: {}", eal.error()); return 1; }

    bench::BenchConfig cfg;
    std::string local_ip, gateway_ip;
    uint16_t dpdk_port = 0, local_port = 0;

    for (int i = 0; i < app_argc; ++i) {
        std::string arg = app_argv[i];
        if (arg == "--server-ip" && i + 1 < app_argc) cfg.server_ip = app_argv[++i];
        else if (arg == "--port" && i + 1 < app_argc) cfg.server_port = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--local-ip" && i + 1 < app_argc) local_ip = app_argv[++i];
        else if (arg == "--gateway-ip" && i + 1 < app_argc) gateway_ip = app_argv[++i];
        else if (arg == "--dpdk-port" && i + 1 < app_argc) dpdk_port = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--local-port" && i + 1 < app_argc) local_port = static_cast<uint16_t>(std::stoi(app_argv[++i]));
        else if (arg == "--duration" && i + 1 < app_argc) cfg.duration = std::chrono::seconds{std::stoi(app_argv[++i])};
        else if (arg == "--order-interval-us" && i + 1 < app_argc) cfg.order_interval = std::chrono::microseconds{std::stoi(app_argv[++i])};
        else if (arg == "--tick-us" && i + 1 < app_argc) cfg.tick_interval = std::chrono::microseconds{std::stoi(app_argv[++i])};
        else if (arg == "--poll-cpu" && i + 1 < app_argc) cfg.poll_cpu = std::stoi(app_argv[++i]);
        else if (arg == "--mock-cpu" && i + 1 < app_argc) cfg.mock_cpu = std::stoi(app_argv[++i]);
        else if (arg == "--help") {
            spdlog::info("Usage: bench_order_rtt_dpdk [EAL args] -- [app args]");
            spdlog::info("  --server-ip IP (required) --local-ip IP (required) --gateway-ip IP (required)");
            return 0;
        }
    }

    if (cfg.server_ip.empty()) { spdlog::error("--server-ip is required"); return 1; }
    if (local_ip.empty())      { spdlog::error("--local-ip is required"); return 1; }
    if (gateway_ip.empty())    { spdlog::error("--gateway-ip is required"); return 1; }

    auto mock = bench::start_mock(cfg, /*order_mode=*/true);

    using BenchTransport = eph::net::DirectTransport<eph::dpdk::TcpSession<>, eph::net::WsFramer, 4096>;

    auto tc = bench::make_bench_transport_config(cfg);
    eph::dpdk::DpdkEndpoint ep{.local_ip = local_ip, .gateway_ip = gateway_ip};
    eph::dpdk::ConnectorOptions opts{.platform = {.port_id = dpdk_port}, .local_port = local_port};

    auto conn = eph::dpdk::connect<BenchTransport>(ep, tc, opts);
    if (!conn) {
        spdlog::error("DPDK connect failed: {}", conn.error());
        bench::stop_mock(mock);
        return 1;
    }

    // Shared poll+send loop — same code as socket bench
    bench::run_order_rtt_poll_loop(*conn->transport, cfg, "Order RTT Benchmark Results (DPDK)");

    conn->transport->stop();
    bench::stop_mock(mock);
    return 0;
}
