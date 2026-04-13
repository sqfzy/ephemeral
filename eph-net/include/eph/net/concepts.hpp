#pragma once

/// @file concepts.hpp
/// `Pollable` / `Stream` / `Datagram` / `Poller` concepts for `eph::net`.
///
/// Defines the narrow
/// waist between the two networking backends (kernel via epoll, DPDK via
/// burst-poll) and user code. Zero virtual dispatch: everything is a template
/// concept satisfied by concrete per-backend types such as
/// `eph::net::kernel::TcpStream<C>` and `eph::net::dpdk::TcpStream<C>`.
///
/// Concept topology:
///
///     Pollable ── base interface all I/O objects implement so a Poller can
///                 drive them. Associated `PacketView` type enables zero-copy.
///     Stream ── refines Pollable with TCP-style `send` / `close_gracefully`
///               / `state` + an `on_message` callback.
///     Datagram ── refines Pollable with UDP-style `send_to` /
///                 `join_multicast` / `leave_multicast` + an `on_datagram`
///                 callback.
///     Poller ── add/remove/poll: the multiplexer that drives Pollable
///               objects in a tight loop.
///
/// Dependency note: `eph-net` DOES NOT depend on `eph-codec`. The concrete
/// `CodecType` associated type is a type-level handle only; consumers that
/// need codec implementations link against `eph-codec` themselves.

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

#include "eph/core/error.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/net/tcp_state.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// Utility: saturate a size_t to uint16_t (for on_message / on_datagram len).
// ---------------------------------------------------------------------------

/// @brief Clamp a size_t frame length to uint16_t range without truncation.
[[nodiscard]] constexpr uint16_t saturate_u16(std::size_t n) noexcept {
    return static_cast<uint16_t>(n > 0xFFFFu ? 0xFFFFu : n);
}

// ---------------------------------------------------------------------------
// Pollable
// ---------------------------------------------------------------------------

/// @brief Internal interface every I/O object implements so a `Poller` can
///        drive it.
///
/// A `Pollable` type exposes:
///   - an associated `PacketView` type describing how incoming bytes are
///     presented (contiguous `span` for kernel, mbuf-backed for DPDK);
///   - a `poll_once_()` method that advances the object by one I/O step and
///     returns the number of frames/packets handled;
///   - an `is_attached_()` method reporting whether the object is currently
///     registered with a Poller;
///   - a `native_handle()` method returning the backend-specific identifier
///     (kernel: `int fd`, DPDK: mbuf pool / connection id as `void*`).
///
/// These methods are conceptually private to the Poller — the convention is
/// to declare them `private` plus `friend Poller`. For concept-checking
/// purposes we require only that they be callable on a non-const `T&`.
/// Backends may relax/tighten the access control as needed; the concept
/// cares only that the expressions are well-formed.
///
/// All three methods require `noexcept` — polling is the tightest inner loop
/// and must not throw. Every existing implementation already satisfies this;
/// the concept now enforces what the code guarantees.
template <class T>
concept Pollable = requires(T& t) {
    typename T::PacketView;

    // One I/O step; returns number of packets/frames processed.
    { t.poll_once_() } noexcept -> std::convertible_to<std::size_t>;

    // Attachment probe: true iff currently registered with a Poller.
    { t.is_attached_() } noexcept -> std::convertible_to<bool>;

    // Backend-specific handle. Kernel: reinterpret as int. DPDK: conn-id /
    // mbuf-pool pointer. We type-erase via `void*` at the concept level so
    // eph-net does not leak kernel fd vs DPDK mbuf details into the waist.
    { t.native_handle() } noexcept -> std::convertible_to<void*>;
};

// ---------------------------------------------------------------------------
// Stream (TCP-style)
// ---------------------------------------------------------------------------

