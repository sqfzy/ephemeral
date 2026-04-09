/// @file tcp/bench.cpp
/// bench_tcp / bench_tcp_dpdk — TCP RTT bench client.
///
/// Usage:
///   ./bench_tcp --server-ip IP --port PORT --client-cpu N
///     --duration 10 --warmup 2
///     --payload-sizes 64,128,256,512,1024,1460,4096,16384
///     [--allow-non-isolated]
///
/// DPDK build note: this binary currently uses POSIX sockets in both
/// kernel and EPH_USE_DPDK builds. A real DPDK transport requires EAL
/// init, manual ARP/MAC discovery, and a userspace TCP stack — substantial
/// work scoped out of stage 3 of the bench rewrite plan. The
/// bench_tcp_dpdk target ships now so the matrix is complete and so the
/// orchestration script can wire everything up.

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
    return {64, 128, 256, 512, 1024, 1460, 4096, 16384};
}
} // namespace

int main(int argc, char** argv) {
#ifdef EPH_USE_DPDK
    std::fprintf(stderr,
        "[bench_lat_tcp_dpdk] WARNING: DPDK transport not yet implemented; "
        "this binary currently uses POSIX sockets identical to bench_lat_tcp. "
        "See benchmarks/latency/tcp/bench.cpp header for details.\n");
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
    if (auto pinned = pin_thread_strict(cfg.client_cpu, "bench_tcp", policy); !pinned) {
        spdlog::error("pin_thread_strict failed: {}", pinned.error());
        return 1;
    }

    auto fd = tcp::connect_tcp(cfg.server_ip, cfg.server_port);
    if (!fd) { spdlog::error("connect failed: {}", fd.error()); return 1; }
    spdlog::info("bench_tcp: connected to {}:{}", cfg.server_ip, cfg.server_port);

    auto payloads = parse_payloads_or_default(argc, argv);
    if (payloads.empty()) {
        spdlog::error("payload sweep is empty");
        ::close(*fd);
        return 1;
    }

    tcp::TcpRttScenario scenario{*fd};
    BenchRunner runner{cfg, "tcp", kTransport};
    runner.run_rtt_sweep(scenario, std::span<const size_t>(payloads));

    ::close(*fd);
    return 0;
}
