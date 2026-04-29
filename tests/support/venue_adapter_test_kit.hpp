/// @file venue_adapter_test_kit.hpp
///
/// Shared scaffolding for the venue-adapter integration tests
/// (`test_okx_adapter`, `test_bybit_adapter`, `test_coinbase_adapter`).
/// Each of those tests independently reinvented the same handful of
/// helpers; this header pulls them into a single place so the only
/// per-venue knobs are the wire-protocol bits (subscribe payload,
/// ack shape, server-side handler).
///
///   - `drive_until` — drive `Poller::poll()` + `Orch::tick()` in a
///     busy loop until a predicate flips true or a deadline elapses.
///   - `encode_ws_text` — encode a complete WS text frame (FIN, masked
///     since this is a client-side frame) into a fresh `std::vector`.
///   - `make_local_tls_ws_config` — build the loopback TLS+WS config
///     used by every venue test (only the WS path varies per venue).
///   - `IncomingSink` — thread-safe collector for codec-emitted frames,
///     plus a `sink()` lambda the test installs as `Stream::on_message`.
///   - `make_stream_factory` — produce the `Orch::Factory` callable
///     that builds a fresh stream per connect attempt and wires the
///     `on_message` sink onto it.
///   - `make_attach` / `make_detach` — produce the attach/detach
///     callables that bridge orchestrator hooks to a poller.
///
/// Templated on the poller / orchestrator types so this header has no
/// build-time dep on `eph-net-kernel` itself; each translation unit
/// that includes this file already has the relevant `KernelPoller` /
/// `ReconnectOrchestrator` headers in scope.
///
/// Header-only by design — the project is header-only and the test
/// scaffolding follows the same invariant.

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "eph/core/error.hpp"
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
    cfg.kernel.tcp_nodelay    = true;
    cfg.tls.hostname          = "eph-test-server";
    cfg.tls.verify_peer       = false;
    cfg.tls.handshake_timeout = 2s;
    cfg.ws.path               = std::string{ws_path};
    cfg.ws.host               = "eph-test-server";
    cfg.ws.timeout            = 2s;
    cfg.ws.permessage_deflate = false;
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

/// Thread-safe collector for codec-emitted text frames. Every venue
/// adapter test does the same dance: build a `vector<string>` + mutex,
/// install an `on_message` lambda that appends a copy of the bytes, then
/// `drive_until` searches the vector under the same mutex. Bundling the
/// pair here keeps the `[&]` capture set in the test body small.
///
/// Usage:
///
/// ```cpp
/// IncomingSink incoming;
/// stream->on_message = incoming.sink();
/// ...
/// drive_until(*poller, orch, [&]() {
///     return incoming.contains("\"event\":\"subscribe\"");
/// });
/// ```
struct IncomingSink {
    std::mutex                 mu;
    std::vector<std::string>   messages;

    /// Return a `std::function<void(std::span<const uint8_t>)>` that
    /// appends a string copy of every received frame to `messages`
    /// under the mutex. Uses `std::function` instead of a generic
    /// lambda so it slots directly into the canonical
    /// `Stream::on_message` field (which is itself a `std::function`).
    [[nodiscard]] std::function<void(std::span<const uint8_t>)> sink() {
        return [this](std::span<const uint8_t> bytes) {
            std::lock_guard lk(mu);
            messages.emplace_back(
                reinterpret_cast<const char*>(bytes.data()), bytes.size());
        };
    }

    /// Count how many messages contain `needle` as a substring. Locks
    /// the mutex internally — safe to call from drive_until predicates.
    [[nodiscard]] std::size_t count_with(std::string_view needle) {
        std::lock_guard lk(mu);
        std::size_t n = 0;
        for (auto const& m : messages) {
            if (m.find(needle) != std::string::npos) ++n;
        }
        return n;
    }

    /// Convenience for the common "ack arrived at least once" check.
    [[nodiscard]] bool contains(std::string_view needle) {
        return count_with(needle) > 0;
    }
};

/// Build the orchestrator `Factory` callable that creates a fresh
/// `Stream` per connect attempt and wires `on_message_handler` onto it
/// before returning. `make_config()` is invoked each call so the test
/// can vary per-attempt knobs (e.g. Coinbase rotates JWT timestamps);
/// for venues that don't need to vary anything just return a constant
/// config.
///
/// `Stream::create(...)` is expected to return
/// `std::expected<unique_ptr<Stream>, ErrorInfo>` — the canonical eph
/// fallible-factory shape. The returned callable matches
/// `ReconnectOrchestrator<Stream>::Factory`.
template <class Stream, class ConfigFn, class OnMessage>
[[nodiscard]] inline auto make_stream_factory(
    ConfigFn make_config, OnMessage on_message_handler)
    -> std::function<std::expected<std::unique_ptr<Stream>,
                                   eph::core::ErrorInfo>()> {
    return [make_config = std::move(make_config),
            on_message_handler = std::move(on_message_handler)]()
        -> std::expected<std::unique_ptr<Stream>, eph::core::ErrorInfo> {
        auto sr = Stream::create(make_config());
        if (!sr) return std::unexpected(sr.error());
        (*sr)->on_message = on_message_handler;
        return std::move(*sr);
    };
}

/// Build the orchestrator `attach` callable that registers the freshly
/// connected stream with `poller`. The returned callable matches
/// `ReconnectOrchestrator<Stream>::Attach` (it returns whatever
/// `Poller::add` returns, which is always `expected<void, ErrorInfo>`
/// for the kernel poller).
template <class Stream, class Poller>
[[nodiscard]] inline auto make_attach(Poller& poller) {
    return [&poller](Stream* s) { return poller.add(s); };
}

/// Build the orchestrator `detach` callable that unregisters a stream
/// from `poller`. Discards the `remove` return because the orchestrator
/// detach hook is signature `void(Stream*)`.
template <class Stream, class Poller>
[[nodiscard]] inline auto make_detach(Poller& poller) {
    return [&poller](Stream* s) { (void)poller.remove(s); };
}

} // namespace eph::test
