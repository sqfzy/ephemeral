#pragma once

/// @file handshake_phase.hpp
/// Fine-grained connection-establishment phase for the non-blocking connect
/// state machine shared by both networking backends.
///
/// `HandshakePhase` is a *diagnostic* refinement of `TcpState`: where
/// `state()` reports only the coarse RFC-793 view (`SynSent` for the entire
/// pre-`Established` window, then `Established`), `handshake_phase()` tells an
/// operator exactly which leg of the TCP → TLS → WebSocket handshake chain a
/// stream is currently driving across poll cycles.
///
/// It deliberately lives *outside* the `eph::net::Stream` concept — the
/// concept's `state()` (returning `TcpState`) is the contract the
/// `ReconnectOrchestrator` and user code key off. `HandshakePhase` is an
/// additive accessor (`handshake_phase()`) the kernel and DPDK `TcpStream`
/// backends expose for logging / metrics / tests, so no concept, mock, or
/// Poller signature changes when a backend gains it.
///
/// Phase → coarse `TcpState` mapping the backends maintain:
///
///     Init           → Closed     (constructed, no syscall issued yet)
///     TcpConnecting  → SynSent    (socket()+non-blocking connect() in flight)
///     TcpConnected   → SynSent    (TCP up; TLS/WS not yet started)
///     TlsHandshaking → SynSent    (SSL_do_handshake driving WANT_READ/WRITE)
///     TlsConnected   → SynSent    (TLS up; WS not yet started)
///     WsHandshaking  → SynSent    (RFC 6455 Upgrade request/response in flight)
///     Established    → Established (whole chain up; data may flow)
///     Failed         → Closed     (any leg errored or the deadline expired)
///
/// Lives in `eph-net` (not `eph-core`) because it is purely a networking-layer
/// concept and no module below `eph-net` needs it.

#include <cstdint>

namespace eph::net {

// ---------------------------------------------------------------------------
// HandshakePhase
// ---------------------------------------------------------------------------

/// @brief Which leg of the (TCP → TLS → WebSocket) connect chain a stream is
///        currently driving.
///
/// The enum order follows the canonical progression; a stream advances
/// monotonically through the configured legs (TLS / WS legs are skipped when
/// not configured) and lands on `Established`, or short-circuits to `Failed`
/// on any error or connect-deadline expiry. `TcpConnected` / `TlsConnected`
/// are brief "leg complete, next leg not yet begun" markers — useful so a
/// single poll step that finishes one leg and cannot immediately start the
/// next still reports a meaningful phase.
enum class HandshakePhase : uint8_t {
    Init = 0,        ///< Constructed; no connect syscall issued yet.
    TcpConnecting,   ///< Non-blocking connect() in flight (awaiting writability).
    TcpConnected,    ///< TCP established; TLS/WS legs not yet started.
    TlsHandshaking,  ///< TLS 1.3 handshake driving across poll cycles.
    TlsConnected,    ///< TLS up; WS leg not yet started.
    WsHandshaking,   ///< RFC 6455 Upgrade request/response in flight.
    Established,     ///< Whole configured chain up; application data may flow.
    Failed,          ///< A leg errored or the connect deadline expired.
};

// ---------------------------------------------------------------------------
// handshake_phase_name
// ---------------------------------------------------------------------------

/// @brief Stable, human-readable name for a `HandshakePhase`.
///
/// Returns a static string literal (never needs freeing). Used for logging
/// and test assertions. Unknown values (ABI drift / `reinterpret_cast` abuse)
/// return `"UNKNOWN"`.
[[nodiscard]] constexpr const char* handshake_phase_name(HandshakePhase p) noexcept {
    switch (p) {
        case HandshakePhase::Init:           return "INIT";
        case HandshakePhase::TcpConnecting:  return "TCP_CONNECTING";
        case HandshakePhase::TcpConnected:   return "TCP_CONNECTED";
        case HandshakePhase::TlsHandshaking: return "TLS_HANDSHAKING";
        case HandshakePhase::TlsConnected:   return "TLS_CONNECTED";
        case HandshakePhase::WsHandshaking:  return "WS_HANDSHAKING";
        case HandshakePhase::Established:     return "ESTABLISHED";
        case HandshakePhase::Failed:         return "FAILED";
    }
    return "UNKNOWN";
}

} // namespace eph::net
