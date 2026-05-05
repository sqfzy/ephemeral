#pragma once

/// @file detail/daemon_disconnected_hook.hpp
/// In-flight semantics for `Error::DaemonDisconnected` from rx/tx burst
/// paths (T1.1 + T1.2 from the 2026-05-05 action list).
///
/// Background:
///   The daemon-led model (post-2026-05-02 reshape) lets `eph-nicd` die
///   independently of a tenant. When that happens, `Platform::is_alive()`
///   flips to false. Until this header landed, the only way to surface
///   the dead-daemon condition was an external watchdog calling
///   `Platform::is_alive()` and short-circuiting the poll loop. The
///   rx/tx burst paths themselves did not yet check, so an in-flight
///   `send()` could enqueue bytes into the NIC's TX path at the very
///   moment the daemon released the shared mempool — best case the
///   bytes hit the wire and the app sees success; worst case the bytes
///   are silently dropped because the per-queue ring is mid-teardown.
///
/// What this header provides:
///   1. `InFlightStatus` — three-state enum the app needs to reconcile
///      its own state machine on a `DaemonDisconnected` error:
///        - `Sent`      — bytes are on the wire (TX-burst returned > 0
///                        before is_alive() flipped; data is committed).
///        - `Unsent`    — bytes never left this process (alive check
///                        happened before TX-burst; safe to retry).
///        - `Uncertain` — bytes may or may not be on the wire (e.g.
///                        TX-burst returned partial; the failed slice
///                        is in this state). The app SHOULD treat
///                        Uncertain as "may have been received by peer
///                        already" and either dedupe via a higher-layer
///                        sequence number or accept duplication risk.
///   2. `DaemonDisconnectedDetail` — POD describing what the burst was
///      doing when it noticed.
///   3. `thread_local` storage for the most-recent detail so callers
///      can read it after observing `Error::DaemonDisconnected`. The
///      thread_local lifetime matches the burst invocation cadence —
///      each new detection overwrites the previous one. App code reads
///      it immediately after the failed call, before the next burst.
///
/// Wire-up (this commit, additive):
///   - `DpdkTcpStream::send`             — pre-burst is_alive() check
///                                          → fills detail with
///                                          `Unsent`, returns error.
///   - `DpdkUdpSocket::send_to`          — same.
///   - `DpdkPoller::poll`                — once-per-cycle is_alive()
///                                          short-circuit (detail not
///                                          set; poll() returns 0
///                                          dispatched, app consults
///                                          `Platform::is_alive()`).
///
/// What this header does NOT do (deferred to a follow-up `--deep` session):
///   - Mid-burst detection inside `rte_eth_tx_burst` / `rte_eth_rx_burst`
///     (those are DPDK PMD calls; injecting checks mid-burst would
///     require either a DPDK API change or burst sizing of 1 which is a
///     latency regression).
///   - `Sent` status detection — needs the burst path to return both
///     success AND the daemon-died flag, which the additive design here
///     does not provide. For now the wire-up only generates `Unsent`
///     (pre-burst check) and `Uncertain` (handler hooks; app populates
///     these on observed partial sends).
///   - State reconciliation API — application's responsibility per
///     `docs/dpdk-reconnect-pattern.md`.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace eph::net::dpdk::detail {

/// @brief Three-state classification of in-flight bytes when
///        `Error::DaemonDisconnected` is returned.
enum class InFlightStatus : uint8_t {
    /// Bytes never left this process. Safe to retry on a fresh
    /// `Platform::create` / `Stream::create_and_attach` round-trip.
    Unsent = 0,

    /// Bytes are committed to the wire (`rte_eth_tx_burst` returned
    /// positive count before is_alive() flipped). The application
    /// should consider the data delivered; do NOT retransmit, lest the
    /// peer see a duplicate.
    Sent = 1,

    /// Bytes may or may not be on the wire. Typically:
    ///   - rx burst aborted partway through processing a multi-mbuf
    ///     batch (some mbufs decoded, some lost in the post-detection
    ///     drop)
    ///   - tx burst returned a partial count (k < requested) AND the
    ///     daemon dies before the retry path completes — k bytes are
    ///     Sent, the rest is Unsent, but from the app's perspective
    ///     the whole call resolves as Uncertain and dedupe / sequence
    ///     reconciliation is the app's responsibility.
    /// Application options:
    ///   (a) Higher-layer sequence number → dedupe at peer. Preferred.
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

/// @brief POD describing the burst-time context of a
///        `DaemonDisconnected` detection.
struct DaemonDisconnectedDetail {
    InFlightStatus status         = InFlightStatus::Unsent;
    /// Bytes the application asked to send (or the burst was about to
    /// receive). Includes any payload still buffered inside the
    /// stream's reasm/codec pipeline; not just the wire-level count.
    size_t         bytes_observed = 0;
    /// Bytes the kernel/PMD has confirmed delivered (TX-side: count
    /// returned by `rte_eth_tx_burst` * mbuf_payload; RX-side: count
    /// already passed to the codec via `on_message`/`on_datagram`).
    size_t         bytes_confirmed = 0;
    /// Free-form short tag describing where the detection fired.
    /// Examples: `"tcp_send_pre_burst"`, `"udp_send_pre_burst"`,
    /// `"poller_cycle_boundary"`. Never null. Storage lives in the
    /// caller's `.rodata` (string literal); not freed.
    const char*    phase = "";
    /// Wall-clock timestamp in nanoseconds since the steady_clock epoch
    /// at the moment the detection fired. Zero if the caller did not
    /// stamp it.
    uint64_t       detected_at_ns = 0;
};

/// @brief Read the most-recent `DaemonDisconnectedDetail` populated on
///        this thread by a burst-path detection.
///
/// Lifetime: the storage is `thread_local` and lives as long as the
/// thread does. Each subsequent detection on the same thread
/// overwrites the previous detail. Application code reads this
/// immediately after observing `Error::DaemonDisconnected` and copies
/// what it needs.
///
/// Note: the storage is per-thread. If the application's burst loop
/// runs on lcore X and the diagnostic / recovery path runs on a
/// different thread, the detail must be copied across the boundary
/// explicitly — the daemon-disconnected path is a normal control-flow
/// return, not a signal.
[[nodiscard]] inline DaemonDisconnectedDetail&
last_daemon_disconnected_detail() noexcept {
    thread_local DaemonDisconnectedDetail detail{};
    return detail;
}

/// @brief Convenience: stamp `last_daemon_disconnected_detail()` with a
///        new detection.
///
/// Call site: the moment after `Platform::is_alive()` returns false in
/// the burst path. Caller fills in everything it can; the helper
/// stamps `detected_at_ns` from steady_clock.
inline void
set_daemon_disconnected_detail(InFlightStatus status,
                               size_t bytes_observed,
                               size_t bytes_confirmed,
                               const char* phase) noexcept {
    auto& d = last_daemon_disconnected_detail();
    d.status         = status;
    d.bytes_observed = bytes_observed;
    d.bytes_confirmed = bytes_confirmed;
    d.phase          = (phase != nullptr) ? phase : "";
    using ns = std::chrono::nanoseconds;
    d.detected_at_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<ns>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// @brief Reset the thread-local detail (useful in tests + before
///        starting a new burst loop).
inline void
clear_daemon_disconnected_detail() noexcept {
    last_daemon_disconnected_detail() = DaemonDisconnectedDetail{};
}

}  // namespace eph::net::dpdk::detail
