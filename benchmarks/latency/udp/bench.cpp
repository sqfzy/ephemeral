/// @file udp/bench.cpp
/// bench_lat_udp / bench_lat_udp_dpdk — UDP RTT bench client.

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "eph/utils/cpu_pin.hpp"
#include "../core/runner.hpp"
#include "../core/signal.hpp"

#if defined(EPH_USE_DPDK)
#include "../core/dpdk_env.hpp"
#include "dpdk_scenario.hpp"
#else
#include "client.hpp"
#include "scenario.hpp"
#endif

using namespace bench;

// UDP payloads: 64..1472 (1472 = 1500 MTU - IP(20) - UDP(8)).
// UDP never fragments above this, so larger values would be nonsense.
static constexpr std::array<size_t, 6> kDefaultPayloads{
    64, 128, 256, 512, 1024, 1472
};

int main(int argc, char** argv) {
    install_signal_handlers();

#if defined(EPH_USE_DPDK)
    constexpr const char* kTransport = "dpdk";

    // Parse DpdkConfig from the post-"--" portion of argv.
    // CommonConfig is also parsed from the full argv (unrecognized flags
    // like EAL -a/-l are silently skipped by parse_common).
    auto cfg = parse_common(argc, argv);
    auto dcfg = parse_dpdk(argc, argv);

    if (cfg.server_ip.empty() || cfg.server_port == 0) {
        spdlog::error("--server-ip and --port are required"); return 1;
    }
    if (dcfg.local_ip.empty() || dcfg.gateway_ip.empty()) {
        spdlog::error("--local-ip and --gateway-ip are required for DPDK"); return 1;
    }
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed"); return 1;
    }

    auto env = DpdkBenchEnv::create_full(argc, argv,
        cfg.server_ip, dcfg.local_ip, dcfg.gateway_ip, dcfg.dpdk_port_id);
    if (!env) { spdlog::error("DPDK env: {}", env.error()); return 1; }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_udp", policy); !p) {
        spdlog::error("pin_thread_strict: {}", p.error()); return 1;
    }

    constexpr uint16_t kLocalPort = 55500;
    auto sender = env->make_udp_sender(kLocalPort, cfg.server_port);
    if (!sender) { spdlog::error("UdpSender: {}", sender.error()); return 1; }
    spdlog::info("bench_udp (dpdk): peer {}:{}", cfg.server_ip, cfg.server_port);

    auto payloads = effective_payloads(cfg, kDefaultPayloads);
    udp::UdpDpdkRttScenario scenario{*sender, env->port_id, 0, kLocalPort};
    BenchRunner runner{cfg, "udp", kTransport};
    runner.run_rtt_sweep(scenario, std::span<const size_t>(payloads));

#else
    constexpr const char* kTransport = "kernel";
    auto cfg = parse_common(argc, argv);
    if (cfg.server_ip.empty() || cfg.server_port == 0) {
        spdlog::error("--server-ip and --port are required"); return 1;
    }
    if (cfg.client_cpu < 0) {
        spdlog::error("--client-cpu <cpu> is required"); return 1;
    }
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed"); return 1;
    }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_udp", policy); !p) {
        spdlog::error("pin_thread_strict: {}", p.error()); return 1;
    }

    auto ep = udp::open_udp(cfg.server_ip, cfg.server_port);
    if (!ep) { spdlog::error("open_udp: {}", ep.error()); return 1; }
    spdlog::info("bench_udp: peer {}:{}", cfg.server_ip, cfg.server_port);

    auto payloads = effective_payloads(cfg, kDefaultPayloads);
    udp::UdpRttScenario scenario{*ep};
    BenchRunner runner{cfg, "udp", kTransport};
    runner.run_rtt_sweep(scenario, std::span<const size_t>(payloads));
    udp::close_endpoint(*ep);
#endif
    return 0;
}
