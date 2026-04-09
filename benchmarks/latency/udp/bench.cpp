/// @file udp/bench.cpp
/// bench_udp / bench_udp_dpdk — UDP RTT bench client.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "../core/cpu_pin.hpp"
#include "../core/runner.hpp"
#include "../core/signal.hpp"
#include "client.hpp"
#include "scenario.hpp"

using namespace bench;

namespace {
std::vector<size_t> parse_payloads_or_default(int argc, char** argv) {
    for (int i = 0; i < argc - 1; ++i) {
        if (std::string_view{argv[i]} == "--payload-sizes") {
            return config_detail::parse_size_list(argv[i + 1]);
        }
    }
    return {64, 128, 256, 512, 1024, 1472};
}
} // namespace

int main(int argc, char** argv) {
#ifdef EPH_USE_DPDK
    std::fprintf(stderr,
        "[bench_lat_udp_dpdk] WARNING: DPDK transport not yet implemented; "
        "this binary currently uses POSIX sockets identical to bench_lat_udp.\n");
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
    if (auto pinned = pin_thread_strict(cfg.client_cpu, "bench_udp", policy); !pinned) {
        spdlog::error("pin_thread_strict failed: {}", pinned.error());
        return 1;
    }

    auto ep = udp::open_udp(cfg.server_ip, cfg.server_port);
    if (!ep) { spdlog::error("open_udp failed: {}", ep.error()); return 1; }
    spdlog::info("bench_udp: peer {}:{}", cfg.server_ip, cfg.server_port);

    auto payloads = parse_payloads_or_default(argc, argv);
    udp::UdpRttScenario scenario{*ep};
    BenchRunner runner{cfg, "udp", kTransport};
    runner.run_rtt_sweep(scenario, std::span<const size_t>(payloads));

    udp::close_endpoint(*ep);
    return 0;
}
