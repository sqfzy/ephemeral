/// @file exchange/bench_order.cpp
/// bench_lat_exchange_order / *_dpdk — order RTT bench with N inflight.

#include <array>
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
#include "eph/utils/cpu_pin.hpp"
#include "../core/runner.hpp"
#include "../core/signal.hpp"

#if defined(EPH_USE_DPDK)
#include "../core/dpdk_env.hpp"
#include "../ws/dpdk_scenario.hpp"
#include "dpdk_scenario.hpp"
#else
#include "../ws/client.hpp"
#include "scenario.hpp"
#endif

using namespace bench;

// Order inflight sweep: 1 = synchronous baseline, 4/16/64 = pipelined.
static constexpr std::array<int, 4> kDefaultInflights{1, 4, 16, 64};

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
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_ex_or", policy); !p) {
        spdlog::error("pin: {}", p.error()); return 1;
    }

    static uint16_t src_port = 55900;
    auto tcfg = env->make_tcp_config(src_port++, cfg.server_port);
    eph::dpdk::TcpSession<> session{tcfg, env->pool};
    if (auto r = session.connect(std::chrono::seconds{3}); !r) {
        spdlog::error("TCP connect: {}", r.error()); return 1;
    }
    std::string host = cfg.server_ip + ":" + std::to_string(cfg.server_port);
    if (auto h = ws::dpdk_ws_handshake(session, host); !h) {
        spdlog::error("WS handshake: {}", h.error()); return 1;
    }
    spdlog::info("bench_lat_exchange_order (dpdk): connected to {}:{}",
                 cfg.server_ip, cfg.server_port);

    auto inflights = effective_inflights(cfg, kDefaultInflights);
    exchange::OrderRttDpdkScenario scenario{session};
    BenchRunner runner{cfg, "exchange/order", kTransport};
    runner.run_rtt_inflight_sweep(scenario, std::span<const int>(inflights));

    (void)session.close();

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
    if (auto p = eph::utils::pin_thread_strict(cfg.client_cpu, "bench_ex_or", policy); !p) {
        spdlog::error("pin: {}", p.error()); return 1;
    }

    auto fd = ws::connect_ws(cfg.server_ip, cfg.server_port);
    if (!fd) { spdlog::error("connect_ws: {}", fd.error()); return 1; }
    spdlog::info("bench_lat_exchange_order: connected to {}:{}",
                 cfg.server_ip, cfg.server_port);

    auto inflights = effective_inflights(cfg, kDefaultInflights);
    exchange::OrderRttScenario scenario{*fd};
    BenchRunner runner{cfg, "exchange/order", kTransport};
    runner.run_rtt_inflight_sweep(scenario, std::span<const int>(inflights));
    ::close(*fd);
#endif
    return 0;
}
