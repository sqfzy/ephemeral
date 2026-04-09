/// @file exchange/bench_market.cpp
/// bench_lat_exchange_market / *_dpdk — multi-symbol market data RX bench.
///
/// Connects to mock_lat_exchange_ws over plain WS, then drives the
/// `OneWayScenario` runner. The runner records one OneWaySample per
/// inbound bookTicker frame (other event types are ignored).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>

#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "../core/cpu_pin.hpp"
#include "../core/runner.hpp"
#include "../core/signal.hpp"
#include "../ws/client.hpp"
#include "scenario.hpp"

using namespace bench;

int main(int argc, char** argv) {
#ifdef EPH_USE_DPDK
    std::fprintf(stderr,
        "[bench_lat_exchange_market_dpdk] WARNING: DPDK transport not "
        "yet implemented; using POSIX sockets identical to "
        "bench_lat_exchange_market.\n");
    constexpr const char* kTransport = "dpdk";
#else
    constexpr const char* kTransport = "kernel";
#endif

    install_signal_handlers();
    auto cfg = parse_common(argc, argv);
    if (cfg.server_ip.empty() || cfg.server_port == 0) {
        spdlog::error("--server-ip and --port are required");
        return 1;
    }
    if (cfg.client_cpu < 0) {
        spdlog::error("--client-cpu <cpu> is required");
        return 1;
    }
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = pin_thread_strict(cfg.client_cpu, "bench_ex_mk", policy); !p) {
        spdlog::error("pin_thread_strict failed: {}", p.error());
        return 1;
    }

    auto fd = ws::connect_ws(cfg.server_ip, cfg.server_port);
    if (!fd) { spdlog::error("connect_ws failed: {}", fd.error()); return 1; }
    spdlog::info("bench_lat_exchange_market: connected to {}:{}",
                 cfg.server_ip, cfg.server_port);

    exchange::MarketRxScenario scenario{*fd};
    BenchRunner runner{cfg, "exchange/market", kTransport};
    runner.run_oneway(scenario);

    ::close(*fd);
    return 0;
}
