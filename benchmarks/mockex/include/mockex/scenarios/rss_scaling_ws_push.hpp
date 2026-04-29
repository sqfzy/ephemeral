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
    bool                          dead = false;   ///< latched on first send error
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

    const uint16_t expected_conn_count =
        ctx.scenario->get_or<uint16_t>("conn_count", 5);

    // Listen with backlog ≥ expected fan-out so the burst of N client
    // connect()s during the bench's setup phase doesn't overflow the
    // SYN queue. tcp_bind_listen sets SO_REUSEADDR + nonblocking +
    // accept_one polls cooperatively.
    auto lfd_e = eph::net::posix::tcp_bind_listen(
        host, port, /*backlog=*/std::max<int>(expected_conn_count, 8));
    if (!lfd_e) {
        SPDLOG_ERROR("[rss_scaling_ws] tcp_bind_listen {}:{} failed: {}",
                     host, port, lfd_e.error());
        return 1;
    }
    const int lfd = *lfd_e;
    SPDLOG_INFO("[rss_scaling_ws] listening {}:{} expected_conns={} "
                "push_rate={} pps interval={} ns payload={} B (TLS+WS)",
                host, port, expected_conn_count,
                push_rate, interval_ns, payload_size);

    std::vector<Subscriber> subs;
    subs.reserve(expected_conn_count);

    // ── Phase 1: accept all clients sequentially ──────────────────────
    //
    // Each accept does:
    //   accept_one(lfd, running)  → cfd
    //   ctx.tls->accept_tls(cfd)  → SSL_accept (aws-lc handshake)
    //   ws::accept_handshake(io)  → RFC 6455 client→server upgrade
    // Sequential is fine: the bench client opens conn i+1 only after
    // conn i's handshake completes, so the pipeline matches.
    while (subs.size() < expected_conn_count &&
           ctx.running->load(std::memory_order_acquire)) {
        auto cfd_e = eph::net::posix::accept_one(lfd, *ctx.running);
        if (!cfd_e) {
            SPDLOG_WARN("[rss_scaling_ws] accept: {}", cfd_e.error());
            continue;
        }
        const int cfd = *cfd_e;
        if (cfd < 0) break;  // shutdown signaled

        auto tls_conn = ctx.tls->accept_tls(cfd);
        if (tls_conn.fd() < 0) {
            SPDLOG_WARN("[rss_scaling_ws] accept_tls(cfd={}) failed; "
                        "client closed pre-handshake?", cfd);
            continue;
        }

        if (!ws::accept_handshake(tls_conn)) {
            SPDLOG_WARN("[rss_scaling_ws] WS handshake failed on cfd={}",
                        tls_conn.fd());
            continue;  // tls_conn destructor closes fd
        }

        // TCP_NODELAY on the underlying fd — bench measures push-to-RX
        // latency, Nagle batching would inflate it visibly at low rate.
        const int one = 1;
        (void)::setsockopt(tls_conn.fd(), IPPROTO_TCP, TCP_NODELAY,
                           &one, sizeof(one));
        // Larger SO_SNDBUF so the per-conn bursty fan-out doesn't drain
        // serialize on the kernel buffer. Best-effort.
        (void)::setsockopt(tls_conn.fd(), SOL_SOCKET, SO_SNDBUF,
                           &kSocketBufBytes, sizeof(kSocketBufBytes));

        Subscriber s{};
        s.conn         = std::make_unique<tls::TlsConn>(std::move(tls_conn));
        s.next_send_ns = bench::monotonic_raw_ns() + kInitialPushDelayNs;
        subs.push_back(std::move(s));

        SPDLOG_INFO("[rss_scaling_ws] subscriber #{} ready (fd={})",
                    subs.size(), subs.back().conn->fd());
    }

    if (!ctx.running->load(std::memory_order_acquire)) {
        ::close(lfd);
        return 0;
    }
    SPDLOG_INFO("[rss_scaling_ws] accepted {} subscribers; entering push loop",
                subs.size());

    // ── Phase 2: pre-build the static frame and enter push loop ───────
    //
    // The WS header for our fixed payload size is built once. Per-push
    // we only patch `mock_send_ns` into the payload region (offset
    // hdr_n + 16) and write_all the whole frame. No per-send heap.
    std::vector<uint8_t> frame(kWsHeaderMax + payload_size, 0);
    const size_t hdr_n     = build_ws_header(payload_size, frame.data());
    const size_t total_n   = hdr_n + payload_size;
    const size_t ts_offset = hdr_n + kMockSendNsOffset;

    while (ctx.running->load(std::memory_order_relaxed)) {
        const uint64_t now_ns = bench::monotonic_raw_ns();
        for (auto& s : subs) {
            if (s.dead || s.next_send_ns > now_ns) continue;

            const uint64_t t_send = bench::monotonic_raw_ns();
            std::memcpy(frame.data() + ts_offset, &t_send, sizeof(t_send));

            if (!s.conn->write_all(frame.data(), total_n)) {
                // SSL_write or underlying TCP write failed (peer reset,
                // SSL fatal). Latch dead — don't retry, would just thrash.
                SPDLOG_WARN("[rss_scaling_ws] write_all failed on fd={} "
                            "after {} packets; latching dead",
                            s.conn->fd(), s.packets_sent);
                s.dead = true;
                continue;
            }
            ++s.packets_sent;
            s.next_send_ns += interval_ns;

            // Drift correction: if the loop fell more than 5 intervals
            // behind (CFS preempt, TLS write contention spike), snap
            // the next deadline forward instead of issuing a long
            // catch-up burst. Same heuristic as rss_scaling_push.
            const uint64_t lag = (s.next_send_ns < now_ns)
                                     ? (now_ns - s.next_send_ns)
                                     : 0;
            if (lag > 5 * interval_ns) {
                s.next_send_ns = now_ns + interval_ns;
            }
        }
    }

    for (size_t i = 0; i < subs.size(); ++i) {
        SPDLOG_INFO("[rss_scaling_ws] subscriber #{} sent={} packets dead={}",
                    i, subs[i].packets_sent, subs[i].dead);
    }
    ::close(lfd);
    SPDLOG_INFO("[rss_scaling_ws] shutdown");
    return 0;
}

} // namespace mockex::scenarios
