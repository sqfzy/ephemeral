/// @file bench_market_rx.cpp
/// WS market data receive — 1-leg pipeline latency benchmark.
///
/// Server pushes bookTicker JSON at tick_interval; client measures
/// time from server stamp ("T" field) to client receive.
///
/// Compiled twice by xmake: bench_market_rx / bench_market_rx_dpdk.

#include <spdlog/spdlog.h>

#include "framework/bench_config.hpp"
#include "framework/bench_runner.hpp"
#include "framework/bench_stats.hpp"
#include "framework/signal.hpp"
#include "framework/ws_transport.hpp"
#include "scenario/market_rx.hpp"
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

    // Market mock: tick push enabled, no echo, no order responses.
    auto mock = bench::start_ws_mock(cfg, {});

    auto conn = bench::ws_connect_dpdk(env, cfg);
    if (!conn) {
        spdlog::error("{}", conn.error());
        bench::stop_mock(mock);
        return 1;
    }

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::MarketRxScenario scenario{**conn};
    bench::BenchRunner runner{cfg, "market_rx", "dpdk", jsonl};
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
    bench::scenario::MarketRxScenario scenario{**conn};
    bench::BenchRunner runner{cfg, "market_rx", "kernel", jsonl};
    runner.run_single(scenario);

    (*conn)->stop();
#endif
    return 0;
}
