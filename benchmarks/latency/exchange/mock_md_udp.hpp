/// @file exchange/mock_md_udp.hpp
/// Exchange UDP market-data echo mock — same wire format as the raw UDP
/// mock (24-byte TSC header + payload). The separate target exists so the
/// report label says "exchange/md_udp" instead of "udp"; transport-level
/// behaviour is identical.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "eph/utils/cpu.hpp"
#include "eph/utils/cpu_pin.hpp"
#include "eph/utils/time.hpp"

#include "../core/config.hpp"
#include "../core/signal.hpp"
#include "../core/socket_bind.hpp"
#include "../core/tsc_protocol.hpp"

namespace bench::exchange {

inline int run_exchange_md_udp_mock(const BenchConfig& cfg) {
    eph::utils::CpuPinPolicy policy;
    if (cfg.allow_non_isolated) policy.require_isolcpus = false;
    if (auto p = eph::utils::pin_thread_strict(cfg.mock_cpu, "mock_lat_ex_md", policy); !p) {
        spdlog::error("mock_lat_ex_md: pin failed: {}", p.error());
        return 1;
    }

    auto fd = bench::udp_bind(cfg.server_ip, cfg.server_port);
    if (!fd) { spdlog::error("mock_lat_ex_md: {}", fd.error()); return 1; }
    spdlog::info("mock_lat_ex_md: bound {}:{} work_ns={}",
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
        eph::utils::spin_for_ns(cfg.server_work_ns);
        uint64_t send_tsc = eph::utils::TSC::now();
        std::memcpy(buf.data() + 16, &send_tsc, 8);

        ::sendto(*fd, buf.data(), n, 0,
                 reinterpret_cast<sockaddr*>(&src), slen);
    }
    ::close(*fd);
    return 0;
}

} // namespace bench::exchange
