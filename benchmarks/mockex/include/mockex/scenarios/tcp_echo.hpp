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
///
/// TLS variant: when `[lat_tcp].use_tls = true` and
/// `ScenarioContext::tls` is set, each accepted fd is wrapped in a
/// `tls::TlsConn` before entering the echo loop. The loop body is
/// templated on `IoStream` so the same code runs over both.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "core/measurement.hpp"          // bench::monotonic_raw_ns
#include "eph/net/posix_io.hpp"
#include "eph/net/posix_listener.hpp"
#include "mockex/io_stream.hpp"
#include "mockex/scenario.hpp"
#include "mockex/tls_server.hpp"

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

/// Per-client echo worker — drains read/write until peer closes or
/// shutdown is signalled. Stamps the 24 B timestamp block on every
/// chunk ≥ 24 B (matches the original sequential path's semantics; see
/// tcp_echo.py:69-74).
///
/// Templated on `IoStream` so the same body serves plain TCP and
/// TLS-wrapped TCP. `io` is moved-in and dropped (closed) on return.
template <class IoStream>
inline void echo_client_loop(IoStream io,
                             const std::atomic<bool>& running,
                             int fd_for_log) noexcept {
    std::vector<uint8_t> scratch(kScratchSize);
    while (running.load(std::memory_order_relaxed)) {
        const ssize_t n = io.read_some(scratch.data(), scratch.size());
        if (n < 0) {
            SPDLOG_WARN("[tcp_echo:fd={}] read err: {}",
                        fd_for_log, std::strerror(errno));
            break;
        }
        if (n == 0) break;  // peer closed

        if (static_cast<size_t>(n) >= kTsBlockSize) {
            const uint64_t t_recv = bench::monotonic_raw_ns();
            put_u64_le(scratch.data(), 8, t_recv);
            const uint64_t t_send = bench::monotonic_raw_ns();
            put_u64_le(scratch.data(), 16, t_send);
        }
        if (!io.write_all(scratch.data(), static_cast<size_t>(n))) {
            SPDLOG_WARN("[tcp_echo:fd={}] write err: {}",
                        fd_for_log, std::strerror(errno));
            break;
        }
    }
    // io.~IoStream() closes the fd / SSL session.
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

    const std::string host = ctx.cfg->networking.server_ip.empty()
                                 ? std::string{"127.0.0.1"}
                                 : ctx.cfg->networking.server_ip;
    auto port_e = ctx.scenario->get<uint16_t>("port");
    if (!port_e) {
        SPDLOG_ERROR("[tcp_echo] missing port: {}", bench::format_error(port_e.error()));
        return 1;
    }
    const uint16_t port = *port_e;

    constexpr uint32_t kMaxMockConn = 64;
    const uint32_t requested =
        ctx.scenario->get_or<uint32_t>("mockex_max_connections", 1u);
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
    SPDLOG_INFO("[tcp_echo] listening on {}:{} (max_conn={}, tls={})",
                host, port, max_conn, ctx.tls != nullptr);

    std::vector<std::thread> client_threads;
    client_threads.reserve(max_conn);

    // Active client fds — needed at shutdown to wake worker threads
    // out of recv() so they can join cleanly. Worker thread closes its
    // own fd on exit; main thread reads this list at shutdown to call
    // shutdown(fd, SHUT_RDWR) on still-blocked sockets.
    std::mutex                         active_fds_mu;
    std::vector<int>                   active_fds;
    active_fds.reserve(max_conn);

    while (ctx.running->load(std::memory_order_acquire)) {
        auto cfd_e = eph::net::posix::accept_one(lfd, *ctx.running);
        if (!cfd_e) {
            SPDLOG_WARN("[tcp_echo] accept failed: {}", cfd_e.error());
            continue;
        }
        const int cfd = *cfd_e;
        if (cfd < 0) break;  // shutdown requested

        // Cap on LIVE conns, not lifetime conns.  `client_threads` only
        // ever grows (we never join finished workers mid-loop), so using
        // its size as the cap meant N sequential reconnects would
        // permanently exhaust the budget even though each prior client
        // had long since exited.  `active_fds.size()` is the live count
        // — workers self-erase on exit (under `active_fds_mu`).
        std::size_t live = 0;
        {
            std::lock_guard lk(active_fds_mu);
            live = active_fds.size();
            if (live >= max_conn) {
                SPDLOG_WARN("[tcp_echo] max_conn={} reached (live={}); "
                            "rejecting fd={}", max_conn, live, cfd);
                ::close(cfd);
                continue;
            }
            active_fds.push_back(cfd);
        }
        SPDLOG_INFO("[tcp_echo] client connected fd={} (live={}/{})",
                    cfd, live + 1, max_conn);

        const bool use_tls = (ctx.tls != nullptr);
        client_threads.emplace_back(
            [cfd, use_tls, tls = ctx.tls,
             running = ctx.running,
             &active_fds_mu, &active_fds]() {
                // Order matters here. Without the explicit ordering, the
                // RAII IoStream's destructor closes cfd BEFORE main's
                // erase happens — opening a tiny window where
                //   1. worker close()d cfd,
                //   2. kernel reused that fd for an unrelated socket,
                //   3. main's shutdown loop sees cfd still in
                //      active_fds, calls shutdown(cfd, SHUT_RDWR),
                //      and nukes the unrelated socket.
                //
                // C++ destruction order is reverse of declaration order
                // within the same scope. So we declare the IoStream
                // FIRST and the Eraser SECOND in the lambda scope: at
                // function exit, ~Eraser fires first (erases under
                // mutex), then ~IoStream fires (closes the fd).
                //
                // Because the run loop is templated on IoStream
                // (RawFdStream vs TLS conn), use std::variant to hold
                // either alternative without forcing the body into the
                // inner if/else.
                io::RawFdStream  raw_io{cfd};
                tls::TlsConn     tls_io{};  // default: fd=-1

                struct Eraser {
                    std::mutex&        mu;
                    std::vector<int>&  fds;
                    int                cfd;
                    ~Eraser() {
                        std::lock_guard lk(mu);
                        fds.erase(std::remove(fds.begin(), fds.end(), cfd),
                                  fds.end());
                    }
                };
                Eraser guard{active_fds_mu, active_fds, cfd};

                if (use_tls) {
                    // accept_tls takes ownership of cfd via the tls
                    // wrapper; release raw_io first so it doesn't
                    // double-close. The release() return value is the
                    // fd we already have; cast to void to acknowledge
                    // the [[nodiscard]] attribute.
                    (void)raw_io.release();
                    tls_io = tls->accept_tls(cfd);
                    if (tls_io.fd() >= 0) {
                        echo_client_loop(std::move(tls_io), *running, cfd);
                    }
                    // accept_tls closed cfd on handshake failure.
                } else {
                    echo_client_loop(std::move(raw_io), *running, cfd);
                }
                // Lambda exits here. Destruction order (reverse decl):
                //   1. ~Eraser    — removes cfd from active_fds (mutex)
                //   2. ~tls_io    — closes (or no-op if moved-out)
                //   3. ~raw_io    — closes (or no-op if released/moved)
                // Step 1 runs before any close(), so main's shutdown
                // loop never sees a stale entry pointing at a reused fd.
            });
    }

    // Graceful shutdown: shutdown(SHUT_RDWR) on every still-active fd
    // so worker threads' recv() returns 0 and they exit + close. Then
    // join all workers — bounded wait, no zombie sockets at process exit.
    {
        std::lock_guard lk(active_fds_mu);
        for (int fd : active_fds) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }
    for (auto& t : client_threads) {
        if (t.joinable()) t.join();
    }

    ::close(lfd);
    SPDLOG_INFO("[tcp_echo] shutdown");
    return 0;
}

} // namespace mockex::scenarios
