#pragma once

/// @file config.hpp
/// Configuration structs for KernelTcpStream / KernelUdpSocket / KernelPoller.
///
/// These are plain data aggregates: every field has a sensible default so
/// user code can designated-initialize only what it cares about.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <optional>

#include "eph/net/http.hpp"  // HttpHeader (for ws_extra_headers)
#include "eph/net/proxy.hpp" // ProxyConfig (HTTP CONNECT)
#include "eph/net/socket_addr.hpp"
#include "eph/net/detail/tls_constants.hpp"  // TlsConfig

namespace eph::net::kernel {

// ---------------------------------------------------------------------------
// StreamConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `KernelTcpStream::create`.
///
/// Minimum usable config is `remote` + default everything else.
struct StreamConfig {
    /// @brief Destination endpoint. Must be populated by the caller.
    SocketAddr remote{};

    /// @brief Optional local bind endpoint applied before connect().
    ///        Default-constructed (`{0.0.0.0}:0`) means "let the kernel
    ///        pick" — same behavior as if no bind() were called. Set a
    ///        non-zero `port` (and optionally a non-zero `ip`) to pin the
    ///        5-tuple's source side, which is required when a downstream
    ///        path-discovery probe (e.g. paris-traceroute) must traverse
    ///        the same ECMP bucket as this connection.
    SocketAddr local{};

    /// @brief Connect deadline (DNS + TCP + TLS handshake combined).
    std::chrono::milliseconds connect_timeout{3000};

    /// @brief Size of the per-stream reassembly buffer. Must be > 0.
    std::size_t reasm_capacity{64 * 1024};

    /// @brief TCP_NODELAY setting at socket creation time.
    bool tcp_nodelay{true};

    /// @brief TLS 1.3 configuration. Only consulted when the stream is
    ///        instantiated with `EnableTls=true`. The default value works
    ///        for plaintext use (since the field is ignored). For TLS use,
    ///        callers MUST set at least `tls.hostname` (SNI) and either
    ///        `tls.ca_cert_path` or rely on the system's default trust
    ///        store.
    ::eph::net::TlsConfig tls{};

    // ── WebSocket upgrade ─────────────────────────────────────────────────
    //
    // When `ws_path` is non-empty, `KernelTcpStream::create` will run a
    // WebSocket HTTP Upgrade handshake after the TCP (and optional TLS)
    // handshake completes. An empty `ws_path` disables the upgrade — the
    // stream behaves exactly like plain TCP / TLS.

    /// @brief WebSocket request-target (e.g. "/ws/btcusdt@bookTicker").
    ///        Empty = no upgrade.
    std::string ws_path{};

    /// @brief Value to emit as the `Host:` header. If empty, the handshake
    ///        falls back to `tls.hostname` (for TLS) or the numeric
    ///        `remote.to_string()` (for plaintext).
    std::string ws_host{};

    /// @brief Optional caller-supplied headers appended AFTER the mandatory
    ///        WebSocket upgrade headers (Host / Upgrade / Connection /
    ///        Sec-WebSocket-Key / Sec-WebSocket-Version). Values are
    ///        non-owning views; callers must keep backing storage alive
    ///        until `create()` returns.
    std::vector<::eph::net::HttpHeader> ws_extra_headers{};

    /// @brief Cumulative deadline for the WS HTTP handshake phase.
    std::chrono::milliseconds ws_timeout{std::chrono::seconds{10}};

    /// @brief Offer RFC 7692 permessage-deflate during the WS upgrade.
    ///        When true and `ws_path` is non-empty, the handshake
    ///        injects `Sec-WebSocket-Extensions: permessage-deflate`
    ///        into the request and, if the server accepts,
    ///        configures the codec to inflate inbound RSV1 frames.
    ///        Default true so users targeting a venue that may opt
    ///        into compression (Binance / Bybit / OKX) get correct
    ///        behaviour out of the box. Set to false to suppress
    ///        the offer (e.g. for venues that mis-implement the
    ///        extension and respond with garbled compressed frames).
    bool ws_permessage_deflate{true};

    // ── HTTP CONNECT proxy ────────────────────────────────────────────────
    //
    // When `proxy` is set, `KernelTcpStream::create` will:
    //   1. TCP-connect to `proxy->host:proxy->port` (instead of `remote`)
    //   2. Drive an HTTP CONNECT handshake through the proxy targeting
    //      `remote.to_string()` (or the explicit Host from the upstream
    //      config)
    //   3. If TLS is enabled, run the TLS handshake inside the tunnel
    //   4. If ws_path is set, run the WS upgrade inside the TLS tunnel
    //
    // See `eph/net/proxy.hpp` for the validation semantics and
    // `eph/net/detail/http_connect.hpp` for the wire format.

    /// @brief Optional HTTP CONNECT proxy. Empty = direct connect.
    std::optional<::eph::net::ProxyConfig> proxy{};
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
