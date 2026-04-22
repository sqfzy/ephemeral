/// @file scenarios/udp_echo.hpp
/// C++ port of benchmarks/latency/mocks/udp_echo.py.
///
/// Binds a UDP socket on `[lat_udp].port` at `mock_ip`, enlarges
/// SO_RCVBUF to 4 MiB (matching the Python version), and runs a single
/// recvfrom→stamp→sendto loop. Each datagram ≥ 24 B gets
/// `t_mock_recv` written to bytes [8:16] and `t_mock_send` to bytes
/// [16:24] via the shared CLOCK_MONOTONIC_RAW timebase.
///
/// The scenario key is templated over two identical implementations
/// (`udp` and `ex_md_udp`) because, per Phase 11.1 D-1, the two are
/// protocol-identical — only the section name + port differ. The
/// template avoids dragging two nearly-identical .hpp files around.
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "core/measurement.hpp"
#include "eph/net/posix_listener.hpp"
#include "mockex/scenario.hpp"
#include "mockex/scenarios/tcp_echo.hpp"  // put_u64_le

namespace mockex::scenarios {

namespace detail::udp_echo {
constexpr size_t kTsBlockSize = 24;
constexpr size_t kScratchSize = 65536;
constexpr int    kRcvBufBytes = 4 * 1024 * 1024;
} // namespace detail::udp_echo

/// Shared UDP echo loop. Named with a `tag` so both the `udp` and
/// `ex_md_udp` scenarios can reuse it while still emitting their own
/// log prefix.
[[nodiscard]] inline int
udp_echo_impl(const ScenarioContext& ctx, std::string_view tag) noexcept {
    using namespace detail::udp_echo;

    const std::string host = ctx.cfg->networking.server_ip.empty()
                                 ? std::string{"127.0.0.1"}
                                 : ctx.cfg->networking.server_ip;
    auto port_e = ctx.scenario->get<uint16_t>("port");
    if (!port_e) {
        SPDLOG_ERROR("[{}] missing port: {}", tag, bench::format_error(port_e.error()));
        return 1;
    }
    const uint16_t port = *port_e;

    auto fd_e = eph::net::posix::udp_bind(host, port);
    if (!fd_e) {
        SPDLOG_ERROR("[{}] udp_bind {}:{} failed: {}",
                     tag, host, port, fd_e.error());
        return 1;
    }
    const int fd = *fd_e;

    // 4 MiB SO_RCVBUF matches udp_echo.py line 42: bursty traffic
    // should not drop at the mock and pollute the latency tail.
    // Errors here are non-fatal — the kernel's default is still usable
    // at modest rates. Ignore setsockopt's return value deliberately.
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                       &kRcvBufBytes, sizeof(kRcvBufBytes));

    SPDLOG_INFO("[{}] listening on {}:{}", tag, host, port);

    std::vector<uint8_t> scratch(kScratchSize);

    while (ctx.running->load(std::memory_order_relaxed)) {
        // Poll first so SIGTERM is observed within 100 ms instead of
        // blocking in recvfrom forever. Pattern matches `accept_one`.
        struct pollfd p{ .fd = fd, .events = POLLIN, .revents = 0 };
        int rv = ::poll(&p, 1, 100);
        if (rv < 0) {
            if (errno == EINTR) continue;
            SPDLOG_WARN("[{}] poll err: {}", tag, std::strerror(errno));
            break;
        }
        if (rv == 0) continue;

        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        ssize_t n = ::recvfrom(fd, scratch.data(), scratch.size(), 0,
                               reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (n < 0) {
            if (errno == EINTR) continue;
            SPDLOG_WARN("[{}] recvfrom err: {}", tag, std::strerror(errno));
            continue;
        }
        if (n == 0) continue;

        if (static_cast<size_t>(n) >= kTsBlockSize) {
            const uint64_t t_recv = bench::monotonic_raw_ns();
            detail::tcp_echo::put_u64_le(scratch.data(), 8, t_recv);
            const uint64_t t_send = bench::monotonic_raw_ns();
            detail::tcp_echo::put_u64_le(scratch.data(), 16, t_send);
        }

        ssize_t sent = ::sendto(fd, scratch.data(),
                                static_cast<size_t>(n), 0,
                                reinterpret_cast<sockaddr*>(&caddr), clen);
        if (sent < 0) {
            SPDLOG_WARN("[{}] sendto err: {}", tag, std::strerror(errno));
        }
    }

    ::close(fd);
    SPDLOG_INFO("[{}] shutdown", tag);
    return 0;
}

[[nodiscard]] inline int udp_echo_run(const ScenarioContext& ctx) noexcept {
    return udp_echo_impl(ctx, "udp_echo");
}

} // namespace mockex::scenarios
