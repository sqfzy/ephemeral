/// @file bench_mock_server.cpp
/// Standalone mock WS server for namespace-separated benchmarks.
/// Usage: bench_mock_server --bind-ip IP [--port PORT] [--symbols SYM,...] [--tick-us US] [--order-mode]

#include <atomic>
#include <chrono>
#include <csignal>
#include <string>

#include <spdlog/spdlog.h>

#include "eph/utils/time.hpp"
#include "bench_common.hpp"
#include "mock/mock_ws_server.hpp"

int main(int argc, char** argv) {
    signal(SIGINT, sig);
    signal(SIGTERM, sig);

    bench::mock::MockServerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bind-ip" && i+1 < argc) cfg.bind_ip = argv[++i];
        else if (arg == "--port" && i+1 < argc) cfg.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--symbols" && i+1 < argc) cfg.symbols = bench::split(argv[++i], ',');
        else if (arg == "--tick-us" && i+1 < argc) cfg.tick_interval = std::chrono::microseconds{std::stoi(argv[++i])};
        else if (arg == "--order-mode") cfg.order_mode = true;
    }

    eph::utils::TSC::init();
    spdlog::info("Starting standalone mock server on {}:{}", cfg.bind_ip, cfg.port);
    bench::mock::run_mock_ws_server(cfg, g_running);
    return 0;
}
