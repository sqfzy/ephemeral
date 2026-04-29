#pragma once

/// @file config.hpp
/// Configuration structs for KernelTcpStream / KernelUdpSocket / KernelPoller.
///
/// These are plain data aggregates: every field has a sensible default so
/// user code can designated-initialize only what it cares about.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "eph/net/keepalive_config.hpp"  // KeepaliveConfig
#include "eph/net/proxy.hpp"             // ProxyConfig (HTTP CONNECT)
#include "eph/net/socket_addr.hpp"
#include "eph/net/detail/tls_constants.hpp"  // TlsConfig
#include "eph/net/ws_config.hpp"             // WsConfig

namespace eph::net::kernel {

// ---------------------------------------------------------------------------
// StreamConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `KernelTcpStream::create`.
///
/// Minimum usable config is `remote` + default everything else.
///
/// Layout convention (post-T3.19): backend-shared concerns live at the top
/// level — `remote / connect_timeout / reasm_capacity / tls / ws /
/// keepalive / proxy`. Kernel-only knobs live inside `kernel` substruct.
/// The DPDK twin (`eph::net::dpdk::StreamConfig`) mirrors the same shape
/// with a `dpdk` substruct in place of `kernel`.
struct StreamConfig {
    /// @brief Destination endpoint. Must be populated by the caller.
    SocketAddr remote{};

    /// @brief Connect deadline (DNS + TCP + TLS handshake combined).
    std::chrono::milliseconds connect_timeout{3000};

    /// @brief Size of the per-stream reassembly buffer. Must be > 0.
    std::size_t reasm_capacity{64 * 1024};

    /// @brief TLS 1.3 configuration. Only consulted when the stream is
    ///        instantiated with `EnableTls=true`. The default value works
    ///        for plaintext use (since the field is ignored). For TLS use,
    ///        callers MUST set at least `tls.hostname` (SNI) and either
    ///        `tls.ca_cert_path` or rely on the system's default trust
    ///        store.
    ::eph::net::TlsConfig tls{};

    /// @brief Optional WebSocket Upgrade. Default-constructed `WsConfig`
    ///        (empty `path`) disables the upgrade — the stream behaves
    ///        as plain TCP / TLS. Non-empty `path` triggers the RFC 6455
    ///        handshake inside `KernelTcpStream::create` after TLS comes up.
    ::eph::net::WsConfig ws{};

    /// @brief Optional TCP keepalive. Default-constructed (interval == 0)
    ///        disables it. When `interval > 0`, `KernelTcpStream::create`
    ///        wires `setsockopt(SO_KEEPALIVE / TCP_KEEPIDLE / TCP_KEEPINTVL
    ///        / TCP_KEEPCNT)` after the connect succeeds. Useful for
    ///        long-lived venue WS sessions where the venue silently
    ///        half-closes idle peers.
    ::eph::net::KeepaliveConfig keepalive{};

    /// @brief Optional HTTP CONNECT proxy. Empty = direct connect.
    ///        When set, `KernelTcpStream::create` will:
    ///          1. TCP-connect to `proxy->host:proxy->port`
    ///          2. Drive an HTTP CONNECT handshake targeting `remote`
    ///          3. (TLS) handshake inside the tunnel if `EnableTls`
    ///          4. (WS)  upgrade  inside the tunnel if `ws.path` non-empty
    ///
    ///        DPDK does not support proxies — the field is absent from
    ///        the DPDK `StreamConfig` so misuse is a compile error.
    std::optional<::eph::net::ProxyConfig> proxy{};

    // ── Kernel-only knobs ────────────────────────────────────────────────

    /// @brief Knobs exposed only by the kernel (epoll / POSIX socket)
    ///        backend. The DPDK backend has its own substruct (`dpdk`) for
    ///        knobs only meaningful to a userspace PMD.
    struct Kernel {
        /// @brief Optional local bind endpoint applied before connect().
        ///        Default-constructed (`{0.0.0.0}:0`) means "let the kernel
        ///        pick" — same behavior as if no bind() were called. Set a
        ///        non-zero `port` (and optionally a non-zero `ip`) to pin
        ///        the 5-tuple's source side, which is required when a
        ///        downstream path-discovery probe (e.g. paris-traceroute)
        ///        must traverse the same ECMP bucket as this connection.
        SocketAddr local_bind{};

        /// @brief TCP_NODELAY setting at socket creation time.
        bool       tcp_nodelay{true};
    } kernel;
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
