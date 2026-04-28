/// @file test_reconnect_orch_integration.cpp
/// Stage-3 integration smoke for `eph::net::ReconnectOrchestrator<S>`.
///
/// Why this file lives in eph-net-kernel/tests (not eph-net/tests as the
/// pax plan originally proposed): eph-net is a header-only concept layer
/// that cannot depend on a concrete backend. The orchestrator is templated,
/// so the *real* concept-instantiation check must run in a target that
/// pulls a backend in. Kernel is the lighter of the two backends and has
/// no NIC requirements, so it is the natural home for this smoke.
///
/// Scenario:
///   1. Bind a TCP echo server on loopback + ephemeral port.
///   2. Create a `ReconnectOrchestrator<KernelTcpStream<RawStreamCodec, false>>`
///      whose factory dials `127.0.0.1:port` and adds the result to a
///      shared `KernelPoller`.
///   3. Drive the poller + tick() until orchestrator reaches Connected.
///   4. Server-side hard-close the connection; orchestrator detects via
///      stream state and reconnects to a fresh server thread.
///   5. Assert reconnect_count >= 1 and current() points at a fresh stream.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <thread>

#include "eph/codec/raw_stream_codec.hpp"
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/reconnect_orchestrator.hpp"
#include "eph/utils/time.hpp"

namespace ek = eph::net::kernel;
namespace en = eph::net;

using PlainStream = ek::KernelTcpStream<eph::codec::RawStreamCodec, false>;
using Orch        = en::ReconnectOrchestrator<PlainStream>;

namespace {

/// Bind loopback + ephemeral, return (fd, port).
std::pair<int, uint16_t> bind_listener() {
    int s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return {-1, 0};
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0
        || ::listen(s, 4) != 0) {
        ::close(s); return {-1, 0};
    }
    socklen_t len = sizeof(sa);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&sa), &len);
    return {s, ::ntohs(sa.sin_port)};
}

/// Accept loop that drops the first `to_drop` clients (immediate close)
/// then holds the next one open until `running` flips false. This lets a
/// single listener fd stay alive across the whole test while the
/// orchestrator's first connect fails-on-disconnect and the second wins.
void accept_drop_then_hold(int lfd, int to_drop,
                           const std::atomic<bool>& running) {
    int dropped = 0;
    while (running.load(std::memory_order_acquire)) {
        sockaddr_in cli{};
        socklen_t clen = sizeof(cli);
        int c = ::accept(lfd, reinterpret_cast<sockaddr*>(&cli), &clen);
        if (c < 0) {
            // accept() blocks; the listener fd close from the test thread
            // wakes us up with EBADF / EINTR. Either way we exit.
            return;
        }
        if (dropped < to_drop) {
            ::shutdown(c, SHUT_RDWR);
            ::close(c);
            ++dropped;
            continue;
        }
        // Hold the surviving client until shutdown.
        while (running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ::shutdown(c, SHUT_RDWR);
        ::close(c);
        return;
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Smoke: orchestrator successfully reconnects after a peer-initiated close.
//
// Exercises the *Stream concept instantiation* path — proves that a real
// KernelTcpStream + KernelPoller pair can be driven by the orchestrator's
// owning + tick-based design.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ReconnectOrchKernelIntegration, ReconnectsAfterPeerClose) {
    eph::utils::TSC::init();

    // Single listener fd lives for the whole test. The accept loop drops
    // the first client immediately (forces orchestrator to detect a peer
    // close) and holds the second client open until we shut down.
    auto [lfd, port] = bind_listener();
    ASSERT_GE(lfd, 0);

    std::atomic<bool> server_running{true};
    std::thread server_thread([lfd, &server_running] {
        accept_drop_then_hold(lfd, /*to_drop=*/1, server_running);
    });

    auto poller = ek::KernelPoller::create().value();

    auto factory = [&]() -> std::expected<Orch::StreamPtr, eph::core::ErrorInfo> {
        ek::StreamConfig cfg{};
        cfg.remote         = en::SocketAddr{en::Ipv4Addr{127, 0, 0, 1}, port};
        cfg.reasm_capacity = 4 * 1024;
        return PlainStream::create(cfg);
    };

    auto attach = [&](PlainStream* s) -> std::expected<void, eph::core::ErrorInfo> {
        return poller->add(s);
    };

    auto detach = [&](PlainStream* s) {
        (void)poller->remove(s);
    };

    Orch orch{
        en::ReconnectConfig{
            .policy = {.initial_backoff = std::chrono::milliseconds(50),
                       .max_backoff     = std::chrono::milliseconds(200),
                       .multiplier      = 2.0,
                       .jitter_factor   = 0.0,
                       .max_attempts    = 5}},
        factory, /*on_disc=*/{}, /*on_recon=*/{}, attach, detach,
    };

    // First connect — server immediately drops the client.
    ASSERT_TRUE(orch.start(eph::utils::TSC::now()).has_value());
    ASSERT_EQ(orch.state(), en::ReconnectState::Connected);

    // Drive the loop until the orchestrator reaches Connected for the
    // SECOND time (count >= 2 means initial connect + 1 reconnect).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)poller->poll(std::chrono::milliseconds(10));
        orch.tick(eph::utils::TSC::now());
        if (orch.reconnect_count() >= 2u
            && orch.state() == en::ReconnectState::Connected) {
            break;
        }
    }

    EXPECT_EQ(orch.state(), en::ReconnectState::Connected);
    EXPECT_GE(orch.reconnect_count(), 2u);  // initial connect + at least 1 reconnect
    ASSERT_NE(orch.current(), nullptr);

    // Tear down cleanly. Stop the orchestrator first so it doesn't try to
    // reconnect again when the server-side closes its surviving client.
    orch.stop();
    if (orch.current()) (void)poller->remove(orch.current());
    server_running.store(false, std::memory_order_release);
    ::shutdown(lfd, SHUT_RDWR);
    ::close(lfd);
    server_thread.join();
}
