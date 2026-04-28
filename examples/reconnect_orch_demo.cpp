/// @file reconnect_orch_demo.cpp
///
/// Minimal `eph::net::ReconnectOrchestrator<S>` demo against an in-process
/// loopback echo server. Doubles as documentation for the multi-venue
/// motivation (`docs/architecture.md`-style) without depending on any
/// real exchange.
///
/// What you see:
///   - One listener thread that drops the first client (forces a
///     reconnect path).
///   - One orchestrator wrapping a `KernelTcpStream<RawStreamCodec>`,
///     resolving its own factory + attach + detach callbacks.
///   - The main loop drives `poller->poll()` followed by `orch.tick()`,
///     watching `reconnect_count` cross 2 (initial connect + 1 reconnect).
///
/// Run:  `./reconnect_orch_demo` (no flags)

#include <atomic>
#include <chrono>
#include <expected>
#include <iostream>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/reconnect_orchestrator.hpp"
#include "eph/utils/time.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;
using PlainStream = ek::KernelTcpStream<eph::codec::RawStreamCodec, false>;
using Orch        = en::ReconnectOrchestrator<PlainStream>;
using namespace std::chrono_literals;

static std::pair<int, uint16_t> bind_loopback() {
    int s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    ::listen(s, 4);
    socklen_t len = sizeof(sa);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&sa), &len);
    return {s, ntohs(sa.sin_port)};
}

int main() {
    eph::utils::TSC::init();

    auto [lfd, port] = bind_loopback();
    std::cout << "[demo] listener bound on 127.0.0.1:" << port << "\n";

    std::atomic<bool> running{true};
    std::thread server([lfd, &running] {
        for (int dropped = 0; running.load(); ) {
            sockaddr_in cli{};
            socklen_t clen = sizeof(cli);
            int c = ::accept(lfd, reinterpret_cast<sockaddr*>(&cli), &clen);
            if (c < 0) return;
            if (dropped++ == 0) {
                ::shutdown(c, SHUT_RDWR); ::close(c);  // first client: drop
                continue;
            }
            while (running.load()) std::this_thread::sleep_for(20ms);
            ::shutdown(c, SHUT_RDWR); ::close(c);
            return;
        }
    });

    auto poller = ek::KernelPoller::create().value();

    Orch orch{
        en::ReconnectConfig{.policy = {.initial_backoff = 50ms,
                                       .max_backoff = 500ms,
                                       .jitter_factor = 0.0}},
        /*factory=*/[&]() -> std::expected<Orch::StreamPtr, eph::core::ErrorInfo> {
            ek::StreamConfig cfg{};
            cfg.remote = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
            cfg.reasm_capacity = 4096;
            return PlainStream::create(cfg);
        },
        /*on_disconnect=*/[](const eph::core::ErrorInfo& e) {
            std::cout << "[demo] disconnected: " << e.detail << "\n";
        },
        /*on_reconnect=*/[](uint32_t a, uint64_t dur_ns) {
            std::cout << "[demo] reconnected on attempt " << a
                      << " (duration " << dur_ns << " ns)\n";
        },
        /*attach=*/[&](PlainStream* s) { return poller->add(s); },
        /*detach=*/[&](PlainStream* s) { (void)poller->remove(s); },
    };

    (void)orch.start(eph::utils::TSC::now());

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline
           && !(orch.reconnect_count() >= 2
                && orch.state() == en::ReconnectState::Connected)) {
        (void)poller->poll(10ms);
        orch.tick(eph::utils::TSC::now());
    }

    std::cout << "[demo] final state=" << en::to_string(orch.state())
              << " count=" << orch.reconnect_count()
              << " failures=" << orch.reconnect_failures()
              << " last_dur_ns=" << orch.last_reconnect_duration_ns() << "\n";

    orch.stop();
    if (orch.current()) (void)poller->remove(orch.current());
    running.store(false);
    ::shutdown(lfd, SHUT_RDWR); ::close(lfd);
    server.join();
    return orch.reconnect_count() >= 2 ? 0 : 1;
}
