#pragma once

/// @file config.hpp
/// Configuration structs for KernelTcpStream / KernelUdpSocket / KernelPoller.
///
/// Part of Phase 3 of the v3.3 refactor (see
/// .artifacts/design-eph-v3.3-architecture-20260410.md). These are plain data
/// aggregates: every field has a sensible default so user code can
/// designated-initialize only what it cares about.

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "eph/net/reconnect_policy.hpp"
#include "eph/net/socket_addr.hpp"
#include "eph/net/detail/tls_constants.hpp"  // Phase 5: TlsConfig

namespace eph::net::kernel {

// ---------------------------------------------------------------------------
// StreamConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `KernelTcpStream::create`.
///
/// Minimum usable config is `remote` + default everything else. The Phase 3
/// scope is plaintext TCP + optional TLS stub + optional WS upgrade; real
/// TLS handshake and WS HTTP upgrade are Phase 5 work.
struct StreamConfig {
    /// @brief Destination endpoint. Must be populated by the caller.
    SocketAddr remote{};

    /// @brief Connect deadline (DNS + TCP + TLS handshake combined).
    std::chrono::milliseconds connect_timeout{3000};

    /// @brief Size of the per-stream reassembly buffer. Must be > 0.
    std::size_t reasm_capacity{64 * 1024};

    /// @brief TCP_NODELAY setting at socket creation time.
    bool tcp_nodelay{true};

    /// @brief Reconnection policy applied by higher-level recovery code.
    ReconnectPolicyConfig reconnect{};

    /// @brief TLS 1.3 configuration. Only consulted when the stream is
    ///        instantiated with `EnableTls=true`. The default value works
    ///        for plaintext use (since the field is ignored). For TLS use,
    ///        callers MUST set at least `tls.hostname` (SNI) and either
    ///        `tls.ca_cert_path` or rely on the system's default trust
    ///        store. Phase 5 wires this through the real handshake.
    ::eph::net::TlsConfig tls{};
};

// ---------------------------------------------------------------------------
// UdpConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `KernelUdpSocket::create`.
struct UdpConfig {
    /// @brief Local bind address. Use `Ipv4Addr{0,0,0,0}` to bind to all NICs.
    SocketAddr bind{};

    /// @brief If set to a non-zero port, wire in an initial `connect(2)` to
    ///        this endpoint so that the kernel filters inbound datagrams to
    ///        that source. Leave default to stay in unconnected mode.
    SocketAddr connect_to{};

    /// @brief Receive buffer size — 0 means "use the kernel default".
    std::size_t rcv_buf{0};

    /// @brief Send buffer size — 0 means "use the kernel default".
    std::size_t snd_buf{0};

    /// @brief Enable SO_REUSEADDR before bind() (for multicast groups this is
    ///        usually required).
    bool reuse_addr{false};
};

// ---------------------------------------------------------------------------
// PollerConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `KernelPoller::create`.
struct PollerConfig {
    /// @brief Initial entries vector capacity reservation.
    std::size_t initial_capacity{16};

    /// @brief Max events drained per epoll_wait call. Bounds worst-case
    ///        latency of a single `poll()` call in the face of thundering
    ///        herds.
    int max_events_per_wait{64};
};

} // namespace eph::net::kernel