/// @brief Connection-oriented byte-stream concept (TCP, TLS, WebSocket).
///
/// A `Stream` is always also a `Pollable`. Conceptually:
///   - `send(data)`           — write bytes to the peer
///   - `close_gracefully()`   — initiate clean shutdown (FIN/TLS close_notify/
///                              WS Close frame as appropriate)
///   - `is_attached()`        — public attach probe (mirrors Pollable's)
///   - `state()`              — current `TcpState`
///   - `on_message`           — callback invoked by `poll_once_()` when the
///                              codec emits a complete frame
///
/// Subsumption: every `Stream` is a `Pollable`, so code constrained on
/// `Pollable` implicitly accepts `Stream` values. This lets a single Poller
/// drive heterogeneous `Stream` and `Datagram` objects.
template <class T>
concept Stream = Pollable<T> && requires(T& t, std::span<const uint8_t> data) {
    typename T::CodecType;
    typename T::OnMessage;

    { t.send(data) } noexcept
        -> std::same_as<std::expected<std::size_t, core::ErrorInfo>>;
    { t.close_gracefully() } noexcept
        -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.is_attached() } noexcept -> std::convertible_to<bool>;
    { t.state() } noexcept      -> std::same_as<TcpState>;

    // on_message must be a member field (or ref-returning accessor) convertible
    // to the associated OnMessage type. We require it be a modifiable target so
    // user code can assign a lambda.
    { t.on_message }    -> std::convertible_to<typename T::OnMessage>;
};

// ---------------------------------------------------------------------------
// Datagram (UDP-style)
// ---------------------------------------------------------------------------

/// @brief Connectionless message concept (UDP, MoldUDP64, raw L2).
///
/// A `Datagram` is always also a `Pollable`. Distinct from `Stream` because:
///   - each `send_to` targets a specific peer, so there is no implicit
///     "connected" remote;
///   - there is no graceful shutdown — UDP is stateless;
///   - multicast group management (`join`/`leave`) is in scope;
///   - the receive callback observes a `SocketAddr` source.
template <class T>
concept Datagram = Pollable<T> && requires(T& t,
                                           std::span<const uint8_t> data,
                                           const SocketAddr& dst,
                                           const SocketAddr& mcast) {
    typename T::CodecType;
    typename T::OnDatagram;

    { t.send_to(data, dst) } noexcept
        -> std::same_as<std::expected<std::size_t, core::ErrorInfo>>;
    { t.join_multicast(mcast) } noexcept
        -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.leave_multicast(mcast) } noexcept
        -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { t.is_attached() } noexcept -> std::convertible_to<bool>;
    { t.on_datagram }   -> std::convertible_to<typename T::OnDatagram>;
};

// ---------------------------------------------------------------------------
// Poller
// ---------------------------------------------------------------------------

/// @brief I/O multiplexer that drives registered `Pollable` objects.
///
/// The concept requires:
///   - `add<P>(P* obj)`     — register a Pollable; returns error on duplicate
///                             or OOM
///   - `remove<P>(P* obj)`  — unregister a Pollable
///   - `poll()`             — advance all registered objects by one I/O step,
///                             returning the aggregate number of packets
///                             processed
///
/// A `poll(std::chrono::milliseconds)` overload is optional — kernel pollers
/// (epoll) support blocking with a timeout, DPDK pollers do not (they are
/// always busy-poll).
///
/// Note on templated methods: the `add`/`remove` expressions are concept
/// requirements with a concrete `Pollable` argument type. We use the
/// `eph::net::test::FakeStream` signature by constraining via a helper
/// `Pollable` template parameter `P` inside the `requires` clause.
template <class T>
concept Poller = requires(T& p) {
    { p.poll() } noexcept -> std::convertible_to<std::size_t>;
};

/// @brief Concept-level probe that `P` can register a specific `Pollable`
///        type `Obj` with its `add` / `remove` methods.
///
/// This is a finer-grained refinement of `Poller` used by tests and by the
/// kernel / dpdk backends to statically verify that a given Pollable type is
/// accepted by a given Poller. We keep it separate from `Poller` itself so
/// that the bare `Poller<T>` concept stays cheap to check and usable as a
/// function-parameter constraint without knowing the Pollable type.
template <class T, class Obj>
concept PollerOf = Poller<T> && Pollable<Obj> && requires(T& p, Obj* obj) {
    { p.add(obj) } noexcept    -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.remove(obj) } noexcept -> std::same_as<std::expected<void, core::ErrorInfo>>;
};

} // namespace eph::net
