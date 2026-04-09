/// @file exchange/bench_md_udp.cpp
/// bench_lat_exchange_md_udp / *_dpdk — UDP market data echo bench.

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
#include "../udp/client.hpp"
#include "scenario.hpp"
#endif

using namespace bench;

namespace {
std::vector<size_t> parse_payloads_or_default(int argc, char** argv) {
    for (int i = 0; i < argc - 1; ++i) {
        if (std::string_view{argv[i]} == "--payload-sizes") {
            return config_detail::parse_size_list(argv[i + 1]);
        }
    }
    return {64, 256, 1024, 1400};
}
} // namespace

int main(int argc, char** argv) {
    install_signal_handlers();

#if defined(EPH_USE_DPDK)
    constexpr const char* kTransport = "dpdk";
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
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_ex_md", policy); !p) {
        spdlog::error("pin: {}", p.error()); return 1;
    }

    constexpr uint16_t kLocalPort = 55550;
    auto sender = env->make_udp_sender(kLocalPort, cfg.server_port);
    if (!sender) { spdlog::error("UdpSender: {}", sender.error()); return 1; }
    spdlog::info("bench_lat_exchange_md_udp (dpdk): peer {}:{}",
                 cfg.server_ip, cfg.server_port);

    auto payloads = parse_payloads_or_default(argc, argv);
    exchange::MdUdpDpdkScenario scenario{*sender, env->port_id, 0, kLocalPort};
    BenchRunner runner{cfg, "exchange/md_udp", kTransport};
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
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_ex_md", policy); !p) {
        spdlog::error("pin: {}", p.error()); return 1;
    }

    auto ep = udp::open_udp(cfg.server_ip, cfg.server_port);
    if (!ep) { spdlog::error("open_udp: {}", ep.error()); return 1; }
    spdlog::info("bench_lat_exchange_md_udp: peer {}:{}", cfg.server_ip, cfg.server_port);

    auto payloads = parse_payloads_or_default(argc, argv);
    exchange::MdUdpScenario scenario{*ep};
    BenchRunner runner{cfg, "exchange/md_udp", kTransport};
    runner.run_rtt_sweep(scenario, std::span<const size_t>(payloads));
    udp::close_endpoint(*ep);
#endif
    return 0;
}
