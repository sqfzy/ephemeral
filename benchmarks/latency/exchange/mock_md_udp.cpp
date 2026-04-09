/// @file exchange/mock_md_udp.cpp
/// mock_lat_exchange_md_udp / *_dpdk — UDP echo with the same wire format
/// as mock_lat_udp. The only thing that differs from the raw UDP mock is
/// the binary name (so the bench script can address them separately) and
/// the labelled scenario in the report. The transport-level behavior is
/// identical: receive a 24-byte-prefixed datagram, stamp recv/send TSCs,
/// echo it back.

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
#include "../core/cpu_pin.hpp"
#include "../core/signal.hpp"
#include "../core/tsc_protocol.hpp"
#include "../mock/lib/udp_bind.hpp"
#include "../mock/lib/work_spin.hpp"

using namespace bench;

int main(int argc, char** argv) {
#ifdef EPH_USE_DPDK
    std::fprintf(stderr,
        "[mock_lat_exchange_md_udp_dpdk] WARNING: DPDK transport not yet "
        "implemented; this binary currently uses POSIX sockets identical "
        "to mock_lat_exchange_md_udp.\n");
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

    CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = pin_thread_strict(cfg.mock_cpu, "mock_ex_md", policy); !p) {
        spdlog::error("pin_thread_strict failed: {}", p.error());
        return 1;
    }

    auto fd = mock::udp_bind(cfg.server_ip, cfg.server_port);
    if (!fd) { spdlog::error("{}", fd.error()); return 1; }
    spdlog::info("mock_lat_exchange_md_udp: bound {}:{} work_ns={}",
                 cfg.server_ip, cfg.server_port, cfg.server_work_ns);

    constexpr size_t kMaxPayload = 65536;
    std::vector<uint8_t> buf(kMaxPayload);

    while (g_running.load(std::memory_order_acquire)) {
        pollfd p{}; p.fd = *fd; p.events = POLLIN;
        int rv = ::poll(&p, 1, 100);
        if (rv < 0) { if (errno == EINTR) continue; break; }
        if (rv == 0) continue;

        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        ssize_t n = ::recvfrom(*fd, buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&src), &slen);
        if (n < static_cast<ssize_t>(tsc::kBinaryHeaderSize)) continue;

        uint64_t recv_tsc = eph::utils::TSC::now();
        std::memcpy(buf.data() + 8, &recv_tsc, 8);
        bench::mock::work_spin(cfg.server_work_ns);
        uint64_t send_tsc = eph::utils::TSC::now();
        std::memcpy(buf.data() + 16, &send_tsc, 8);

        ::sendto(*fd, buf.data(), n, 0,
                 reinterpret_cast<sockaddr*>(&src), slen);
    }
    ::close(*fd);
    return 0;
}
