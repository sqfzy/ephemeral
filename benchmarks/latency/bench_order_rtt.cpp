/// @file bench_order_rtt.cpp
/// WS order submit + execution report RTT — synchronous version.
///
/// Sends order, polls until executionReport response arrives, records
/// 4-leg latency. Sync design (one in-flight order at a time).
///
/// Compiled twice by xmake: bench_order_rtt / bench_order_rtt_dpdk.

#include <spdlog/spdlog.h>

#include "framework/bench_config.hpp"
#include "framework/bench_runner.hpp"
#include "framework/bench_stats.hpp"
#include "framework/signal.hpp"
#include "framework/ws_transport.hpp"
#include "scenario/order_rtt.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
#include "framework/dpdk_setup.hpp"
#include "framework/ws_mock_helper.hpp"
#endif

int main(int argc, char** argv) {
    bench::install_signal_handlers();
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

#if defined(EPH_USE_DPDK)
    auto env_result = bench::DpdkBenchEnv::create(argc, argv);
    if (!env_result) { spdlog::error("{}", env_result.error()); return 1; }
    auto& env = *env_result;
    auto& cfg = env.cfg;

    // Order mock: pushes ticks AND responds to orders with executionReport.
    auto mock = bench::start_ws_mock(cfg, {.order_mode = true});

    auto conn = bench::ws_connect_dpdk(env, cfg);
    if (!conn) {
        spdlog::error("{}", conn.error());
        bench::stop_mock(mock);
        return 1;
    }

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::OrderRttScenario scenario{**conn};
    bench::BenchRunner runner{cfg, "order_rtt", "dpdk", jsonl};
    runner.run_single(scenario);

    (*conn)->stop();
    bench::stop_mock(mock);

#else
    auto cfg = bench::parse_bench_config(argc, argv);
    if (cfg.server_ip.empty()) {
        spdlog::error("--server-ip is required");
        return 1;
    }

    auto conn = bench::ws_connect_kernel(cfg);
    if (!conn) {
        spdlog::error("{}", conn.error());
        return 1;
    }

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::OrderRttScenario scenario{**conn};
    bench::BenchRunner runner{cfg, "order_rtt", "kernel", jsonl};
    runner.run_single(scenario);

    (*conn)->stop();
#endif
    return 0;
}
