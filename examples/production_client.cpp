/// @file production_client.cpp
///
/// Shows the production-quality knobs a real HFT client wires onto a
/// KernelTcpStream: TLS on, TCP_NODELAY, bounded reasm buffer,
/// signal-driven shutdown, and an outer reconnect loop driven by
/// `eph::net::ReconnectPolicy`. The reconnect loop lives in the caller —
/// the stream layer itself does not retry (by design: real recovery
/// needs protocol-layer state the stream cannot see, e.g. FIX Logon
/// seq numbers, kill-switch gates, primary/backup routing).
///
/// No exchange-specific logic — the file is deliberately a template
/// that a strategy can drop into.

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
#include "eph/net/reconnect_policy.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/net/stream_metrics.hpp"

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
// `true`  = session ended cleanly on signal (outer loop should stop)
// `false` = session dropped on its own (outer loop should reconnect)

template <bool EnableTls>
static bool run_one_session(en::StreamConfig& cfg,
                            en::KernelPoller& poller) {
    auto sr = en::KernelTcpStream<ec::WsCodec, EnableTls>::create(cfg);
    if (!sr) {
        spdlog::error("create failed: {}", sr.error().detail);
        return false;  // reconnect
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
        return false;
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
    return !g_running.load(std::memory_order_acquire);
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
    cfg.tcp_nodelay     = true;

    // Reconnect policy lives HERE in the caller — not on the stream
    // config. This is deliberate: after a drop, real recovery needs
    // protocol-level state (FIX Logon seq num resync, ITCH snapshot
    // replay, kill-switch check) that a stream-local retry cannot see.
    // Production-sane defaults: exponential back-off, unbounded
    // attempts, ±25% jitter.
    eph::net::ReconnectPolicy reconnect{eph::net::ReconnectPolicyConfig{}};

    while (g_running.load(std::memory_order_acquire)
           && reconnect.should_reconnect()) {
        const bool stop = use_tls ? run_one_session<true>(cfg, *poller)
                                  : run_one_session<false>(cfg, *poller);
        if (stop) break;

        // Session dropped (or create failed). Sleep with jitter before
        // trying again. A real strategy would re-check kill-switch,
        // consult a backup remote, or refresh credentials here before
        // reconnecting.
        const auto delay = reconnect.next_backoff();
        spdlog::warn("session dropped; sleeping {}ms before reconnect "
                     "(attempt {})",
                     delay.count(), reconnect.attempts());
        std::this_thread::sleep_for(delay);
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
