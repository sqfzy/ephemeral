/// @file bench_udp_echo.cpp
/// Raw UDP echo latency benchmark — sync send-recv with TSC stamps.
///
/// Compiled twice by xmake:
///   - bench_udp_echo:      kernel socket path
///   - bench_udp_echo_dpdk: DPDK UdpSender + rte_eth_rx_burst path
///
/// Usage (kernel):
///   bench_udp_echo --server-ip IP [--port PORT]
///       [--payload-sizes 64,128,512] [--duration N] [--warmup N]
///       [--poll-cpu N] [--output FILE.jsonl]
///
/// Usage (DPDK):
///   bench_udp_echo_dpdk [EAL args] -- --server-ip IP --local-ip IP
///       --gateway-ip IP [other options]

#include <spdlog/spdlog.h>

#include "framework/bench_config.hpp"
#include "framework/bench_runner.hpp"
#include "framework/bench_stats.hpp"
#include "framework/signal.hpp"
#include "framework/udp_io.hpp"
#include "scenario/udp_echo.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
#include "framework/dpdk_setup.hpp"
#include "mock/udp_echo_server.hpp"
#endif

namespace {
constexpr uint16_t kDpdkLocalUdpPort = 55555;
} // namespace

int main(int argc, char** argv) {
    bench::install_signal_handlers();
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

#if defined(EPH_USE_DPDK)
    // ── DPDK mode ───────────────────────────────────────────────────────
    auto env_result = bench::DpdkBenchEnv::create(argc, argv);
    if (!env_result) {
        spdlog::error("{}", env_result.error());
        return 1;
    }
    auto& env = *env_result;
    auto& cfg = env.cfg;
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kUdpPayloads;

    // In-process UDP echo mock on NIC-A (kernel socket)
    auto mock = bench::mock::start_udp_echo(
        {.bind_ip = cfg.server_ip, .port = cfg.server_port}, cfg.mock_cpu);

    // DPDK UDP sender + receiver
    auto sender = env.make_udp_sender(kDpdkLocalUdpPort, cfg.server_port);
    if (!sender) {
        spdlog::error("UdpSender create failed: {}", sender.error());
        bench::stop_mock(mock);
        return 1;
    }
    bench::DpdkUdpSenderAdapter sender_adapter{*sender};
    bench::DpdkUdpReceiver receiver{
        env.port_id, /*queue=*/0, kDpdkLocalUdpPort,
        bench::ns_to_cycles(2'000'000'000ULL),  // 2s timeout
    };

    // Run sweep
    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::UdpEchoScenario scenario{sender_adapter, receiver};
    bench::BenchRunner runner{cfg, "udp_echo", "dpdk", jsonl};
    runner.run_sweep(scenario, cfg.payload_sizes);

    bench::stop_mock(mock);
    return 0;

#else
    // ── Kernel mode ─────────────────────────────────────────────────────
    auto cfg = bench::parse_bench_config(argc, argv);
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kUdpPayloads;
    if (cfg.server_ip.empty()) {
        spdlog::error("--server-ip is required");
        return 1;
    }

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::KernelUdpSocket sock{cfg.server_ip, cfg.server_port};
    if (!sock.valid()) return 1;

    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::UdpEchoScenario scenario{sock, sock};
    bench::BenchRunner runner{cfg, "udp_echo", "kernel", jsonl};
    runner.run_sweep(scenario, cfg.payload_sizes);
    return 0;
#endif
}
