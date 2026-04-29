/// @file session_reconnect.cpp
///
/// Minimal demonstration of the idiomatic reconnect pattern for eph:
///
///     ReconnectPolicy policy{ReconnectPolicyConfig{...}};
///     while (policy.should_reconnect()) {
///         auto stream_r = KernelTcpStream<Codec>::create(cfg);
///         if (stream_r) {
///             policy.reset();       // back to initial_backoff
///             run_session(*stream_r); // protocol layer (Logon, seq sync, ...)
///             continue;             // loop to reconnect after clean drop
///         }
///         std::this_thread::sleep_for(policy.next_backoff());
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
///      business logic, not a `ReconnectPolicyConfig` field.
///   4. **Poller supervision.** A stream only becomes observable to
///      the `Poller` after `poller.add(stream)`. A retry loop inside
///      `create()` runs in a blind-spot where no supervisor can
///      enforce timeouts or kill gates.
///
/// `ReconnectPolicy` itself is just exponential-backoff math (see
/// `eph/net/reconnect_policy.hpp`). It does not sleep, does not call
/// connect — the caller drives both. That is what makes it trivially
/// unit-testable and composable with the patterns above.

#include <atomic>
#include <chrono>
#include <csignal>
#include <span>
#include <thread>

#include <spdlog/spdlog.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/reconnect_policy.hpp"
#include "eph/net/socket_addr.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
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

    eph::net::ReconnectPolicy policy{eph::net::ReconnectPolicyConfig{
        .initial_backoff = 100ms,
        .max_backoff     = 5s,
        .multiplier      = 2.0,
        .jitter_factor   = 0.25,
        .max_attempts    = 0,  // unlimited — outer loop bounded by g_running
    }};

    while (g_running.load() && policy.should_reconnect()) {
        auto sr = en::KernelTcpStream<ec::RawStreamCodec>::create(cfg);
        if (!sr) {
            const auto delay = policy.next_backoff();
            spdlog::warn("connect failed: {} — retry in {}ms (attempt {})",
                         sr.error().detail, delay.count(), policy.attempts());
            std::this_thread::sleep_for(delay);
            continue;
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

        // Connected — reset the backoff so the NEXT failure starts
        // fresh instead of inheriting the previous chain's delay.
        policy.reset();
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
        // Loop back to reconnect. `policy.should_reconnect()` stays
        // true because max_attempts==0; use a non-zero max_attempts
        // if you want the loop to eventually give up.
    }

    spdlog::info("shutdown");
    return 0;
}
