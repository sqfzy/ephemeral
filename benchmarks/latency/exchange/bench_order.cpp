/// @file exchange/bench_order.cpp
/// bench_lat_exchange_order / *_dpdk — order RTT bench with N inflight pipeline.
///
/// Usage:
///   ./bench_lat_exchange_order --server-ip IP --port PORT --client-cpu N
///       --duration 10 --warmup 2 --inflights 1,4,16,64
///       [--allow-non-isolated]
///
/// `--inflights 1` is the synchronous baseline; higher values measure how
/// p99/p999 degrades under pipelined load.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

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

namespace {
std::vector<int> parse_inflights_or_default(int argc, char** argv) {
    for (int i = 0; i < argc - 1; ++i) {
        if (std::string_view{argv[i]} == "--inflights") {
            return config_detail::parse_int_list(argv[i + 1]);
        }
    }
    return {1, 4, 16, 64};
}
} // namespace

int main(int argc, char** argv) {
#ifdef EPH_USE_DPDK
    std::fprintf(stderr,
        "[bench_lat_exchange_order_dpdk] WARNING: DPDK transport not yet "
        "implemented; using POSIX sockets identical to "
        "bench_lat_exchange_order.\n");
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
    if (auto p = pin_thread_strict(cfg.client_cpu, "bench_ex_or", policy); !p) {
        spdlog::error("pin_thread_strict failed: {}", p.error());
        return 1;
    }

    auto fd = ws::connect_ws(cfg.server_ip, cfg.server_port);
    if (!fd) { spdlog::error("connect_ws failed: {}", fd.error()); return 1; }
    spdlog::info("bench_lat_exchange_order: connected to {}:{}",
                 cfg.server_ip, cfg.server_port);

    auto inflights = parse_inflights_or_default(argc, argv);
    exchange::OrderRttScenario scenario{*fd};
    BenchRunner runner{cfg, "exchange/order", kTransport};
    runner.run_rtt_inflight_sweep(scenario, std::span<const int>(inflights));

    ::close(*fd);
    return 0;
}
