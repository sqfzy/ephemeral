/// @file venue_adapter_test_kit.hpp
///
/// Shared scaffolding for the venue-adapter integration tests
/// (`test_okx_adapter`, `test_bybit_adapter`, `test_coinbase_adapter`).
/// Each of those tests independently reinvented two helpers:
///
///   - `drive_until` — drive `Poller::poll()` + `Orch::tick()` in a busy
///     loop until a predicate flips true or a deadline elapses.
///   - `encode_ws_text` — encode a complete WS text frame (FIN, masked
///     since this is a client-side frame) into a fresh `std::vector`.
///
/// Both are pulled in here so they live in **one** place. The signatures
/// stay templated on poller / orchestrator types so this header has no
/// build-time dep on `eph-net-kernel` headers themselves; each translation
/// unit that includes this file already has the relevant `KernelPoller` /
/// `ReconnectOrchestrator` headers in scope.
///
/// Header-only by design — the project is header-only and the test
/// scaffolding follows the same invariant.

#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "eph/net/detail/websocket.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/utils/time.hpp"

namespace eph::test {

/// Build a `KernelTcpStream` config dialing 127.0.0.1:`port` against the
/// in-process `TlsWsEchoServer`. The TLS hostname and the WS Host header
/// both pin to `eph-test-server` (the SAN burned into the test cert by
/// `tls_ws_echo_server.hpp`); `ws_path` is the only knob that varies
/// across venues. Permessage-deflate is disabled because the test server
/// does not echo `Sec-WebSocket-Extensions` and the codec's deflate state
/// tracking would just be noise.
[[nodiscard]] inline eph::net::kernel::StreamConfig make_local_tls_ws_config(
    std::uint16_t port, std::string_view ws_path) {
    using namespace std::chrono_literals;
    eph::net::kernel::StreamConfig cfg{};
    cfg.remote                = eph::net::SocketAddr{
        eph::net::Ipv4Addr{127, 0, 0, 1}, port};
    cfg.reasm_capacity        = 64 * 1024;
    cfg.connect_timeout       = 2s;
    cfg.tcp_nodelay           = true;
    cfg.tls.hostname          = "eph-test-server";
    cfg.tls.verify_peer       = false;
    cfg.tls.handshake_timeout = 2s;
    cfg.ws_path               = std::string{ws_path};
    cfg.ws_host               = "eph-test-server";
    cfg.ws_timeout            = 2s;
    cfg.ws_permessage_deflate = false;
    return cfg;
}

/// Drive `poller.poll()` + `orch.tick()` in a 10 ms-stepped loop until
/// `pred()` returns true or `budget` elapses. Returns whether `pred`
/// flipped true within the budget.
///
/// Templated on `Poller`, `Orch`, and `Pred` so this header doesn't have
/// to depend on `KernelPoller` / `ReconnectOrchestrator` directly. Both
/// types must expose:
///   - `poller.poll(std::chrono::milliseconds)` (return value ignored)
///   - `orch.tick(uint64_t tsc)` (return value ignored)
///
/// The 10 ms poll budget matches what each per-venue copy used.
template <class Poller, class Orch, class Pred>
[[nodiscard]] inline bool drive_until(
    Poller& poller, Orch& orch, Pred pred,
    std::chrono::milliseconds budget = std::chrono::seconds(5)) {
    using namespace std::chrono_literals;
    const auto end = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < end) {
        (void)poller.poll(10ms);
        orch.tick(eph::utils::TSC::now());
        if (pred()) return true;
    }
    return false;
}

/// Encode a complete WS text frame (FIN, opcode=text, masked) carrying
/// `payload` into a fresh `std::vector<uint8_t>`. The 14-byte oversize is
/// the maximum WS frame header (2 length + 8 ext-length + 4 mask) so the
/// underlying `eph::net::ws::encode_frame` always has room.
[[nodiscard]] inline std::vector<uint8_t> encode_ws_text(
    std::string_view payload) {
    std::vector<uint8_t> out;
    out.resize(payload.size() + 14);
    auto n = eph::net::ws::encode_frame(
        out.data(), eph::net::ws::opcode::kText,
        reinterpret_cast<const uint8_t*>(payload.data()),
        payload.size(), /*fin=*/true);
    out.resize(n);
    return out;
}

} // namespace eph::test
