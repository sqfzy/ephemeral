/// @file session_reconnect.cpp
///
/// Minimal demonstration of the idiomatic reconnect pattern for eph:
///
///     while (running) {
///         // connect-with-backoff is a generic retry; `when` aborts on shutdown
///         auto stream_r = eph::utils::retry(
///             [&]{ return KernelTcpStream<Codec>::create(cfg); },
///             eph::utils::ExponentialBackoff{{...}},
///             [](const auto&){ return running.load(); });
///         if (!stream_r) break;     // gave up (shutdown requested)
///         run_session(*stream_r);   // protocol layer (Logon, seq sync, ...)
///         // loop back to reconnect; next retry() builds a fresh backoff chain
///     }
///
/// Why the reconnect loop is in the caller, not inside
/// `KernelTcpStream::create()`:
///
///   1. **Session recovery is protocol-layer.** FIX Logon sequence
///      number resync, ITCH snapshot replay, WebSocket resubscription
///      — none of these are visible at the byte-stream layer.
///   2. **Kill-switch / risk gates.** After a drop the strategy may
///      need to consult `eph::utils::KillSwitch` and refuse to
///      reconnect. A stream-local retry cannot see that state.
///   3. **Multi-path routing.** Primary/backup datacenter failover is
///      business logic, not an `ExponentialBackoff::Config` field.
///   4. **Poller supervision.** A stream only becomes observable to
///      the `Poller` after `poller.add(stream)`. A retry loop inside
///      `create()` runs in a blind-spot where no supervisor can
///      enforce timeouts or kill gates.
///
/// `eph::utils::ExponentialBackoff` itself is just exponential-backoff math
/// (see `eph/utils/backoff.hpp`); `eph::utils::retry` is the blocking driver
/// that sleeps between attempts. The connect call and the reconnect loop both
/// stay in the caller — that is what makes them composable with the patterns
/// above (and `eph::net::ReconnectOrchestrator` lifts the whole loop into a
/// non-blocking, poller-friendly state machine when you need it).

#include <atomic>
#include <chrono>
#include <csignal>
#include <span>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/retry.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
namespace eu = eph::utils;
using namespace std::chrono_literals;

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false); }

int main() {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    auto poller = en::KernelPoller::create({}).value();

    en::StreamConfig cfg{};
    cfg.remote          = eph::net::SocketAddr{eph::net::Ipv4Addr{127,0,0,1}, 9000};
    cfg.connect_timeout = 3s;
    cfg.kernel.tcp_nodelay = true;

    const eu::ExponentialBackoff::Config backoff_cfg{
        .initial_backoff = 100ms,
        .max_backoff     = 5s,
        .multiplier      = 2.0,
        .jitter_factor   = 0.25,
        .max_attempts    = 0,  // unlimited — bounded by g_running (see `when`)
    };

    while (g_running.load()) {
        // Connect with exponential backoff via the generic retry driver. The
        // `when` predicate aborts the backoff the instant shutdown is
        // requested, so a SIGTERM during a sleep cycle is honored without an
        // unbounded wait — and each fresh retry() call starts a new backoff
        // chain, so no explicit policy.reset() after a clean session.
        auto sr = eu::retry(
            [&] { return en::KernelTcpStream<ec::RawStreamCodec>::create(cfg); },
            eu::ExponentialBackoff{backoff_cfg},
            [](const eph::core::ErrorInfo&) { return g_running.load(); });
        if (!sr) {
            spdlog::info("giving up reconnect: {} (shutdown requested)",
                         sr.error().detail);
            break;
        }

        auto stream = std::move(*sr);
        // [[maybe_unused]] on the param: SPDLOG_DEBUG compiles out under
        // SPDLOG_ACTIVE_LEVEL=INFO (the release-build default), so
        // app_frame is referenced only at TRACE/DEBUG levels — without
        // the attribute, release builds get -Wunused-parameter.
        stream->on_message = [](std::span<const uint8_t> app_frame [[maybe_unused]]) {
            SPDLOG_DEBUG("rx {} bytes", app_frame.size());
        };
        if (auto r = poller->add(stream.get()); !r) {
            spdlog::error("poller add failed: {}", r.error().detail);
            return 1;
        }

        spdlog::info("connected to {}:{}",
                     cfg.remote.ip.to_string(), cfg.remote.port);

        // Protocol-layer session: this is where a real client runs
        // FIX Logon / ITCH snapshot / WS subscribe before entering
        // the steady-state poll loop.
        while (g_running.load()
               && stream->state() == eph::net::TcpState::Established) {
            (void)poller->poll(100ms);
        }
        (void)poller->remove(stream.get());
        spdlog::info("session dropped (state={})",
                     eph::net::tcp_state_name(stream->state()));
        // Loop back to reconnect: the next retry() builds a fresh backoff
        // chain. Set backoff_cfg.max_attempts to a non-zero value if you want
        // a bounded per-cycle reconnect budget.
    }

    spdlog::info("shutdown");
    return 0;
}
