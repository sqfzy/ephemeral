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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
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

namespace detail::tcp_echo {

/// Per-client echo worker — drains recv/send until peer closes or
/// shutdown is signalled. Stamps the 24 B timestamp block on every
/// recv chunk ≥ 24 B (matches the original sequential path's
/// semantics; see tcp_echo.py:69-74).
inline void echo_client_loop(int cfd,
                              const std::atomic<bool>& running) noexcept {
    std::vector<uint8_t> scratch(kScratchSize);
    while (running.load(std::memory_order_relaxed)) {
        ssize_t n = ::recv(cfd, scratch.data(), scratch.size(), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            SPDLOG_WARN("[tcp_echo:fd={}] recv err: {}",
                        cfd, std::strerror(errno));
            break;
        }
        if (n == 0) break;  // peer closed

        if (static_cast<size_t>(n) >= kTsBlockSize) {
            const uint64_t t_recv = bench::monotonic_raw_ns();
            put_u64_le(scratch.data(), 8, t_recv);
            const uint64_t t_send = bench::monotonic_raw_ns();
            put_u64_le(scratch.data(), 16, t_send);
        }
        if (!eph::net::posix::send_all(cfd, scratch.data(),
                                       static_cast<size_t>(n))) {
            SPDLOG_WARN("[tcp_echo:fd={}] send err: {}",
                        cfd, std::strerror(errno));
            break;
        }
    }
    ::close(cfd);
}

} // namespace detail::tcp_echo

/// tcp_echo scenario handler — accept-loop with one OS thread per
/// connected client.
///
/// Reads `mockex_max_connections` from the scenario section (default 1
/// for backwards-compat with single-client lat scenarios).  Threads are
/// spawned at accept time and joined at shutdown.  Per-client state is
/// fully isolated: each client gets its own scratch buffer, its own
/// recv/send loop, and stamps timestamps from `bench::monotonic_raw_ns`
/// on every recv ≥ 24 B (same protocol as the original sequential
/// path).
///
/// Multi-conn enables PR-8's multi-stream throughput bench: N parallel
/// clients each pinned to a different RSS queue exercise the full
/// multi-queue Poller topology.
[[nodiscard]] inline int tcp_echo_run(const ScenarioContext& ctx) noexcept {
    using namespace detail::tcp_echo;

    const auto host = ctx.globals->get_string("mock_ip", "127.0.0.1");
    auto port_e = ctx.section->get_u32("port");
    if (!port_e) {
        SPDLOG_ERROR("[tcp_echo] missing port: {}", port_e.error());
        return 1;
    }
    const uint16_t port = static_cast<uint16_t>(*port_e);

    // mockex_max_connections caps concurrent clients.  Default 1 = the
    // original single-client behaviour, fully backwards-compatible with
    // lat_tcp et al.  Set to N in [lat_ex_market_multi] for PR-8.
    //
    // Hard upper bound to prevent a typo / runaway config from
    // spawning millions of std::threads → OOM / EAGAIN.  64 is more
    // than enough for any realistic HFT bench (matches kMaxRssQueues).
    constexpr uint32_t kMaxMockConn = 64;
    const uint32_t requested =
        ctx.section->get_u32("mockex_max_connections").value_or(1u);
    if (requested > kMaxMockConn) {
        SPDLOG_ERROR("[tcp_echo] mockex_max_connections={} exceeds hard cap {} "
                     "(prevents thread-per-conn exhaustion); aborting",
                     requested, kMaxMockConn);
        return 1;
    }
    const uint32_t max_conn = std::max(1u, requested);

    auto lfd_e = eph::net::posix::tcp_bind_listen(
        host, port, /*backlog=*/static_cast<int>(std::max(1u, max_conn)));
    if (!lfd_e) {
        SPDLOG_ERROR("[tcp_echo] bind/listen {}:{} failed: {}",
                     host, port, lfd_e.error());
        return 1;
    }
    const int lfd = *lfd_e;
    SPDLOG_INFO("[tcp_echo] listening on {}:{} (max_conn={})",
                host, port, max_conn);

    std::vector<std::thread> client_threads;
    client_threads.reserve(max_conn);

    while (ctx.running->load(std::memory_order_acquire)) {
        auto cfd_e = eph::net::posix::accept_one(lfd, *ctx.running);
        if (!cfd_e) {
            SPDLOG_WARN("[tcp_echo] accept failed: {}", cfd_e.error());
            continue;
        }
        const int cfd = *cfd_e;
        if (cfd < 0) break;  // shutdown requested

        // Reap any joinable finished threads before deciding capacity.
        client_threads.erase(
            std::remove_if(client_threads.begin(), client_threads.end(),
                [](std::thread& t) {
                    if (!t.joinable()) return true;
                    return false;
                }),
            client_threads.end());

        if (client_threads.size() >= max_conn) {
            SPDLOG_WARN("[tcp_echo] max_conn={} reached; rejecting fd={}",
                        max_conn, cfd);
            ::close(cfd);
            continue;
        }

        SPDLOG_INFO("[tcp_echo] client connected fd={} (active={}/{})",
                    cfd, client_threads.size() + 1, max_conn);
        client_threads.emplace_back(
            [cfd, running = ctx.running]() {
                echo_client_loop(cfd, *running);
            });
    }

    // Detach client threads — process exit will tear them down. Joining
    // here would block waiting for clients still in recv() that haven't
    // observed the shutdown flag yet.
    for (auto& t : client_threads) {
        if (t.joinable()) t.detach();
    }

    ::close(lfd);
    SPDLOG_INFO("[tcp_echo] shutdown");
    return 0;
}

} // namespace mockex::scenarios
