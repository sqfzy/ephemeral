/// @file bench_udp_relay.cpp
/// UDP send-receive latency benchmark — same fair pattern as `udp_echo`,
/// kept as a separate scenario so its historical bench data and the
/// "client → relay → client" naming continues to make sense.
///
/// Topology: simple echo on port 9996.
///
///   Kernel mode :  bench client uses one kernel UDP socket on ens35,
///                  the standalone bench_udp_echo_server (port 9996)
///                  receives, stamps server_recv/server_send TSCs into
///                  the payload, sendto's it back.
///   DPDK mode   :  bench client uses an eph-dpdk UdpSender + an
///                  in-process udp_echo_server thread on NIC-A. Same
///                  protocol, same wire path.
///
/// Why this pattern (and not the previous "store-and-forward to a different
/// port" relay)?
///
/// The ONLY thing this benchmark is supposed to isolate is the cost of
/// the *client-side* UDP stack — DPDK PMD vs `sendto/recvfrom`. Everything
/// else (NIC hardware, Nitro fabric, server process, kernel UDP path on
/// the server) is held identical between the two runs. The previous
/// "two-port store-and-forward" wiring added a second socket on the
/// client and a forwarding hop on the server, which buried the variable
/// we actually care about under noise from extra socket/queue work.
///
/// The simplified single-port echo is the same fair-comparison shape as
/// `udp_echo` — see `bench_udp_echo.cpp` for the canonical version. The
/// resulting RTT difference between kernel and DPDK runs decomposes
/// cleanly into:
///   RTT_advantage  ≈  TX_advantage + RX_advantage
/// because server processing is identical (same mock, same kernel sockets).

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
// Distinct from udp_echo's 9997 so the two scenarios can be launched
// concurrently if needed and so the bench HISTORY.md keeps udp_relay's
// data separable from udp_echo's by the destination port alone.
constexpr uint16_t kRelayServerPort = 9996;
constexpr uint16_t kDpdkLocalPort   = 55556;
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

    // In-process UDP echo mock on NIC-A (kernel sockets, kRelayServerPort)
    auto mock = bench::mock::start_udp_echo(
        {.bind_ip = cfg.server_ip, .port = kRelayServerPort}, cfg.mock_cpu);

    // DPDK UDP sender targeting the echo server, plus a receiver on the
    // same DPDK port that filters by our local source port.
    auto sender = env.make_udp_sender(kDpdkLocalPort, kRelayServerPort);
    if (!sender) {
        spdlog::error("UdpSender create failed: {}", sender.error());
        bench::stop_mock(mock);
        return 1;
    }
    bench::DpdkUdpSenderAdapter sender_adapter{*sender};
    bench::DpdkUdpReceiver receiver{
        env.port_id, /*queue=*/0, kDpdkLocalPort,
        bench::ns_to_cycles(2'000'000'000ULL),  // 2s timeout
    };

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::UdpEchoScenario scenario{sender_adapter, receiver};
    bench::BenchRunner runner{cfg, "udp_relay", "dpdk", jsonl};
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
    // Single-socket UDP echo client. The kernel auto-assigns an ephemeral
    // source port; the echo server's sendto() returns to that port.
    bench::KernelUdpSocket sock{cfg.server_ip, kRelayServerPort};
    if (!sock.valid()) return 1;

    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::UdpEchoScenario scenario{sock, sock};
    bench::BenchRunner runner{cfg, "udp_relay", "kernel", jsonl};
    runner.run_sweep(scenario, cfg.payload_sizes);
    return 0;
#endif
}
