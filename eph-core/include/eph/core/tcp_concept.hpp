#pragma once

/// @file tcp_concept.hpp
/// TcpTransport concept — defines the interface that any TCP backend must
/// satisfy (DPDK, io_uring, kernel sockets, loopback for testing, etc.).
///
/// All methods are constrained via C++20 concepts so that template
/// instantiation produces zero-overhead monomorphized code.

#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <string>

namespace eph::net {

/// @brief TCP connection states (client-side only).
///
/// This library does not implement server accept. States follow the
/// RFC 793 active-open path for client-initiated connections.
enum class TcpState : uint8_t {
    Closed,
    SynSent,
    Established,
    FinWait1,
    FinWait2,
    Closing,   ///< RFC 793: simultaneous close — both sides sent FIN before receiving peer's FIN
    TimeWait,
    CloseWait,
    LastAck,
};

/// @brief Return the RFC 793 name for a TCP state as a C string.
/// @param s  The TcpState value to convert.
/// @return A null-terminated string (e.g., "ESTABLISHED", "SYN_SENT").
constexpr const char* tcp_state_name(TcpState s) noexcept {
    switch (s) {
        case TcpState::Closed:      return "CLOSED";
        case TcpState::SynSent:     return "SYN_SENT";
        case TcpState::Established: return "ESTABLISHED";
        case TcpState::FinWait1:    return "FIN_WAIT_1";
        case TcpState::FinWait2:    return "FIN_WAIT_2";
        case TcpState::Closing:     return "CLOSING";
        case TcpState::TimeWait:    return "TIME_WAIT";
        case TcpState::CloseWait:   return "CLOSE_WAIT";
        case TcpState::LastAck:     return "LAST_ACK";
    }
    return "UNKNOWN";
}

/// @brief Concept for a TCP transport backend.
///
/// Any type satisfying this concept can be used with TlsSession and Transport.
/// The key design constraint: all methods must be defined in headers so that
/// template instantiation can fully inline them (zero runtime overhead).
///
/// Required methods:
///   - connect(timeout)       -- establish the TCP connection
///   - send(data, len)        -- send raw bytes
///   - poll_rx(callback)      -- poll for incoming data, invoke callback per payload
///   - last_rx_burst_tsc()    -- TSC captured right after the lowest-level receive
///                               (rte_eth_rx_burst / recvmsg) returns data
///   - close()                -- graceful close (FIN)
///   - reset()                -- forced close (RST)
///   - mss()                  -- maximum segment size
///   - state()                -- current TCP state
///   - is_established()       -- convenience check
///
/// @tparam T  The TCP transport backend type to check.
template <typename T>
concept TcpTransport = requires(T& t,
    const void* data, size_t len,
    std::chrono::milliseconds timeout) {
    // Connection lifecycle
    { t.connect(timeout) } -> std::same_as<std::expected<void, std::string>>;
    { t.close() }          -> std::same_as<std::expected<void, std::string>>;
    { t.reset() }          noexcept;

    // Data transfer
    { t.send(data, len) } -> std::same_as<std::expected<size_t, std::string>>;

    // Timestamping — TSC captured at the earliest point after data arrives
    { t.last_rx_burst_tsc() } -> std::convertible_to<uint64_t>;

    // State queries
    /// mss() returns 0 before connection is established, and a positive
    /// value (typically 1460 for standard Ethernet) after connect() succeeds.
    { t.mss() }            -> std::convertible_to<uint16_t>;
    { t.state() }          -> std::same_as<TcpState>;
    { t.is_established() } -> std::same_as<bool>;
} && requires(T& t) {
    // poll_rx must accept a callback with signature void(const uint8_t*, uint16_t)
    /// @note The uint16_t length parameter supports payloads up to 65535 bytes.
    ///       Standard Ethernet MTU (1500) and typical jumbo frames (9000) fit comfortably.
    ///       For unusual configurations with >64KB payload reassembly, this would need
    ///       to be widened to uint32_t (breaking API change).
    { t.poll_rx([](const uint8_t*, uint16_t) {}) }
        -> std::same_as<std::expected<uint16_t, std::string>>;
};

} // namespace eph::net

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for TcpState
// ─────────────────────────────────────────────────────────────────────────────

/// @brief std::formatter specialization for TcpState.
///
/// Formats TcpState values using their RFC 793 names (e.g., "ESTABLISHED").
template <>
struct std::formatter<eph::net::TcpState> : std::formatter<const char*> {
    /// @brief Format the TcpState value as its RFC 793 name.
    /// @param s    The TcpState value to format.
    /// @param ctx  The format context to write into.
    /// @return Iterator past the end of the formatted output.
    auto format(eph::net::TcpState s, auto& ctx) const {
        return std::formatter<const char*>::format(
            eph::net::tcp_state_name(s), ctx);
    }
};
