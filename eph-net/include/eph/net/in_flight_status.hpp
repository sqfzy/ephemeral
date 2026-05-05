#pragma once

/// @file in_flight_status.hpp
/// Backend-neutral three-state in-flight semantics for `send()` /
/// `send_to()` failure paths. Shared by `eph-net-dpdk` and
/// `eph-net-kernel` backends so HFT applications can write a single
/// reconciliation policy regardless of which backend they're using.
///
/// **Original motivation** (T1.1+T1.2 in the 2026-05-05 action list):
/// in the DPDK backend, when the `eph-nicd` daemon dies mid-send the
/// application needs to know whether the bytes hit the wire (commit
/// + don't retransmit) or were rejected before the burst (safe to
/// retry) — without that distinction, HFT risk controls cannot
/// reconcile state machines after a daemon failure.
///
/// **Generalized motivation** (this header — 2026-05-05 follow-up):
/// the same three-state outcome applies to the kernel backend on
/// `send()` / `sendto()` failure:
///   - `EBADF` / `ENOTCONN` / fd_ < 0 → bytes never left process → `Unsent`
///   - partial send (sock_.send returned `*sr < requested`) → some
///     bytes in kernel buffer, some not → `Uncertain`
///   - `EPIPE` / `ECONNRESET` after full send returned successfully →
///     bytes are committed to kernel buffer (and possibly already on
///     the wire when the peer reset) → `Sent`
/// The kernel backend has no daemon to die, but the in-flight semantic
/// is the same — application code wants the same three-state answer.
///
/// **Cross-backend symmetry**: when an HFT app switches between
/// DPDK and kernel backends, the only differences should be hot-path
/// performance characteristics — NOT the recovery state machine. This
/// header makes the recovery state machine identical.
///
/// **Lifetime**: storage is `thread_local`. Each subsequent detection
/// on the same thread overwrites the previous detail. Application code
/// reads it immediately after observing an error — before issuing the
/// next send.
///
/// **Backwards compatibility**: the previous DPDK-specific header at
/// `eph-net-dpdk/include/eph/net/dpdk/detail/daemon_disconnected_hook.hpp`
/// is preserved as a re-export of the symbols in this header (same
/// `thread_local` storage; same enum tags; existing call sites
/// continue to work without recompilation).

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace eph::net {

/// @brief Three-state classification of in-flight bytes when a `send()`
///        / `send_to()` call returns an error.
enum class InFlightStatus : uint8_t {
    /// Bytes never left this process. Safe to retry on a fresh
    /// connection / `Stream::create` round-trip.
    Unsent = 0,

    /// Bytes are committed to the wire (DPDK: `rte_eth_tx_burst`
    /// returned positive count; kernel: `send`/`sendto` returned
    /// the full requested size before the failure detector fired).
    /// The application should consider the data delivered; do NOT
    /// retransmit, lest the peer see a duplicate.
    Sent = 1,

    /// Bytes may or may not be on the wire. Typically:
    ///   - DPDK: tx burst returned a partial count and the daemon
    ///     died before the retry path completed.
    ///   - kernel: `sock_.send` returned `*sr < requested` and a
    ///     subsequent `EPIPE` / `ECONNRESET` indicates the peer reset.
    /// Application options:
    ///   (a) Higher-layer sequence number → dedupe at peer. Preferred
    ///       for HFT order flow (use client_order_id).
    ///   (b) Accept duplication risk and retransmit blindly.
    ///   (c) Tear down and rebuild the higher-layer session entirely.
    Uncertain = 2,
};

[[nodiscard]] constexpr const char*
to_string(InFlightStatus s) noexcept {
    switch (s) {
        case InFlightStatus::Unsent:    return "Unsent";
        case InFlightStatus::Sent:      return "Sent";
        case InFlightStatus::Uncertain: return "Uncertain";
    }
    return "Unknown";
}

/// @brief POD describing the burst-time / send-time context of an
///        in-flight detection.
///
/// `phase` carries a short backend-specific tag identifying the call
/// site (e.g. `"DpdkTcpStream::send"`, `"KernelTcpStream::send"`,
/// `"KernelUdpSocket::send_to"`). Storage is a string literal in
/// .rodata; not freed.
struct InFlightDetail {
    InFlightStatus status         = InFlightStatus::Unsent;
    /// Bytes the application asked to send.
    size_t         bytes_observed = 0;
    /// Bytes the underlying transport confirmed sent.
    size_t         bytes_confirmed = 0;
    const char*    phase = "";
    /// Wall-clock timestamp (steady_clock epoch ns) at the moment the
    /// detection fired. 0 if unstamped.
    uint64_t       detected_at_ns = 0;
};

/// @brief Read the most-recent `InFlightDetail` populated on this
///        thread by a backend's send-path failure detection.
[[nodiscard]] inline InFlightDetail&
last_in_flight_detail() noexcept {
    thread_local InFlightDetail detail{};
    return detail;
}

/// @brief Stamp `last_in_flight_detail()` with a new detection, taking
///        a wall-clock timestamp from steady_clock automatically.
inline void
set_in_flight_detail(InFlightStatus status,
                     size_t bytes_observed,
                     size_t bytes_confirmed,
                     const char* phase) noexcept {
    auto& d = last_in_flight_detail();
    d.status          = status;
    d.bytes_observed  = bytes_observed;
    d.bytes_confirmed = bytes_confirmed;
    d.phase           = (phase != nullptr) ? phase : "";
    using ns = std::chrono::nanoseconds;
    d.detected_at_ns  = static_cast<uint64_t>(
        std::chrono::duration_cast<ns>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// @brief Reset the thread-local detail (useful in tests + before
///        starting a new burst loop).
inline void
clear_in_flight_detail() noexcept {
    last_in_flight_detail() = InFlightDetail{};
}

}  // namespace eph::net
