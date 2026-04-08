/// @file bench_tcp_echo_server.cpp
/// Standalone TCP echo server binary — thin wrapper around mock/tcp_echo_server.hpp.
///
/// Used by bench_latency.sh to run a kernel TCP echo server in the host
/// namespace while bench_tcp_echo runs in a network namespace.
///
/// Usage: bench_tcp_echo_server --bind-ip IP [--port PORT] [--msg-size N] [--cpu N]

#include <atomic>
#include <csignal>
#include <string>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/time.hpp"
#include "mock/tcp_echo_server.hpp"

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running.store(false, std::memory_order_relaxed); }

int main(int argc, char** argv) {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    std::string bind_ip;
    uint16_t port = 9998;
    size_t msg_size = 64;
    int cpu = 4;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--bind-ip" && i + 1 < argc) bind_ip = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--msg-size" && i + 1 < argc) msg_size = static_cast<size_t>(std::stoi(argv[++i]));
        else if (arg == "--cpu" && i + 1 < argc) cpu = std::stoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            spdlog::info("Usage: bench_tcp_echo_server --bind-ip IP [--port PORT] "
                         "[--msg-size N] [--cpu N]");
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

    if (auto r = eph::utils::set_thread_affinity(cpu, "tcp-echo"); !r) {
        spdlog::warn("Failed to pin to core {}: {}", cpu, r.error());
    } else {
        spdlog::info("Pinned to core {}", cpu);
    }

    bench::mock::run_tcp_echo_server(
        {.bind_ip = bind_ip, .port = port, .msg_size = msg_size}, g_running);
    return 0;
}
