/// @file production_client.cpp
///
/// Shows the production-quality knobs a real HFT client wires onto a
/// KernelTcpStream: TLS on, TCP_NODELAY, bounded reasm buffer,
/// signal-driven shutdown, and an outer reconnect loop driven by
/// `eph::utils::ExponentialBackoff`. The reconnect loop lives in the caller —
/// the stream layer itself does not retry (by design: real recovery
/// needs protocol-layer state the stream cannot see, e.g. FIX Logon
/// seq numbers, kill-switch gates, primary/backup routing).
///
/// No exchange-specific logic — the file is deliberately a template
/// that a strategy can drop into.
///
/// Usage:
///   production_client [--host <ipv4>] [--port <port>] [--tls | --no-tls]
///
///   --host    Remote IPv4 literal (default: 127.0.0.1). Hostnames not
///             accepted — production deployments resolve once at startup
///             via the async DNS path (see async_dns_multi_resolve.cpp).
///   --port    Remote port (default: 9443).
///   --tls     Enable TLS (aws-lc backend).
///   --no-tls  Disable TLS (default; flip when pointing at a real peer).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/codec/ws_codec.hpp"
#include "eph/core/metrics_concept.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/net/stream_metrics.hpp"
#include "eph/utils/backoff.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

// ── Signal-driven shutdown ─────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static void on_signal(int) {
    g_running.store(false, std::memory_order_release);
}

// ── Run one session to completion (returns when disconnected or signalled) ─
//
// `SessionOutcome::SignalStop`        = session ended cleanly on signal
//                                       (outer loop should stop)
// `SessionOutcome::Connected`         = stream connected & ran the poll
//                                       loop, then dropped — outer loop
//                                       should `policy.reset()` then
//                                       reconnect (a clean drop after a
//                                       healthy session restarts the
//                                       backoff chain fresh).
// `SessionOutcome::CreateFailed`      = `Stream::create()` failed before
//                                       the poll loop ever ran — outer
//                                       loop must NOT reset; the next
//                                       attempt should inherit the
//                                       exponential-backoff growth.

enum class SessionOutcome : uint8_t {
    SignalStop,
    Connected,
    CreateFailed,
};

template <bool EnableTls>
static SessionOutcome run_one_session(en::StreamConfig& cfg,
                                      en::KernelPoller& poller) {
    auto sr = en::KernelTcpStream<ec::WsCodec, EnableTls>::create(cfg);
    if (!sr) {
        spdlog::error("create failed: {}", sr.error().detail);
        return SessionOutcome::CreateFailed;
    }
    auto stream = std::move(*sr);
    // [[maybe_unused]] on the param: SPDLOG_DEBUG compiles out under
    // SPDLOG_ACTIVE_LEVEL=INFO (the release-build default), so app_frame
    // is referenced only at TRACE/DEBUG levels — without the attribute,
    // release builds get -Wunused-parameter.
    stream->on_message = [](std::span<const uint8_t> app_frame [[maybe_unused]]) {
        SPDLOG_DEBUG("prod: rx {} bytes", app_frame.size());
    };
    if (auto r = poller.add(stream.get()); !r) {
        spdlog::error("poller add failed: {}", r.error().detail);
        // poller.add() failing after a successful create is rare — the fd is
        // already open. Treat it as a create-class failure for backoff
        // purposes: we never entered the steady-state poll loop, so the
        // exponential chain should keep growing rather than reset.
        return SessionOutcome::CreateFailed;
    }

    // ── Observability hookup ─────────────────────────────────────────────
    // Stream metrics are auto-collected on the hot path (single `lock add`
    // per event). Periodically `publish_metrics()` to any MetricsSink.
    // In real production, swap NullSink for PrometheusSink / OtelSink etc.;
    // the snapshot frequency is the application's choice (here: 1s).
    eph::core::NullSink metrics_sink;  // <-- replace with prod sink
    auto next_publish = std::chrono::steady_clock::now() + 1s;

    while (g_running.load(std::memory_order_acquire)
           && stream->state() == eph::net::TcpState::Established) {
        (void)poller.poll(100ms);

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_publish) {
            eph::net::publish_metrics(*stream, metrics_sink);
            next_publish = now + 1s;
        }
    }
    // ── Graceful shutdown path ────────────────────────────────────────────
    // If we exited the loop because the operator hit Ctrl-C while the
    // session was still Established (the typical end-of-day shape), ride
    // through `drain(timeout)` so:
    //   1. Anything still buffered in our send-side socket buffer flushes
    //      to the peer.
    //   2. We send our own FIN (`shutdown(SHUT_WR)`).
    //   3. We block until the peer's FIN-ACK comes back (or the timeout
    //      bumps `kRxSessionResets` and we tear the session down).
    //
    // We must `poller.remove()` *before* `drain()` because drain is
    // synchronous and inherently single-threaded — it can't share the fd
    // with a Poller that's still poll()-ing it. After drain returns we
    // exit run_one_session; the unique_ptr destructor is a no-op since
    // the fd is already closed.
    //
    // For loss-of-connectivity (state() != Established when the loop
    // exits), drain() would return InvalidConfig — we skip it and just
    // remove from the Poller.
    (void)poller.remove(stream.get());
    if (stream->state() == eph::net::TcpState::Established) {
        if (auto dr = stream->drain(2s); !dr) {
            spdlog::warn("drain failed: {} (peer FIN may be lost; closing "
                         "socket regardless)", dr.error().detail);
        } else {
            spdlog::info("drain ok — peer FIN received, session orderly-closed");
        }
    }
    // Reached the poll loop, then exited it. If the operator hit Ctrl-C
    // (g_running cleared) we propagate SignalStop so the outer loop bails
    // out instead of reconnecting; otherwise the session connected
    // successfully and dropped on its own — the outer loop should
    // `policy.reset()` before computing the next backoff.
    if (!g_running.load(std::memory_order_acquire)) {
        return SessionOutcome::SignalStop;
    }
    return SessionOutcome::Connected;
}

