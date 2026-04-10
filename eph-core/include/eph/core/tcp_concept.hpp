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

// v3.3 Phase 4 prerequisite: forward TcpState + tcp_state_name to the
// canonical definition in `eph/core/tcp_state.hpp`. The legacy enum used
// to live inline in this header, but Phase 2 introduced
// `eph/net/tcp_state.hpp` (which now also forwards to the same canonical
// header). The dual definition was an ODR conflict in any TU that
// included both — Phase 4's eph-net-dpdk module is the first to do so,
// and the breakage surfaced there. Pulling the enum into one shared
// header in eph-core resolves the conflict without changing anyone's
// public API.
//
// Phase 7 deletes this header outright.
#include "eph/core/tcp_state.hpp"

namespace eph::net {

// `TcpState` and `tcp_state_name` are now provided by
// `eph/core/tcp_state.hpp`. We deliberately do NOT redefine them here
// to avoid the ODR violation. The names are still in `eph::net::` so
// all legacy callers resolve unchanged.

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
