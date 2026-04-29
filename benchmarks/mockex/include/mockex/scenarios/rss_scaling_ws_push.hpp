/// @file scenarios/rss_scaling_ws_push.hpp
/// WS-over-TLS variant of `rss_scaling_push` — N concurrent
/// WebSocket-over-TLS clients on a single TCP listener, mock pushes
/// timestamped binary frames at fixed `push_rate_pps_per_conn`.
///
/// Wire path (mock side, per connection):
///     accept(2) → SSL_accept (aws-lc) → ws::accept_handshake (RFC 6455)
///     → push loop emits binary frames with `mock_send_ns` stamped at
///        bytes [16:24] of the WS payload.
///
/// Why a separate scenario from the UDP `rss_scaling_push`:
///   * TLS record framing + WS opcode + TCP per-conn state changes
///     the NIC RSS dispatch / per-flow-table behaviour vs raw UDP.
///   * The codec on the wire is what production uses (binance, okx,
///     coinbase WS feeds), so the slope here is the real-world one.
///
/// Required scenario keys:
///   - port                       (uint16) — TCP listen port
///   - payload_size               (uint32) — WS payload bytes (>= 24)
///   - push_rate_pps_per_conn     (uint32) — per-peer send rate
///   - conn_count                 (uint16) — caller's expected fan-out
///                                          (drives accept-loop cap)
///   - use_tls                    (bool)   — must be true (TLS-only;
///                                          plain WS is rejected to
///                                          keep the scenario name
///                                          honest)
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "core/measurement.hpp"
#include "core/timestamp_proto.hpp"
#include "eph/net/posix_listener.hpp"
#include "mockex/io_stream.hpp"
#include "mockex/scenario.hpp"
#include "mockex/tls_server.hpp"
#include "mockex/ws_server.hpp"

