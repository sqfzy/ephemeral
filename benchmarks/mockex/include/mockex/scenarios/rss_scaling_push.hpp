/// @file scenarios/rss_scaling_push.hpp
/// One-way UDP push handler used by the `lat_rss_scaling` benchmark.
///
/// Goal: reproduce the "DPDK multi-lcore RSS RX latency grows with N
/// connections" observation. To isolate the *client-side* RX path
/// (NIC/PMD → lcore burst → user callback) we ditch the echo bench
/// entirely and have the mock fire timestamped pushes at every known
/// peer at a fixed rate. The client measures
///     `t_recv_client - t_mock_send`
/// (one-way RX latency on the DPDK side; analogous on the kernel side).
///
/// Wire protocol (matches `core/timestamp_proto.hpp`):
///   - Subscribe: client sends a single zero-filled datagram from each
///     of its UDP sockets. Mock records the (client_ip, src_port) tuple
///     on first arrival.
///   - Push:   mock writes `mock_send_ns` into bytes [16:24] (the same
///     RX-leg slot used by the echo scenarios) and `sendto`s it to each
///     subscribed peer at `push_rate_pps_per_conn`.
///
/// Subscribers persist for the lifetime of the scenario; we do not
/// auto-evict on idle. The scenario lives one bench run, so the steady-
/// state count is exactly the client's `conn_count`.
///
/// Required scenario keys:
///   - port                       (uint16) — UDP listen port
///   - payload_size               (uint32) — bytes per push (>= 24)
///   - push_rate_pps_per_conn     (uint32) — per-peer send rate in pps
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "core/measurement.hpp"        // bench::monotonic_raw_ns
#include "core/timestamp_proto.hpp"    // bench::kTimestampBlockSize
#include "eph/net/posix_listener.hpp"  // posix::udp_bind
#include "mockex/scenario.hpp"