static int run_session(const std::string& host, uint16_t port, bool use_tls) {
    auto ip = eph::net::Ipv4Addr::parse(host);
    if (!ip) {
        spdlog::error("production_client: --host must be an IPv4 literal, "
                      "got '{}'", host);
        return 1;
    }

    auto poller = en::KernelPoller::create({}).value();

    en::StreamConfig cfg{};
    cfg.remote          = eph::net::SocketAddr{*ip, port};
    cfg.reasm_capacity  = 256 * 1024;   // room for burst of snapshot frames
    cfg.connect_timeout = 3s;
    cfg.kernel.tcp_nodelay = true;

    // Reconnect policy lives HERE in the caller — not on the stream
    // config. This is deliberate: after a drop, real recovery needs
    // protocol-level state (FIX Logon seq num resync, ITCH snapshot
    // replay, kill-switch check) that a stream-local retry cannot see.
    // Production-sane defaults: exponential back-off, unbounded
    // attempts, ±25% jitter.
    eph::utils::ExponentialBackoff reconnect{eph::utils::ExponentialBackoff::Config{}};

    while (g_running.load(std::memory_order_acquire)) {
        const auto outcome = use_tls ? run_one_session<true>(cfg, *poller)
                                     : run_one_session<false>(cfg, *poller);
        if (outcome == SessionOutcome::SignalStop) break;

        // A clean drop after a healthy session restarts the backoff chain
        // fresh — otherwise every successive drop would inherit the
        // previous chain's accumulated delay and quickly saturate at
        // max_backoff, defeating the exponential growth design. Matches
        // the documented pattern in session_reconnect.cpp / the
        // production loop in binance_latency.cpp. The create-failure
        // path deliberately skips the reset so the exponential chain
        // continues to grow as documented in ExponentialBackoff.
        if (outcome == SessionOutcome::Connected) {
            reconnect.reset();
        }

        // Session dropped (or create failed). Sleep with jitter before
        // trying again. A real strategy would re-check kill-switch,
        // consult a backup remote, or refresh credentials here before
        // reconnecting.
        const auto delay = reconnect.next_delay();
        if (!delay) break;  // backoff budget exhausted (never with the
                            // default unlimited config; honored if a finite
                            // max_attempts is configured)
        spdlog::warn("session dropped; sleeping {}ms before reconnect "
                     "(attempt {})",
                     delay->count(), reconnect.attempts());
        std::this_thread::sleep_for(*delay);
    }
    return 0;
}

int main(int argc, char** argv) {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    spdlog::set_level(spdlog::level::info);

    std::string host    = "127.0.0.1";
    uint16_t    port    = 9443;
    bool        use_tls = false;  // flip to true when pointing at a real peer

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if      (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--tls")                  use_tls = true;
        else if (a == "--no-tls")               use_tls = false;
    }

    spdlog::info("production_client: host={}:{} tls={}", host, port, use_tls);
    return run_session(host, port, use_tls);
}
