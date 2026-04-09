/// @file udp/mock.cpp
/// mock_udp / mock_udp_dpdk — UDP echo server with TSC stamping + work_spin.
///
/// Hot loop:
///   recvfrom (blocking, MSG_DONTWAIT polled in a loop) → stamp recv →
///   work_spin → stamp send → sendto. The peer address comes from the
///   incoming datagram so the same socket can serve transient clients.
///
/// DPDK build note: same as tcp/mock.cpp — POSIX path with stderr warning.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "eph/utils/cpu_pin.hpp"
#include "../core/signal.hpp"
#include "../core/tsc_protocol.hpp"
#include "../core/socket_bind.hpp"
#include "eph/utils/cpu.hpp"

using namespace bench;

int main(int argc, char** argv) {
#ifdef EPH_USE_DPDK
    std::fprintf(stderr,
        "[mock_lat_udp_dpdk] WARNING: DPDK transport not yet implemented; "
        "this binary currently uses POSIX sockets identical to mock_lat_udp. "
        "See benchmarks/latency/udp/mock.cpp header for details.\n");
#endif

    install_signal_handlers();
    auto cfg = parse_common(argc, argv);

    if (cfg.server_ip.empty() || cfg.server_port == 0) {
        spdlog::error("--server-ip and --port are required");
        return 1;
    }
    if (cfg.mock_cpu < 0) {
        spdlog::error("--mock-cpu <cpu> is required");
        return 1;
    }
    if (!eph::utils::TSC::init()) {
        spdlog::error("TSC calibration failed");
        return 1;
    }

    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto pinned = eph::utils::pin_thread_strict(cfg.mock_cpu, "mock_udp", policy); !pinned) {
        spdlog::error("pin_thread_strict failed: {}", pinned.error());
        return 1;
    }

    auto fd = bench::udp_bind(cfg.server_ip, cfg.server_port);
    if (!fd) { spdlog::error("{}", fd.error()); return 1; }

    spdlog::info("mock_udp: bound {}:{} work_ns={}",
                 cfg.server_ip, cfg.server_port, cfg.server_work_ns);

    constexpr size_t kMaxPayload = 65536;
    std::vector<uint8_t> buf(kMaxPayload);

    while (g_running.load(std::memory_order_acquire)) {
        // Poll-driven shutdown so SIGTERM interrupts even when no packets
        // are arriving.
        pollfd p{}; p.fd = *fd; p.events = POLLIN;
        int rv = ::poll(&p, 1, 100);
        if (rv < 0) {
            if (errno == EINTR) continue;
            spdlog::error("mock_udp: poll failed: {}", std::strerror(errno));
            break;
        }
        if (rv == 0) continue;

        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        ssize_t n = ::recvfrom(*fd, buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&src), &slen);
        if (n < static_cast<ssize_t>(tsc::kBinaryHeaderSize)) continue;

        uint64_t recv_tsc = eph::utils::TSC::now();
        std::memcpy(buf.data() + 8, &recv_tsc, 8);
        eph::utils::spin_for_ns(cfg.server_work_ns);
        uint64_t send_tsc = eph::utils::TSC::now();
        std::memcpy(buf.data() + 16, &send_tsc, 8);

        ::sendto(*fd, buf.data(), n, 0,
                 reinterpret_cast<sockaddr*>(&src), slen);
    }
    ::close(*fd);
    return 0;
}
