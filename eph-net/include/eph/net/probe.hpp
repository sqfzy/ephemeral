#pragma once

/// @file probe.hpp
/// Zero-overhead instrumentation probes for Transport pipeline measurement.
///
/// The TransportProbe concept defines 4 timestamp injection points in the
/// Transport data path.  Users supply a Probe type as a template parameter
/// to Transport; the default NullProbe compiles to nothing (all calls are
/// guarded by `if constexpr (!std::same_as<Probe, NullProbe>)`).
///
/// Probe points and their semantics:
///
///   on_tx_enqueue(tsc)  — app thread, immediately after SPSC produce succeeds
///   on_tx_flush(tsc)    — TX  thread, after frame encode + TLS encrypt,
///                          just before TCP send
///   on_rx_arrival(tsc)  — RX  thread, after TLS decrypt + WS deframe,
///                          before deliver_message / callback
///   on_rx_deliver(tsc)  — app thread, when recv() dequeues a message;
///                          or RX thread, when on_message callback fires
///
/// All callbacks receive a raw TSC cycle count (from `TSC::now()` or
/// equivalent).  The probe implementation decides how to store / aggregate.
///
/// Thread safety: on_tx_enqueue is called from the app thread; on_tx_flush
/// from the TX thread; on_rx_arrival and on_rx_deliver from the RX thread
/// (or app thread for recv()-based delivery).  A probe implementation must
/// be safe for these access patterns.

#include <cstdint>
#include <type_traits>

namespace eph::net {

// ---------------------------------------------------------------------------
// TransportProbe concept
// ---------------------------------------------------------------------------

/// A type satisfying TransportProbe can be used as the Probe template
/// parameter of Transport<>.  All four methods must be noexcept.
template <typename P>
concept TransportProbe = requires(P& p, uint64_t tsc) {
    { p.on_tx_enqueue(tsc) } noexcept;
    { p.on_tx_flush(tsc) } noexcept;
    { p.on_rx_arrival(tsc) } noexcept;
    { p.on_rx_deliver(tsc) } noexcept;
};

// ---------------------------------------------------------------------------
// NullProbe — zero-overhead default
// ---------------------------------------------------------------------------

/// Default probe that compiles to nothing.  All methods are constexpr
/// empty inlines; additionally, Transport guards every probe call site
/// with `if constexpr (!std::same_as<Probe, NullProbe>)` so that even
/// the TSC::now() call preceding the probe is eliminated.
struct NullProbe {
    constexpr void on_tx_enqueue(uint64_t) noexcept {}
    constexpr void on_tx_flush(uint64_t)   noexcept {}
    constexpr void on_rx_arrival(uint64_t) noexcept {}
    constexpr void on_rx_deliver(uint64_t) noexcept {}
};

static_assert(TransportProbe<NullProbe>,
              "NullProbe must satisfy TransportProbe concept");

} // namespace eph::net
