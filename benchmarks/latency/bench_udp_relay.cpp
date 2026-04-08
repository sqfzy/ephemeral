/// @file bench_udp_relay.cpp
/// UDP store-and-forward relay latency benchmark.
///
/// Topology:
///   Client (port C) → Relay (port A) → Client (port B)
///
/// Kernel mode: relay runs as external bench_udp_relay_server (started
/// by bench_latency.sh in host namespace).
/// DPDK mode:   relay runs in-process via mock/udp_relay_server.hpp on
/// kernel socket NIC-A; bench client uses DPDK on NIC-B.

#include <spdlog/spdlog.h>

#include "framework/bench_config.hpp"
#include "framework/bench_runner.hpp"
#include "framework/bench_stats.hpp"
#include "framework/signal.hpp"
#include "framework/udp_io.hpp"
#include "scenario/udp_relay.hpp"
#include "eph/utils/time.hpp"

#if defined(EPH_USE_DPDK)
#include "framework/dpdk_setup.hpp"
#include "mock/udp_relay_server.hpp"
#endif

namespace {
constexpr uint16_t kRelayListenPort = 9996;  // bench client → relay
constexpr uint16_t kClientRecvPort  = 9995;  // relay → bench client
constexpr uint16_t kDpdkLocalPort   = 55556; // DPDK local UDP port
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
    if (!env_result) { spdlog::error("{}", env_result.error()); return 1; }
    auto& env = *env_result;
    auto& cfg = env.cfg;
    if (cfg.payload_sizes.empty()) cfg.payload_sizes = bench::kUdpPayloads;

    // In-process relay (kernel socket on NIC-A): listens on server_ip,
    // forwards to local_ip (NIC-B, where DPDK is bound).
    auto relay_mock = bench::mock::start_udp_relay({
        .bind_ip = cfg.server_ip,
        .listen_port = kRelayListenPort,
        .forward_ip = cfg.local_ip,
        .forward_port = kClientRecvPort,
    }, cfg.mock_cpu);

    // DPDK sender targeting the relay's listen port
    auto sender = env.make_udp_sender(kDpdkLocalPort, kRelayListenPort);
    if (!sender) {
        spdlog::error("UdpSender create failed: {}", sender.error());
        bench::stop_mock(relay_mock);
        return 1;
    }
    bench::DpdkUdpSenderAdapter sender_adapter{*sender};

    // DPDK receiver listening for forwarded packets at kClientRecvPort
    bench::DpdkUdpReceiver receiver{
        env.port_id, /*queue=*/0, kClientRecvPort,
        bench::ns_to_cycles(2'000'000'000ULL),
    };

    bench::pin_or_die(cfg.poll_cpu, "bench-poll");
    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::UdpRelayScenario scenario{sender_adapter, receiver};
    bench::BenchRunner runner{cfg, "udp_relay", "dpdk", jsonl};
    runner.run_sweep(scenario, cfg.payload_sizes);

    bench::stop_mock(relay_mock);
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
    bench::KernelUdpRelayClient client{
        cfg.server_ip, kRelayListenPort, kClientRecvPort};
    if (!client.valid()) return 1;

    bench::JsonlWriter jsonl(cfg.output_path);
    bench::scenario::UdpRelayScenario scenario{client, client};
    bench::BenchRunner runner{cfg, "udp_relay", "kernel", jsonl};
    runner.run_sweep(scenario, cfg.payload_sizes);
    return 0;
#endif
}
