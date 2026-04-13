#pragma once

/// @file mock_dispatcher.hpp
/// Multi-threaded kernel mock dispatcher for the DPDK E2E test suite.
///
/// Run as the entry point of the test main()'s forked child process.
/// Spawns one thread per port (TCP echo, UDP echo, RST, FIN, WS echo,
/// plus N reactor echo ports).  Catches SIGTERM/SIGINT, flips
/// `g_running` to false, joins all threads, exits 0.
///
/// All mocks bind to `cfg.server_ip` (the SERVER_IP from bench.conf,
/// which lives on NIC_A in the host network namespace).  This is the
/// same end of the wire that lat_*_dpdk's child mock binds to — so the
/// DPDK client running in the parent process will reach them via NIC_B.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "echo_mocks.hpp"

namespace eph::dpdk::test_e2e {

// ─────────────────────────────────────────────────────────────────────────
// Port allocation — keep these in sync with `dpdk_e2e_ports.hpp` semantics.
// Picked above 19000 to avoid colliding with bench.conf scenario ports
// or any commonly bound service.
// ─────────────────────────────────────────────────────────────────────────
inline constexpr uint16_t kTcpEchoPort     = 19001;
inline constexpr uint16_t kTcpRstPort      = 19002;
inline constexpr uint16_t kTcpFinPort      = 19003;
inline constexpr uint16_t kUdpEchoPort     = 19101;
inline constexpr uint16_t kWsEchoPort      = 19201;
inline constexpr uint16_t kRxDispatcherPortBase = 19301;  ///< 19301..19308
inline constexpr int      kRxDispatcherConns    = 8;

// Process-wide running flag.  Threads poll it; signal handler flips it.
inline std::atomic<bool> g_dispatcher_running{true};

inline void dispatcher_signal_handler(int /*sig*/) noexcept {
    g_dispatcher_running.store(false, std::memory_order_release);
}

/// Child-process entry point.  Spawns mock threads, waits for SIGTERM,
/// joins, exits.  Returns the exit code for the child process.
inline int run_mock_dispatcher(const std::string& server_ip) noexcept {
    // Install signal handlers so the parent can SIGTERM us cleanly.
    struct sigaction sa{};
    sa.sa_handler = dispatcher_signal_handler;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT,  &sa, nullptr);

    spdlog::info("dispatcher: starting mocks on {}", server_ip);

    std::vector<std::thread> threads;
    threads.reserve(5 + kRxDispatcherConns);

    threads.emplace_back([&] {
        tcp_echo_mock_thread(server_ip, kTcpEchoPort, g_dispatcher_running);
    });
    threads.emplace_back([&] {
        tcp_rst_mock_thread(server_ip, kTcpRstPort, g_dispatcher_running);
    });
    threads.emplace_back([&] {
        tcp_fin_mock_thread(server_ip, kTcpFinPort, g_dispatcher_running);
    });
    threads.emplace_back([&] {
        udp_echo_mock_thread(server_ip, kUdpEchoPort, g_dispatcher_running);
    });
    threads.emplace_back([&] {
        ws_echo_mock_thread(server_ip, kWsEchoPort, g_dispatcher_running);
    });

    // RxDispatcher multi-conn mock — N parallel TCP echo servers on contiguous ports.
    for (int i = 0; i < kRxDispatcherConns; ++i) {
        uint16_t port = static_cast<uint16_t>(kRxDispatcherPortBase + i);
        threads.emplace_back([server_ip, port] {
            tcp_echo_mock_thread(server_ip, port, g_dispatcher_running);
        });
    }

    spdlog::info("dispatcher: {} mock threads running, awaiting SIGTERM",
                 threads.size());

    // Block until signal handler flips g_dispatcher_running.
    while (g_dispatcher_running.load(std::memory_order_acquire)) {
        // sigsuspend with empty mask blocks until any signal arrives.
        sigset_t mask;
        sigemptyset(&mask);
        ::sigsuspend(&mask);  // returns -1 EINTR; loop checks the flag
    }

    // Threads may be blocked in inner recv loops on a half-open client
    // socket; rather than wire SIGTERM through every blocking call, we
    // just _exit() the entire child process.  Process death tears all
    // threads down deterministically — std::thread::detach() prevents
    // their destructors from std::terminate()ing.
    spdlog::info("dispatcher: shutdown signaled, _exit(0)");
    for (auto& t : threads) t.detach();
    ::_exit(0);
}

} // namespace eph::dpdk::test_e2e
