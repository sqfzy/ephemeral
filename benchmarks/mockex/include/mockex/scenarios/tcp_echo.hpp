/// @file scenarios/tcp_echo.hpp
/// C++ port of benchmarks/latency/mocks/tcp_echo.py.
///
/// Reads `[lat_tcp].port` + globals `mock_ip`, binds a TCP listener,
/// accepts one client at a time. On every recv chunk ≥ 24 B we stamp
/// `t_mock_recv` at bytes [8:16] and `t_mock_send` at bytes [16:24]
/// (little-endian uint64 ns, CLOCK_MONOTONIC_RAW via
/// `bench::monotonic_raw_ns`) before echoing the bytes back. The
/// client's timestamp at [0:8] and the filler at [24:] are untouched.
///
/// The 24-byte block convention matches Phase 11.1 of the bench.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "core/measurement.hpp"          // bench::monotonic_raw_ns
#include "eph/net/posix_io.hpp"
#include "eph/net/posix_listener.hpp"
#include "mockex/scenario.hpp"

namespace mockex::scenarios {

namespace detail::tcp_echo {
constexpr size_t kTsBlockSize = 24;
constexpr size_t kScratchSize = 65536;

/// Write a little-endian uint64 into `buf` at `off`. The bench
/// protocol uses LE regardless of host byte order because both client
/// and mock run on the same architecture (Graviton aarch64 is LE).
/// Keeping the code explicit avoids a surprise if the bench ever moves
/// to a BE host.
inline void put_u64_le(uint8_t* buf, size_t off, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        buf[off + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (i * 8));
    }
}
} // namespace detail::tcp_echo

/// tcp_echo scenario handler — one accept loop, per-client inner loop.
///
/// Mirrors `tcp_echo.py` semantics: the first recv chunk of each
/// application frame has its [8:24] block overwritten with the mock's
/// RECV/SEND timestamps; subsequent chunks of the same frame (kernel
/// may split a single client send into multiple chunks) pass through
/// unmodified.
[[nodiscard]] inline int tcp_echo_run(const ScenarioContext& ctx) noexcept {
    using namespace detail::tcp_echo;

    const auto host = ctx.globals->get_string("mock_ip", "127.0.0.1");
    auto port_e = ctx.section->get_u32("port");
    if (!port_e) {
        SPDLOG_ERROR("[tcp_echo] missing port: {}", port_e.error());
        return 1;
    }
    const uint16_t port = static_cast<uint16_t>(*port_e);

    auto lfd_e = eph::net::posix::tcp_bind_listen(host, port, /*backlog=*/1);
    if (!lfd_e) {
        SPDLOG_ERROR("[tcp_echo] bind/listen {}:{} failed: {}",
                     host, port, lfd_e.error());
        return 1;
    }
    const int lfd = *lfd_e;
    SPDLOG_INFO("[tcp_echo] listening on {}:{}", host, port);

    // Scratch buffer reused across accepts — matches the Python
    // optimization and keeps the mock off the critical-path allocator.
    std::vector<uint8_t> scratch(kScratchSize);

    while (ctx.running->load(std::memory_order_acquire)) {
        auto cfd_e = eph::net::posix::accept_one(lfd, *ctx.running);
        if (!cfd_e) {
            SPDLOG_WARN("[tcp_echo] accept failed: {}", cfd_e.error());
            continue;
        }
        const int cfd = *cfd_e;
        if (cfd < 0) break;  // shutdown requested

        SPDLOG_INFO("[tcp_echo] client connected fd={}", cfd);
        while (ctx.running->load(std::memory_order_relaxed)) {
            ssize_t n = ::recv(cfd, scratch.data(), scratch.size(), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                SPDLOG_WARN("[tcp_echo] recv err: {}", std::strerror(errno));
                break;
            }
            if (n == 0) break;  // peer closed

            // Phase 11.1 timestamp stamping on the first chunk of each
            // recv. See tcp_echo.py:69-74 for the identical logic.
            if (static_cast<size_t>(n) >= kTsBlockSize) {
                const uint64_t t_recv = bench::monotonic_raw_ns();
                put_u64_le(scratch.data(), 8, t_recv);
                const uint64_t t_send = bench::monotonic_raw_ns();
                put_u64_le(scratch.data(), 16, t_send);
            }
            if (!eph::net::posix::send_all(cfd, scratch.data(),
                                           static_cast<size_t>(n))) {
                SPDLOG_WARN("[tcp_echo] send err: {}", std::strerror(errno));
                break;
            }
        }
        ::close(cfd);
        SPDLOG_INFO("[tcp_echo] client disconnected");
    }

    ::close(lfd);
    SPDLOG_INFO("[tcp_echo] shutdown");
    return 0;
}

} // namespace mockex::scenarios
