#pragma once

/// @file mock_dispatcher.hpp
/// Multi-threaded kernel mock dispatcher for the DPDK E2E test suite.
///
/// Run as the entry point of the test main()'s forked child process.
/// Spawns one thread per port (TCP echo, UDP echo, RST, FIN, WS echo,
/// DNS responder, plus N reactor echo ports).  Catches SIGTERM/SIGINT,
/// flips `g_running` to false, joins all threads, exits 0.
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

#include <sys/prctl.h>

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
inline constexpr uint16_t kWsPingPort      = 19202;
/// Non-standard DNS port — port 53 would force the test binary to run
/// with CAP_NET_BIND_SERVICE and risks colliding with systemd-resolved.
/// `dns::resolve()` accepts an arbitrary port via `DnsConfig::port`.
inline constexpr uint16_t kDnsMockPort          = 19401;

// Lock the "above 19000, below 20000" invariant at compile time so an
// accidental edit that lowered a port back into the ephemeral range
// (≥ 32768 on Linux) or the privileged range (< 1024) cannot slip in
// without being noticed. Tightening to < 20000 also keeps the block
// contiguous and reserved — see bench.conf for the overlapping ranges.
static_assert(kTcpEchoPort    >= 19000 && kTcpEchoPort    < 20000);
static_assert(kTcpRstPort     >= 19000 && kTcpRstPort     < 20000);
static_assert(kTcpFinPort     >= 19000 && kTcpFinPort     < 20000);
static_assert(kUdpEchoPort    >= 19000 && kUdpEchoPort    < 20000);
static_assert(kWsEchoPort     >= 19000 && kWsEchoPort     < 20000);
static_assert(kWsPingPort     >= 19000 && kWsPingPort     < 20000);
static_assert(kDnsMockPort    >= 19000 && kDnsMockPort    < 20000);
/// IP that the DNS mock answers with for every A query.  192.0.2.42 is
/// in TEST-NET-1 (RFC 5737) — unambiguously synthetic, won't be
/// confused with real production data.
inline constexpr uint32_t kDnsMockResolvedIp    = 0xc000022aU;

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

    // Parent-death watchdog: if the parent test process exits abnormally
    // (crash / SIGKILL / forgotten ChildReaper), the kernel automatically
    // delivers SIGTERM to us, which our handler converts into a clean
    // shutdown via g_dispatcher_running. Without this, an orphaned mock
    // process can survive for hours holding ports 19000-19499 (observed
    // in the ephemeral_dev autonomous-loop infrastructure: a stuck
    // test_dpdk_rss_fanout child sat in sigsuspend for 4+ hours after
    // its parent died, blocking subsequent integration runs).
    //
    // PR_SET_PDEATHSIG is per-thread on Linux; we set it on the main
    // thread before spawning workers so the workers inherit nothing —
    // only the main thread (which calls sigsuspend below) needs the
    // signal. Errors are swallowed: this is a defensive watchdog, not
    // a hard requirement, and the existing ChildReaper still covers
    // the happy path.
    (void)::prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);

    SPDLOG_INFO("dispatcher: starting mocks on {}", server_ip);

    std::vector<std::thread> threads;
    threads.reserve(7);

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
    threads.emplace_back([&] {
        ws_server_ping_mock_thread(server_ip, kWsPingPort, g_dispatcher_running);
    });
    threads.emplace_back([&] {
        dns_mock_thread(server_ip, kDnsMockPort, kDnsMockResolvedIp,
                        g_dispatcher_running);
    });

    SPDLOG_INFO("dispatcher: {} mock threads running, awaiting SIGTERM",
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
    SPDLOG_INFO("dispatcher: shutdown signaled, _exit(0)");
    for (auto& t : threads) t.detach();
    ::_exit(0);
}

} // namespace eph::dpdk::test_e2e
