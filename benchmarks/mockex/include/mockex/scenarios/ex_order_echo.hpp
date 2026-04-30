/// @file scenarios/ex_order_echo.hpp
/// Port of benchmarks/latency/mocks/ex_order_echo.py.
///
/// Order-path WebSocket echo: accept a client, run the handshake, then
/// for every incoming binary frame parse it as JSON, inject
/// `"t_mock_recv":<ns>` and `"t_mock_send":<ns>`, and re-emit as a
/// binary frame. The client (lat_ex_order) then reads all three
/// timestamps via `bench::scan_json_uint_field` to decompose the RTT
/// into TX and RX legs.
///
/// We deliberately avoid pulling in a full JSON parser — the bench
/// client reads fields by name (order-insensitive), so the mock can
/// just splice `,"t_mock_recv":<ns>,"t_mock_send":<ns>` before the
/// closing `}`. On malformed input we fall back to verbatim echo so
/// the client's malformed-counter can skip the sample rather than
/// deadlocking.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include <spdlog/spdlog.h>

#include "core/measurement.hpp"
#include "eph/net/posix_listener.hpp"
#include "mockex/io_stream.hpp"
#include "mockex/scenario.hpp"
#include "mockex/tls_server.hpp"
#include "mockex/ws_server.hpp"

namespace mockex::scenarios {

namespace detail::ex_order_echo {

/// Find the last `}` in a JSON payload, skipping any trailing whitespace
/// after it. Returns npos if the payload doesn't look like a JSON object.
[[nodiscard]] inline size_t
find_closing_brace(std::string_view s) noexcept {
    for (size_t i = s.size(); i > 0; --i) {
        const char c = s[i - 1];
        if (c == '}') return i - 1;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return std::string_view::npos;
        }
    }
    return std::string_view::npos;
}

/// Splice `,"t_mock_recv":<r>,"t_mock_send":<s>` before the final `}`.
/// Requires `payload` to already be a well-formed JSON object with at
/// least one member — we don't handle `{}` specially because the
/// client never sends empty objects. Returns the rewritten bytes; on
/// any structural surprise returns `payload` verbatim.
[[nodiscard]] inline std::vector<uint8_t>
inject_timestamps(std::span<const uint8_t> payload,
                  uint64_t t_recv, uint64_t t_send) {
    std::string_view s(reinterpret_cast<const char*>(payload.data()),
                       payload.size());
    const size_t brace = find_closing_brace(s);
    if (brace == std::string_view::npos) {
        return std::vector<uint8_t>(payload.begin(), payload.end());
    }
    char buf[96];
    // `,"t_mock_recv":<20digits>,"t_mock_send":<20digits>` — max ~70 B.
    const int n = std::snprintf(
        buf, sizeof(buf),
        ",\"t_mock_recv\":%llu,\"t_mock_send\":%llu",
        static_cast<unsigned long long>(t_recv),
        static_cast<unsigned long long>(t_send));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) {
        return std::vector<uint8_t>(payload.begin(), payload.end());
    }
    std::vector<uint8_t> out;
    out.reserve(payload.size() + static_cast<size_t>(n));
    out.insert(out.end(), payload.begin(), payload.begin() + brace);
    out.insert(out.end(), buf, buf + n);
    out.insert(out.end(), payload.begin() + brace, payload.end());
    return out;
}

} // namespace detail::ex_order_echo

template <class IoStream>
inline void
ex_order_handle(IoStream& io, const std::atomic<bool>& running) noexcept {
    if (!ws::accept_handshake(io)) {
        SPDLOG_WARN("[ex_order_echo] handshake failed");
        return;
    }

    while (running.load(std::memory_order_relaxed)) {
        auto frame = ws::decode_frame(io);
        if (!frame) return;

        if (frame->opcode == ws::kOpcodeClose) {
            (void)ws::send_frame(io, ws::kOpcodeClose, {});
            return;
        }
        if (frame->opcode == ws::kOpcodePing) {
            if (!ws::send_frame(io, ws::kOpcodePong, frame->payload)) return;
            continue;
        }

        // Capture t_mock_recv IMMEDIATELY after decode — this is the
        // ns-resolution server-side recv timestamp the client will
        // read from the JSON.
        const uint64_t t_recv = bench::monotonic_raw_ns();
        // t_send is captured here, before `inject_timestamps` does its
        // vector allocation + memcpy and before `send_frame` writes to
        // the socket. The gap (a few hundred ns at most on Graviton)
        // shows up as positive bias in the RX-leg latency the client
        // computes; since both kernel and DPDK clients see the same
        // bias the kernel-vs-DPDK comparison stays fair. A
        // placeholder-patch optimisation (write 0 here, splice the
        // real ns just before sendall) was considered but adds enough
        // code complexity that we deferred it — see git log for
        // history if revisiting.
        auto patched = detail::ex_order_echo::inject_timestamps(
            frame->payload, t_recv, bench::monotonic_raw_ns());
        if (!ws::send_frame(io, ws::kOpcodeBinary,
                            std::span<const uint8_t>(patched))) {
            return;
        }
    }
}

[[nodiscard]] inline int ex_order_echo_run(const ScenarioContext& ctx) noexcept {
    const std::string host = ctx.cfg->networking.server_ip.empty()
                                 ? std::string{"127.0.0.1"}
                                 : ctx.cfg->networking.server_ip;
    auto port_e = ctx.scenario->get<uint16_t>("port");
    if (!port_e) {
        SPDLOG_ERROR("[ex_order_echo] missing port: {}", bench::format_error(port_e.error()));
        return 1;
    }
    const uint16_t port = *port_e;

    auto lfd_e = eph::net::posix::tcp_bind_listen(host, port);
    if (!lfd_e) {
        SPDLOG_ERROR("[ex_order_echo] bind {}:{} failed: {}",
                     host, port, lfd_e.error());
        return 1;
    }
    const int lfd = *lfd_e;
    SPDLOG_INFO("[ex_order_echo] listening on {}:{}", host, port);

    SPDLOG_INFO("[ex_order_echo] tls={}", ctx.tls != nullptr);
    while (ctx.running->load(std::memory_order_acquire)) {
        auto cfd_e = eph::net::posix::accept_one(lfd, *ctx.running);
        if (!cfd_e) {
            SPDLOG_WARN("[ex_order_echo] accept failed: {}", cfd_e.error());
            continue;
        }
        const int cfd = *cfd_e;
        if (cfd < 0) break;
        SPDLOG_INFO("[ex_order_echo] client connected fd={}", cfd);
        if (ctx.tls) {
            auto conn = ctx.tls->accept_tls(cfd);
            if (conn.fd() >= 0) {
                ex_order_handle(conn, *ctx.running);
            }
        } else {
            io::RawFdStream io{cfd};
            ex_order_handle(io, *ctx.running);
        }
        SPDLOG_INFO("[ex_order_echo] client disconnected");
    }

    ::close(lfd);
    SPDLOG_INFO("[ex_order_echo] shutdown");
    return 0;
}

} // namespace mockex::scenarios
