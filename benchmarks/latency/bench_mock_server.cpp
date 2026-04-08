/// @file bench_mock_server.cpp
/// Standalone mock WS server for namespace-separated benchmarks.
///
/// Runs in host namespace (binds NIC-A). Socket bench client runs in a
/// separate namespace (NIC-B) via bench_latency.sh, forcing traffic
/// through real NICs (no loopback).
///
/// Usage: bench_mock_server --bind-ip IP [--port PORT] [--symbols SYM,...]
///            [--tick-us US] [--order-mode] [--cpu N]

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/time.hpp"
#include "bench_common.hpp"
#include "mock/ws_server.hpp"

int main(int argc, char** argv) {
    signal(SIGINT, sig);
    signal(SIGTERM, sig);

    bench::mock::MockServerConfig cfg;
    int cpu = 4;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bind-ip" && i+1 < argc) cfg.bind_ip = argv[++i];
        else if (arg == "--port" && i+1 < argc) cfg.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--symbols" && i+1 < argc) cfg.symbols = bench::split(argv[++i], ',');
        else if (arg == "--tick-us" && i+1 < argc) cfg.tick_interval = std::chrono::microseconds{std::stoi(argv[++i])};
        else if (arg == "--order-mode") cfg.order_mode = true;
        else if (arg == "--echo-mode") cfg.echo_mode = true;
        else if (arg == "--cpu" && i+1 < argc) cpu = std::stoi(argv[++i]);
    }

    if (cfg.bind_ip.empty()) {
        spdlog::error("--bind-ip is required");
        return 1;
    }

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    auto r = eph::utils::set_thread_affinity(cpu, "mock-server");
    if (!r) {
        spdlog::error("Failed to pin to core {}: {}", cpu, r.error());
        return 1;
    }
    spdlog::info("Pinned mock-server to core {}", cpu);
    spdlog::info("Starting mock server on {}:{}", cfg.bind_ip, cfg.port);
    bench::mock::run_mock_ws_server(cfg, g_running);
    return 0;
}