namespace mockex::scenarios {

namespace detail::rss_scaling_ws {

constexpr int      kSocketBufBytes       = 4 * 1024 * 1024;
constexpr uint64_t kInitialPushDelayNs   = 100ull * 1'000'000ull;
constexpr size_t   kWsHeaderMax          = 14;     ///< RFC 6455 max server header
constexpr size_t   kMockSendNsOffset     = 16;     ///< [16:24] in TS block

/// Build the static WebSocket binary-frame header for a fixed payload
/// size. Mirrors `mockex::ws::encode_frame` but emits *only* the
/// header into `out` and returns the header byte count, so the caller
/// can pre-stage the full frame buffer once and per-push only patch
/// the timestamp into the payload slice.
[[nodiscard]] inline size_t
build_ws_header(size_t payload_size, uint8_t* out) noexcept {
    size_t o = 0;
    out[o++] = static_cast<uint8_t>(
        0x80 | (static_cast<uint8_t>(ws::kOpcodeBinary) & 0x0F));
    if (payload_size < 126) {
        out[o++] = static_cast<uint8_t>(payload_size);
    } else if (payload_size < (1u << 16)) {
        out[o++] = 126;
        out[o++] = static_cast<uint8_t>((payload_size >> 8) & 0xFF);
        out[o++] = static_cast<uint8_t>(payload_size & 0xFF);
    } else {
        out[o++] = 127;
        for (int i = 7; i >= 0; --i) {
            out[o++] = static_cast<uint8_t>(
                (payload_size >> (i * 8)) & 0xFF);
        }
    }
    return o;
}

/// Per-subscriber state. The `conn` is heap-boxed so the vector can
/// hold it via unique_ptr — `tls::TlsConn` is move-only and the vector
/// resize path needs stable addresses for in-flight pointers.
struct Subscriber {
    std::unique_ptr<tls::TlsConn> conn;
    uint64_t                      next_send_ns{};
    uint64_t                      packets_sent{};
    bool                          dead     = false;   ///< latched on first send error
    bool                          active   = false;   ///< push only after first client
                                                      ///< frame ("subscribe") arrives
};

} // namespace detail::rss_scaling_ws

[[nodiscard]] inline int
rss_scaling_ws_push_run(const ScenarioContext& ctx) noexcept {
    using namespace detail::rss_scaling_ws;

    const bool use_tls = ctx.scenario->get_or<bool>("use_tls", false);
    if (!use_tls || !ctx.tls) {
        SPDLOG_ERROR("[rss_scaling_ws] requires use_tls=true (TLS-only "
                     "scenario; plain WS is served by lat_ws)");
        return 1;
    }

    const std::string host = ctx.cfg->networking.server_ip.empty()
                                 ? std::string{"127.0.0.1"}
                                 : ctx.cfg->networking.server_ip;

    auto port_e = ctx.scenario->get<uint16_t>("port");
    if (!port_e) {
        SPDLOG_ERROR("[rss_scaling_ws] missing scenarios.{}.port: {}",
                     ctx.scenario_name,
                     bench::format_error(port_e.error()));
        return 1;
    }
    const uint16_t port = *port_e;

    const uint32_t payload_size =
        ctx.scenario->get_or<uint32_t>("payload_size", 256);
    if (payload_size < bench::kTimestampBlockSize) {
        SPDLOG_ERROR("[rss_scaling_ws] payload_size={} < {} (need full TS block)",
                     payload_size, bench::kTimestampBlockSize);
        return 1;
    }

    const uint32_t push_rate =
        ctx.scenario->get_or<uint32_t>("push_rate_pps_per_conn", 2000);
    if (push_rate == 0) {
        SPDLOG_ERROR("[rss_scaling_ws] push_rate_pps_per_conn must be > 0");
        return 1;
    }
    const uint64_t interval_ns = 1'000'000'000ull / push_rate;

    // Backlog only matters for the burst of accepts at cell-start; pick
    // a generous fixed size so we don't have to know conn_count up
    // front. Mock has no per-conn cap — see "interleaved accept+push"
    // note below.
    auto lfd_e = eph::net::posix::tcp_bind_listen(host, port, /*backlog=*/256);
    if (!lfd_e) {
        SPDLOG_ERROR("[rss_scaling_ws] tcp_bind_listen {}:{} failed: {}",
                     host, port, lfd_e.error());
        return 1;
    }
    const int lfd = *lfd_e;

    // Non-blocking accept — we interleave accept-if-pending with the
    // push fanout in a single loop, so accept(2) must not block.
    {
        const int flags = ::fcntl(lfd, F_GETFL, 0);
        if (flags >= 0) (void)::fcntl(lfd, F_SETFL, flags | O_NONBLOCK);
    }

    SPDLOG_INFO("[rss_scaling_ws] listening {}:{} push_rate={} pps "
                "interval={} ns payload={} B (TLS+WS, accept-on-demand)",
                host, port, push_rate, interval_ns, payload_size);

    std::vector<Subscriber> subs;
    subs.reserve(64);

    // Pre-build the static WS binary frame once. Per-push we only patch
    // mock_send_ns into the payload region and write_all the whole
    // buffer — no per-send heap allocation.
    std::vector<uint8_t> frame(kWsHeaderMax + payload_size, 0);
    const size_t hdr_n     = build_ws_header(payload_size, frame.data());
    const size_t total_n   = hdr_n + payload_size;
    const size_t ts_offset = hdr_n + kMockSendNsOffset;

    auto accept_one_nonblocking = [&]() {
        // Try a single accept(2). The blocking SSL_accept and
        // ws::accept_handshake that follow are brief on LAN (~1–3 ms);
        // they introduce a small jitter blip on existing subscribers'
        // pushes only during cell-transition warmup, which the bench
        // client discards via warmup_seconds.
        sockaddr_in caddr{};
        socklen_t   clen = sizeof(caddr);
        const int cfd = ::accept(lfd,
                                  reinterpret_cast<sockaddr*>(&caddr),
                                  &clen);
        if (cfd < 0) return;  // EAGAIN / EWOULDBLOCK / EINTR

        auto tls_conn = ctx.tls->accept_tls(cfd);
        if (tls_conn.fd() < 0) {
            SPDLOG_WARN("[rss_scaling_ws] accept_tls(cfd={}) failed", cfd);
            return;
        }
        if (!ws::accept_handshake(tls_conn)) {
            SPDLOG_WARN("[rss_scaling_ws] WS handshake failed on cfd={}",
                        tls_conn.fd());
            return;
        }
        const int one = 1;
        (void)::setsockopt(tls_conn.fd(), IPPROTO_TCP, TCP_NODELAY,
                           &one, sizeof(one));
        (void)::setsockopt(tls_conn.fd(), SOL_SOCKET, SO_SNDBUF,
                           &kSocketBufBytes, sizeof(kSocketBufBytes));

        Subscriber s{};
        s.conn         = std::make_unique<tls::TlsConn>(std::move(tls_conn));
        s.next_send_ns = bench::monotonic_raw_ns() + kInitialPushDelayNs;
        subs.push_back(std::move(s));
        SPDLOG_INFO("[rss_scaling_ws] subscriber #{} ready (fd={})",
                    subs.size(), subs.back().conn->fd());
    };

    // Interleaved accept + push loop. Per iteration: one accept attempt
    // (non-blocking; cheap if no pending), then push to every due
    // subscriber. Dead subscribers stay in the vector but are skipped —
    // memory cost across cells is bounded (~100 entries per cell × few
    // cells) so we don't bother purging.
    uint64_t loop_counter = 0;
    // Scratch for the "subscribe" frame each new client sends. The
    // payload content is irrelevant — its arrival is the signal that
    // the client's RX path is fully wired up (workers spawned, RX
    // ring being drained) and mock can safely begin pushing without
    // backpressuring the unread SO_RCVBUF / mempool. Without this
    // gate, mbufs accumulate in the DPDK PMD's queues during the
    // bench's create_and_attach setup loop and starve out subsequent
    // SYN-ACK delivery.
    uint8_t subscribe_scratch[64];

    while (ctx.running->load(std::memory_order_relaxed)) {
        // Throttle accept attempts to ~every 64th iter — accept syscall
        // overhead on the hot push loop matters when conn_count is high.
        if ((loop_counter++ & 0x3F) == 0) {
            accept_one_nonblocking();
        }

        // Subscribe-poll throttle: same 64-iter cadence as accept. Each
        // round does a single poll(2) over every inactive sub's fd
        // (O(1) syscall, O(N) on the kernel side). Active subs are
        // skipped — once a sub is activated it's never re-polled.
        const bool subscribe_round = ((loop_counter & 0x3F) == 0);
        const uint64_t now_ns = bench::monotonic_raw_ns();
        if (subscribe_round) {
            // Build a pollfd vector for inactive, non-dead subs only.
            std::vector<struct pollfd> pfds;
            std::vector<size_t> pfd_idx;
            pfds.reserve(subs.size());
            pfd_idx.reserve(subs.size());
            for (size_t i = 0; i < subs.size(); ++i) {
                if (!subs[i].dead && !subs[i].active) {
                    pfds.push_back(
                        pollfd{.fd = subs[i].conn->fd(),
                               .events = POLLIN, .revents = 0});
                    pfd_idx.push_back(i);
                }
            }
            if (!pfds.empty()) {
                const int rv = ::poll(pfds.data(), pfds.size(), 0);
                if (rv > 0) {
                    for (size_t k = 0; k < pfds.size(); ++k) {
                        if (!(pfds[k].revents & POLLIN)) continue;
                        auto& s = subs[pfd_idx[k]];
                        const ssize_t nrd = s.conn->read_some(
                            subscribe_scratch, sizeof(subscribe_scratch));
                        if (nrd <= 0) { s.dead = true; continue; }
                        s.active       = true;
                        s.next_send_ns = bench::monotonic_raw_ns() +
                                         kInitialPushDelayNs;
                        SPDLOG_INFO("[rss_scaling_ws] subscriber fd={} "
                                    "active (subscribe frame {} bytes)",
                                    s.conn->fd(), nrd);
                    }
                }
            }
        }

        for (auto& s : subs) {
            if (s.dead || !s.active || s.next_send_ns > now_ns) continue;

            const uint64_t t_send = bench::monotonic_raw_ns();
            std::memcpy(frame.data() + ts_offset, &t_send, sizeof(t_send));

            if (!s.conn->write_all(frame.data(), total_n)) {
                // SSL_write or underlying TCP write failed (peer reset,
                // SSL fatal). Latch dead — typical when the bench
                // client closes streams between cells.
                SPDLOG_INFO("[rss_scaling_ws] write_all closed on fd={} "
                            "after {} packets (latching dead)",
                            s.conn->fd(), s.packets_sent);
                s.dead = true;
                continue;
            }
            ++s.packets_sent;
            s.next_send_ns += interval_ns;

            const uint64_t lag = (s.next_send_ns < now_ns)
                                     ? (now_ns - s.next_send_ns)
                                     : 0;
            if (lag > 5 * interval_ns) {
                s.next_send_ns = now_ns + interval_ns;
            }
        }
    }

    size_t alive = 0, dead = 0;
    uint64_t total_sent = 0;
    for (const auto& s : subs) {
        if (s.dead) ++dead; else ++alive;
        total_sent += s.packets_sent;
    }
    SPDLOG_INFO("[rss_scaling_ws] shutdown — subs={} (alive={}, dead={}) "
                "total_sent={}",
                subs.size(), alive, dead, total_sent);
    ::close(lfd);
    return 0;
}

} // namespace mockex::scenarios
