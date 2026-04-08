/// @file bench_tcp_echo.cpp
/// Raw TCP echo latency benchmark — sync send-recv with TSC stamps,
/// fixed-size message framing.
///
/// Compiled twice by xmake:
///   - bench_tcp_echo:      kernel SocketTransport path
///   - bench_tcp_echo_dpdk: DPDK TcpSession path

#include <ctime>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "framework/bench_config.hpp"
#include "framework/bench_runner.hpp"
#include "framework/bench_stats.hpp"
#include "framework/signal.hpp"
#include "framework/tcp_io.hpp"
#include "scenario/tcp_echo.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
#include "framework/dpdk_setup.hpp"
#include "mock/tcp_echo_server.hpp"
#endif

int main(int argc, char** argv) {
    bench::install_signal_handlers();
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

#if defined(EPH_USE_DPDK)
    // ── DPDK mode ───────────────────────────────────────────────────────
    auto env_result = bench::DpdkBenchEnv::create(argc, argv);
    if (!env_result) { spdlog::error("{}", env_result.error()); return 1; }
    auto& env = *env_result;
    auto& cfg = env.cfg;
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kTcpPayloads;

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::JsonlWriter jsonl(cfg.output_path);

    // Randomize base src_port to avoid TIME_WAIT collisions across runs.
    uint16_t src_port_base = static_cast<uint16_t>(
        40000 + (static_cast<unsigned>(getpid()) +
                 static_cast<unsigned>(std::time(nullptr))) % 20000);

    int iter = 0;
    for (size_t payload : cfg.payload_sizes) {
        // Per-payload mock (msg_size is fixed-size framing)
        auto mock = bench::mock::start_tcp_echo(
            {.bind_ip = cfg.server_ip, .port = cfg.server_port,
             .msg_size = payload}, cfg.mock_cpu);

        // New TcpSession with rotating src_port
        eph::dpdk::TcpConfig tcp_cfg{
            .tuple = {
                .src_ip = env.src_ip, .dst_ip = env.dst_ip,
                .src_port = static_cast<uint16_t>(src_port_base + iter),
                .dst_port = cfg.server_port,
            },
            .src_mac = env.src_mac, .dst_mac = env.gw_mac,
            .port_id = env.port_id,
        };
        ++iter;

        eph::dpdk::TcpSession session{tcp_cfg, env.mempool()};
        auto conn = session.connect(std::chrono::milliseconds{3000});
        if (!conn) {
            spdlog::error("DPDK TCP connect failed: {}", conn.error());
            bench::stop_mock(mock);
            continue;
        }

        bench::DpdkTcpStream stream{session,
            bench::ns_to_cycles(2'000'000'000ULL)};
        bench::scenario::TcpEchoScenario scenario{stream};
        bench::BenchRunner runner{cfg, "tcp_echo", "dpdk", jsonl};
        runner.run_sweep(scenario, std::vector<size_t>{payload});

        session.close();
        bench::stop_mock(mock);
    }
    return 0;

#else
    // ── Kernel mode ─────────────────────────────────────────────────────
    auto cfg = bench::parse_bench_config(argc, argv);
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kTcpPayloads;
    if (cfg.server_ip.empty()) {
        spdlog::error("--server-ip is required");
        return 1;
    }

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::JsonlWriter jsonl(cfg.output_path);

    // Note: kernel TCP needs a fresh connection per payload because the
    // mock side reads exactly `msg_size` bytes per message, and the mock
    // server is started per payload by bench_latency.sh.
    for (size_t payload : cfg.payload_sizes) {
        bench::KernelTcpStream stream{cfg.server_ip, cfg.server_port};
        if (!stream.valid()) continue;

        bench::scenario::TcpEchoScenario scenario{stream};
        bench::BenchRunner runner{cfg, "tcp_echo", "kernel", jsonl};
        runner.run_sweep(scenario, std::vector<size_t>{payload});
    }
    return 0;
#endif
}
