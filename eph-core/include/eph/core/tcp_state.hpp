#pragma once

/// @file tcp_state.hpp
/// Canonical `eph::net::TcpState` enum + `tcp_state_name` helper.
///
/// Lives in `eph-core` (not `eph-net`) so that the dependency direction
/// is correct: every module that needs `TcpState` already depends on
/// `eph-core`, which is the universal substrate. The enum is declared
/// in the `eph::net::` namespace because that is where the rest of the
/// v3.3 networking concepts live (Stream, Datagram, Poller).
///
/// This header resolves an ODR conflict between `eph/core/tcp_concept.hpp`
/// and `eph/net/tcp_state.hpp` — both now forward here so the enum has
/// exactly one definition per TU.

#include <cstdint>

namespace eph::net {

// ---------------------------------------------------------------------------
// TcpState
// ---------------------------------------------------------------------------

/// @brief TCP connection state, verbatim from RFC 793 §3.2.
///
/// The ordering reflects the canonical state-machine progression; both the
/// kernel and DPDK backends map their internal connection state to one of
/// these values via `state()`. Userspace code must treat `Closed` and
/// `Established` as the two stable states; everything else is transitional.
enum class TcpState : uint8_t {
    Closed = 0,    ///< No connection (default state before connect or after teardown).
    Listen,        ///< Passive open: waiting for a SYN.
    SynSent,       ///< Active open: SYN sent, waiting for SYN-ACK.
    SynReceived,   ///< SYN-ACK sent, waiting for ACK.
    Established,   ///< Connection open, bidirectional data flowing.
    FinWait1,      ///< Sent FIN, waiting for FIN or ACK.
    FinWait2,      ///< FIN acked, waiting for peer's FIN.
    CloseWait,     ///< Received peer's FIN, waiting for local close.
    Closing,       ///< Simultaneous close in progress.
    LastAck,       ///< Waiting for ACK of our FIN after CloseWait.
    TimeWait,      ///< Waiting 2*MSL after close to absorb delayed packets.
};

// ---------------------------------------------------------------------------
// tcp_state_name
// ---------------------------------------------------------------------------

/// @brief Return a stable, human-readable name for a `TcpState`.
///
/// Used for logging and test assertions. Returns a static string literal;
/// callers never need to free the pointer. Unknown values (future ABI drift
/// or `reinterpret_cast` abuse) return `"UNKNOWN"`.
[[nodiscard]] constexpr const char* tcp_state_name(TcpState s) noexcept {
    switch (s) {
        case TcpState::Closed:       return "CLOSED";
        case TcpState::Listen:       return "LISTEN";
        case TcpState::SynSent:      return "SYN_SENT";
        case TcpState::SynReceived:  return "SYN_RECEIVED";
        case TcpState::Established:  return "ESTABLISHED";
        case TcpState::FinWait1:     return "FIN_WAIT_1";
        case TcpState::FinWait2:     return "FIN_WAIT_2";
        case TcpState::CloseWait:    return "CLOSE_WAIT";
        case TcpState::Closing:      return "CLOSING";
        case TcpState::LastAck:      return "LAST_ACK";
        case TcpState::TimeWait:     return "TIME_WAIT";
    }
    return "UNKNOWN";
}

} // namespace eph::net
