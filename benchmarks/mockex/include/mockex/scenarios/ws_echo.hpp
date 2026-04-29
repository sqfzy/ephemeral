/// @file scenarios/ws_echo.hpp
/// Port of benchmarks/latency/mocks/ws_echo.py.
///
/// WebSocket echo server: accept a client, run the RFC 6455 handshake,
/// then receive masked client frames. Every decoded text/binary frame
/// has its payload [8:16] / [16:24] overwritten with
/// `t_mock_recv` / `t_mock_send` before being echoed back as an
/// unmasked binary frame. Ping is replied with Pong; Close acks and
/// closes the socket.
///
/// TLS variant: when `[lat_ws].use_tls = true` and `ctx.tls` is set,
/// each accepted fd is wrapped in `tls::TlsConn` before the WS
/// handshake — i.e. the handshake itself rides on TLS.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include <spdlog/spdlog.h>

#include "core/measurement.hpp"
#include "eph/net/posix_listener.hpp"
#include "mockex/io_stream.hpp"
#include "mockex/scenario.hpp"
#include "mockex/scenarios/tcp_echo.hpp"  // put_u64_le
#include "mockex/tls_server.hpp"
#include "mockex/ws_server.hpp"

namespace mockex::scenarios {

namespace detail::ws_echo {
constexpr size_t kTsBlockSize = 24;
} // namespace detail::ws_echo

/// Inner per-client loop. Returns when the client closes, sends a
/// malformed frame, or shutdown is requested.
template <class IoStream>
inline void
ws_echo_handle(IoStream& io, const std::atomic<bool>& running) noexcept {
    using namespace detail::ws_echo;

    if (!ws::accept_handshake(io)) {
        SPDLOG_WARN("[ws_echo] handshake failed");
        return;
    }

    while (running.load(std::memory_order_relaxed)) {
        auto frame = ws::decode_frame(io);
        if (!frame) return;   // peer close / IO error

        if (frame->opcode == ws::kOpcodeClose) {
            // Polite close ack. Failure is fine; we're bailing anyway.
            (void)ws::send_frame(io, ws::kOpcodeClose, {});
            return;
        }
        if (frame->opcode == ws::kOpcodePing) {
            if (!ws::send_frame(io, ws::kOpcodePong, frame->payload)) return;
            continue;
        }

        // Text/binary: stamp t_mock_recv / t_mock_send at [8:24] then
        // echo as binary. Same protocol as ws_echo.py:44-53.
        if (frame->payload.size() >= kTsBlockSize) {
            const uint64_t t_recv = bench::monotonic_raw_ns();
            detail::tcp_echo::put_u64_le(frame->payload.data(), 8, t_recv);
            const uint64_t t_send = bench::monotonic_raw_ns();
            detail::tcp_echo::put_u64_le(frame->payload.data(), 16, t_send);
        }
        if (!ws::send_frame(io, ws::kOpcodeBinary, frame->payload)) return;
    }
}

[[nodiscard]] inline int ws_echo_run(const ScenarioContext& ctx) noexcept {
    const std::string host = ctx.cfg->networking.server_ip.empty()
                                 ? std::string{"127.0.0.1"}
                                 : ctx.cfg->networking.server_ip;
    auto port_e = ctx.scenario->get<uint16_t>("port");
    if (!port_e) {
        SPDLOG_ERROR("[ws_echo] missing port: {}", bench::format_error(port_e.error()));
        return 1;
    }
    const uint16_t port = *port_e;

    auto lfd_e = eph::net::posix::tcp_bind_listen(host, port);
    if (!lfd_e) {
        SPDLOG_ERROR("[ws_echo] tcp_bind_listen {}:{} failed: {}",
                     host, port, lfd_e.error());
        return 1;
    }
    const int lfd = *lfd_e;
    SPDLOG_INFO("[ws_echo] listening on {}:{} (tls={})",
                host, port, ctx.tls != nullptr);

    while (ctx.running->load(std::memory_order_acquire)) {
        auto cfd_e = eph::net::posix::accept_one(lfd, *ctx.running);
        if (!cfd_e) {
            SPDLOG_WARN("[ws_echo] accept failed: {}", cfd_e.error());
            continue;
        }
        const int cfd = *cfd_e;
        if (cfd < 0) break;

        SPDLOG_INFO("[ws_echo] client connected fd={}", cfd);

        if (ctx.tls) {
            auto conn = ctx.tls->accept_tls(cfd);
            if (conn.fd() >= 0) {
                ws_echo_handle(conn, *ctx.running);
            }
            // accept_tls already closed cfd on handshake failure.
        } else {
            io::RawFdStream io{cfd};
            ws_echo_handle(io, *ctx.running);
            // RawFdStream dtor closes fd.
        }
        SPDLOG_INFO("[ws_echo] client disconnected");
    }

    ::close(lfd);
    SPDLOG_INFO("[ws_echo] shutdown");
    return 0;
}

} // namespace mockex::scenarios