namespace mockex::scenarios {

namespace detail::rss_scaling {

constexpr int    kSocketBufBytes        = 4 * 1024 * 1024;
constexpr size_t kRecvScratch           = 1024;
constexpr uint64_t kInitialPushDelayNs  = 100ull * 1'000'000ull;  // 100 ms grace
constexpr uint64_t kMockSendNsOffset    = 16;                      // [16:24] in TS block

/// Internal subscriber record. Keyed by 5-tuple via memcmp on `addr`.
struct Subscriber {
    sockaddr_in  addr{};
    socklen_t    addrlen{};
    uint64_t     next_send_ns{};   ///< absolute ns deadline for next push
    uint64_t     packets_sent{};
};

} // namespace detail::rss_scaling

[[nodiscard]] inline int
rss_scaling_push_run(const ScenarioContext& ctx) noexcept {
    using namespace detail::rss_scaling;

    const std::string host = ctx.cfg->networking.server_ip.empty()
                                 ? std::string{"127.0.0.1"}
                                 : ctx.cfg->networking.server_ip;

    auto port_e = ctx.scenario->get<uint16_t>("port");
    if (!port_e) {
        SPDLOG_ERROR("[rss_scaling] missing scenarios.{}.port: {}",
                     ctx.scenario_name,
                     bench::format_error(port_e.error()));
        return 1;
    }
    const uint16_t port = *port_e;

    const uint32_t payload_size =
        ctx.scenario->get_or<uint32_t>("payload_size", 256);
    if (payload_size < bench::kTimestampBlockSize) {
        SPDLOG_ERROR("[rss_scaling] payload_size={} < {} (need full TS block)",
                     payload_size, bench::kTimestampBlockSize);
        return 1;
    }

    const uint32_t push_rate =
        ctx.scenario->get_or<uint32_t>("push_rate_pps_per_conn", 5000);
    if (push_rate == 0) {
        SPDLOG_ERROR("[rss_scaling] push_rate_pps_per_conn must be > 0");
        return 1;
    }
    const uint64_t interval_ns = 1'000'000'000ull / push_rate;

    auto fd_e = eph::net::posix::udp_bind(host, port);
    if (!fd_e) {
        SPDLOG_ERROR("[rss_scaling] udp_bind {}:{} failed: {}",
                     host, port, fd_e.error());
        return 1;
    }
    const int fd = *fd_e;

    // Buffers: large RCVBUF to absorb the subscribe burst (N peers
    // hitting at once); large SNDBUF so the push fan-out doesn't
    // serialize on socket-buffer drain. Failures are non-fatal.
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                       &kSocketBufBytes, sizeof(kSocketBufBytes));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                       &kSocketBufBytes, sizeof(kSocketBufBytes));

    // Non-blocking — the hot loop interleaves subscribe-drain with
    // push-fanout, neither of which can afford to block.
    {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    SPDLOG_INFO("[rss_scaling] listening {}:{} push_rate={} pps "
                "interval={} ns payload={} B",
                host, port, push_rate, interval_ns, payload_size);

    std::vector<Subscriber> subs;
    subs.reserve(256);

    std::vector<uint8_t> payload(payload_size, 0);

    auto find_or_add = [&subs](const sockaddr_in& sa,
                               socklen_t sl,
                               uint64_t now_ns) -> Subscriber& {
        for (auto& s : subs) {
            if (s.addrlen == sl &&
                std::memcmp(&s.addr, &sa, sl) == 0) {
                return s;
            }
        }
        Subscriber s{};
        s.addr        = sa;
        s.addrlen     = sl;
        // First send: 100 ms after subscribe. Lets the client finish
        // the subscribe pass on every socket before the push storm
        // begins — avoids early packets racing late subscribe sends
        // through the kernel mock.
        s.next_send_ns = now_ns + kInitialPushDelayNs;
        s.packets_sent = 0;
        subs.push_back(s);
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
        SPDLOG_INFO("[rss_scaling] new peer #{}: {}:{} (first push in 100 ms)",
                    subs.size(), ip,
                    static_cast<unsigned>(::ntohs(sa.sin_port)));
        return subs.back();
    };

    while (ctx.running->load(std::memory_order_relaxed)) {
        const uint64_t now_ns = bench::monotonic_raw_ns();

        // 1) Drain any pending subscribe datagrams. We don't care about
        //    payload — the (caddr, clen) is the subscription key.
        for (;;) {
            sockaddr_in caddr{};
            socklen_t   clen = sizeof(caddr);
            uint8_t     scratch[kRecvScratch];
            ssize_t n = ::recvfrom(fd, scratch, sizeof(scratch),
                                   MSG_DONTWAIT,
                                   reinterpret_cast<sockaddr*>(&caddr),
                                   &clen);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                SPDLOG_WARN("[rss_scaling] recvfrom err: {}",
                            std::strerror(errno));
                break;
            }
            (void)find_or_add(caddr, clen, now_ns);
        }

        // 2) Push to every subscriber whose deadline has passed.
        //    Stamp the mock_send_ns RIGHT before sendto for tightest
        //    one-way RX measurement.
        for (auto& s : subs) {
            if (s.next_send_ns > now_ns) continue;

            const uint64_t t_send = bench::monotonic_raw_ns();
            std::memcpy(payload.data() + kMockSendNsOffset,
                        &t_send, sizeof(t_send));
            ssize_t sent = ::sendto(
                fd, payload.data(), payload.size(), MSG_DONTWAIT,
                reinterpret_cast<sockaddr*>(&s.addr), s.addrlen);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // SNDBUF full — defer this slot one interval.
                    s.next_send_ns = now_ns + interval_ns;
                    continue;
                }
                if (errno == ECONNREFUSED || errno == ENETUNREACH) {
                    // Client gone — stop counting this peer's rate but
                    // don't drop it from the table (cheap, scenario-
                    // scoped).
                    s.next_send_ns = now_ns + 100ull * 1'000'000ull;
                    continue;
                }
                SPDLOG_WARN("[rss_scaling] sendto err: {}",
                            std::strerror(errno));
                continue;
            }
            ++s.packets_sent;
            s.next_send_ns += interval_ns;

            // Drift correction: if the loop fell >5 intervals behind
            // (e.g. CFS preemption on shared dev hosts), snap forward
            // so we don't issue a long burst of catch-up sends.
            const uint64_t lag = (s.next_send_ns < now_ns)
                                     ? (now_ns - s.next_send_ns)
                                     : 0;
            if (lag > 5 * interval_ns) {
                s.next_send_ns = now_ns + interval_ns;
            }
        }
    }

    for (size_t i = 0; i < subs.size(); ++i) {
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &subs[i].addr.sin_addr, ip, sizeof(ip));
        SPDLOG_INFO("[rss_scaling] peer #{} {}:{} sent={} packets",
                    i, ip,
                    static_cast<unsigned>(::ntohs(subs[i].addr.sin_port)),
                    subs[i].packets_sent);
    }

    ::close(fd);
    SPDLOG_INFO("[rss_scaling] shutdown");
    return 0;
}

} // namespace mockex::scenarios
