/// @file bench_udp_relay_server.cpp
/// Standalone UDP relay server binary — thin wrapper around mock/udp_relay_server.hpp.
///
/// Used by bench_latency.sh to run a UDP relay in the host namespace while
/// bench_udp_relay runs in a network namespace (kernel mode).
///
/// Usage: bench_udp_relay_server --bind-ip IP --forward-ip IP
///            [--listen-port PORT] [--forward-port PORT] [--cpu N]

#include <atomic>
#include <csignal>
#include <string>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/time.hpp"
#include "mock/udp_relay_server.hpp"

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running.store(false, std::memory_order_relaxed); }

int main(int argc, char** argv) {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    std::string bind_ip, forward_ip;
    uint16_t listen_port = 9996;
    uint16_t forward_port = 9995;
    int cpu = 4;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bind-ip" && i + 1 < argc) bind_ip = argv[++i];
        else if (arg == "--forward-ip" && i + 1 < argc) forward_ip = argv[++i];
        else if (arg == "--listen-port" && i + 1 < argc) listen_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--forward-port" && i + 1 < argc) forward_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--cpu" && i + 1 < argc) cpu = std::stoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            spdlog::info("Usage: bench_udp_relay_server --bind-ip IP --forward-ip IP "
                         "[--listen-port PORT] [--forward-port PORT] [--cpu N]");
            return 0;
        }
    }

    if (bind_ip.empty() || forward_ip.empty()) {
        spdlog::error("--bind-ip and --forward-ip are required");
        return 1;
    }

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    if (auto r = eph::utils::set_thread_affinity(cpu, "udp-relay"); !r) {
        spdlog::warn("Failed to pin to core {}: {}", cpu, r.error());
    } else {
        spdlog::info("Pinned to core {}", cpu);
    }

    bench::mock::run_udp_relay_server(
        {.bind_ip = bind_ip, .listen_port = listen_port,
         .forward_ip = forward_ip, .forward_port = forward_port},
        g_running);
    return 0;
}
