/// @file bench_udp_echo_server.cpp
/// Standalone UDP echo server binary — thin wrapper around mock/udp_echo_server.hpp.
///
/// Used by bench_latency.sh to run a kernel UDP echo server in the host
/// namespace while bench_udp_echo runs in a network namespace.
///
/// Usage: bench_udp_echo_server --bind-ip IP [--port PORT] [--cpu N]

#include <atomic>
#include <csignal>
#include <string>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/time.hpp"
#include "mock/udp_echo_server.hpp"

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running.store(false, std::memory_order_relaxed); }

int main(int argc, char** argv) {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    std::string bind_ip;
    uint16_t port = 9997;
    int cpu = 4;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bind-ip" && i + 1 < argc) bind_ip = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--cpu" && i + 1 < argc) cpu = std::stoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            spdlog::info("Usage: bench_udp_echo_server --bind-ip IP [--port PORT] [--cpu N]");
            return 0;
        }
    }

    if (bind_ip.empty()) {
        spdlog::error("--bind-ip is required");
        return 1;
    }

    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    if (auto r = eph::utils::set_thread_affinity(cpu, "udp-echo"); !r) {
        spdlog::warn("Failed to pin to core {}: {}", cpu, r.error());
    } else {
        spdlog::info("Pinned to core {}", cpu);
    }

    bench::mock::run_udp_echo_server({.bind_ip = bind_ip, .port = port}, g_running);
    return 0;
}
