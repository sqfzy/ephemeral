#pragma once

/// @file config.hpp
/// Configuration aggregates for `DpdkTcpStream` / `DpdkUdpSocket` /
/// `DpdkPoller`.
///
/// These types are thin adapters over the existing `eph::dpdk::TcpConfig` /
/// `eph::dpdk::UdpConfig` structures. We re-export them under the
/// `eph::net::dpdk` namespace so user code can write
/// `eph::net::dpdk::StreamConfig` without pulling in the underlying header
/// name at the call site.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "eph/dpdk/tcp.hpp"       // TcpConfig
#include "eph/dpdk/udp.hpp"       // UdpConfig (legacy naming collides with our
                                  // eph::net::dpdk::UdpConfig below — we keep
                                  // the legacy type in eph::dpdk::UdpConfig,
                                  // and our config wrapper names it
                                  // LegacyUdpConfig at its single use-site).
#include <optional>

#include "eph/net/http.hpp"                   // HttpHeader
#include "eph/net/proxy.hpp"                  // ProxyConfig
#include "eph/net/detail/tls_constants.hpp"   // TlsConfig

// The StreamConfig carries a real `eph::net::TlsConfig` field. aws-lc is
// the only OpenSSL flavour in eph-net-dpdk TUs; `RAND_bytes` call sites use
// `getrandom(2)` to avoid symbol conflicts.

namespace eph::net::dpdk {

// ---------------------------------------------------------------------------
// StreamConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `DpdkTcpStream::create`.
///
/// The heavy DPDK wiring (MAC, mempool, port/queue IDs, MSS, recv_window)
/// lives inside `eph::dpdk::TcpConfig` and is preserved verbatim via the
/// `legacy` field. We add higher-level knobs on top (connect timeout,
/// reconnect policy) without duplicating any of the validated low-level
/// fields.
///
/// Design rationale for embedding rather than flattening: it keeps a single
/// source of truth for the validated DPDK fields in `eph::dpdk::TcpConfig`.
struct StreamConfig {
    /// @brief Underlying DPDK TcpSession configuration (mempool, port/queue,
    ///        4-tuple, MAC addresses, MSS). Must be populated by the caller;
    ///        validation runs in `DpdkTcpStream::create()` via
    ///        `legacy.validate()`.
    ::eph::dpdk::TcpConfig legacy{};

    /// @brief The DPDK mempool used for TcpSession mbuf allocations. Kept as a
    ///        top-level field so callers can use designated initialization
    ///        even though the legacy TcpConfig does not itself hold a
    ///        mempool pointer (historically the mempool was passed separately
    ///        to the `TcpSession(config, pool)` ctor).
    ::rte_mempool* pool{nullptr};

    /// @brief TCP handshake deadline (SYN -> SYN/ACK -> ACK round trip).
    std::chrono::milliseconds connect_timeout{3000};

    /// @brief TLS 1.3 handshake configuration. Ignored when the template
    /// parameter `EnableTls=false`.
    ::eph::net::TlsConfig tls{};

    // ── WebSocket upgrade ─────────────────────────────────────────────────
    //
    // Same contract as `eph::net::kernel::StreamConfig`: non-empty `ws_path`
    // enables a WS HTTP Upgrade handshake after TCP (and optional TLS)
    // completes. Empty = plain TCP/TLS byte stream.

    /// @brief WebSocket request-target (e.g. "/ws/btcusdt@bookTicker").
    ///        Empty = no upgrade.
    std::string ws_path{};

    /// @brief Value for the `Host:` header. If empty, falls back to
    ///        `tls.hostname` when TLS is enabled, otherwise a synthesized
    ///        `IP:port` string from the legacy TCP 4-tuple.
    std::string ws_host{};

    /// @brief Extra headers appended after the five mandatory upgrade
    ///        headers. Non-owning views — keep backing storage alive.
    std::vector<::eph::net::HttpHeader> ws_extra_headers{};

    /// @brief WS handshake deadline.
    std::chrono::milliseconds ws_timeout{std::chrono::seconds{10}};

    /// @brief Offer RFC 7692 permessage-deflate during the WS upgrade.
    ///        Symmetric with `eph::net::kernel::StreamConfig::ws_permessage_deflate`.
    ///        Default true; the handshake injects
    ///        `Sec-WebSocket-Extensions: permessage-deflate` and
    ///        wires the codec for inbound inflate when the server
    ///        accepts. Set to false to opt out.
    bool ws_permessage_deflate{true};

    // ── HTTP CONNECT proxy ────────────────────────────────────────────────
    //
    // The DPDK backend does NOT support HTTP CONNECT proxies — HFT colo
    // deployments never use proxies, and a DPDK client by definition bypasses
    // the kernel's userland TCP stack that a proxy would be reachable
    // through. Nevertheless this field exists on the DPDK `StreamConfig` so
    // user code can write one config-construction helper that targets both
    // backends.
    //
    // `DpdkTcpStream::create` rejects any non-empty `proxy` with
    // `Error::InvalidConfig` at factory time.

    /// @brief Unsupported on DPDK — always rejected if set.
    std::optional<::eph::net::ProxyConfig> proxy{};

    /// @brief Reassembly buffer capacity in bytes. Raised from the legacy
    ///        64 KiB default so typical L2 orderbook snapshots (often
    ///        100-200 KiB in a single burst) fit without triggering the
    ///        overflow error path. If the codec is unable to drain faster
    ///        than the producer fills, `append()` returns false and the
    ///        stream is forcibly reset so the reconnect policy takes over
    ///        — silently dropping application bytes is not an option in
    ///        HFT. Kept symmetric with `eph::net::kernel::StreamConfig`
    ///        which carries the same field.
    ///
    /// Sizing guidance:
    ///   - Rule of thumb: set to **2-4x** the largest single frame your
    ///     codec is expected to emit in one burst, rounded up to a
    ///     convenient power of two. The factor covers transient drain
    ///     lag where the codec hasn't consumed the previous frame yet.
    ///   - Workload benchmarks:
    ///       * WS market-data (JSON bookTicker) ................ 64-128 KiB
    ///       * WS L2 orderbook snapshot (BTC/USDT) ............ 256-384 KiB
    ///       * FIX Drop Copy / large ITCH message bundle ...... 512 KiB
    ///   - Overflow is visible: monitor `StreamMetric::kReasmOverflows`.
    ///     A non-zero counter means frames were dropped and the stream
    ///     was reset; if it persists, raise `reasm_capacity` until the
    ///     counter stays at zero under peak load.
    ///   - Memory footprint: the buffer is allocated once per Stream and
    ///     is not shared — N streams → N × capacity resident bytes.
    std::size_t reasm_capacity{256 * 1024};

    /// @brief When using `DpdkTcpStream::create_and_attach(cfg, platform)`,
    ///        force the connection to land on a specific RX queue.
    ///
    /// nullopt (default):
    ///   * RssPartitioned mode → let the NIC's hash decide; the stream is
    ///     attached to whichever Poller already owns that queue.
    ///   * FlowDirector mode  → the helper round-robins across queues to
    ///     spread connections evenly.
    ///   * Software mode      → attach to queue 0 (the only queue).
    ///
    /// uint16_t value:
    ///   * RssPartitioned mode → search the ephemeral src-port range for a
    ///     port whose Toeplitz hash lands on `pin_to_queue`. If the range
    ///     is exhausted, create_and_attach returns InvalidConfig.
    ///   * FlowDirector mode  → install an rte_flow rule steering the 5-tuple
    ///     to `pin_to_queue`.
    ///   * Software mode      → must be 0 (or nullopt); otherwise QueueOutOfRange.
    std::optional<uint16_t> pin_to_queue{};

    /// @brief Per-lcore mempool hint for `create_and_attach`.
    ///
    /// When set, the helper resolves `cfg.pool` via
    /// `platform.pool_for_lcore(*pool_lcore_hint)` before constructing
    /// the stream. This lets callers stay within their lcore's
    /// NUMA-local pool when `PlatformConfig::per_lcore_pools > 0`,
    /// avoiding the +50–100 ns cross-NUMA mbuf alloc penalty.
    ///
    /// Default = -1 (not set):
    ///   * The helper does NOT touch `cfg.pool`. If the caller already
    ///     populated `cfg.pool` (e.g. with `platform.mempool()` or a
    ///     pool of their own), it is used verbatim. This is the
    ///     backwards-compat path — no existing call sites break.
    ///
    /// `>= 0` (set):
    ///   * The helper overrides `cfg.pool` with
    ///     `platform.pool_for_lcore(uint16_t(*pool_lcore_hint))`. If
    ///     the lookup returns nullptr (lcore id out of range when
    ///     `per_lcore_pools > 0`), `create_and_attach` returns
    ///     `Error::InvalidConfig`.
    ///
    /// In default-mode Platforms (`per_lcore_pools == 0`),
    /// `pool_for_lcore` always returns the shared pool, so setting this
    /// hint is a harmless no-op.
    ///
    /// Encoded as `int` (not `uint16_t`) so the canonical "not set"
    /// sentinel `-1` is representable; values `>= 0` are clamped to a
    /// `uint16_t` lcore id at the call site.
    int pool_lcore_hint{-1};
};

// ---------------------------------------------------------------------------
// UdpConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `DpdkUdpSocket::create`.
///
/// Parallels `StreamConfig`: wraps the legacy `eph::dpdk::UdpConfig`
/// (fixed-peer UDP packet template) and adds high-level knobs. The legacy
/// config carries src/dst MAC, IP, port, mempool, port_id, tx_queue_id, and
/// hw_cksum — exactly what `UdpPacketTemplate::init` needs.
struct UdpConfig {
    /// @brief Underlying DPDK UdpSender configuration.
    ::eph::dpdk::UdpConfig legacy{};

    /// @brief See StreamConfig::pin_to_queue. UDP has no connect handshake,
    /// so the pin is honoured at attach time — the user is expected to have
    /// already set `legacy.src_port` such that the (src/dst) tuple hashes
    /// to the desired queue under RSS, or the helper installs an rte_flow
    /// rule under FlowDirector. nullopt = auto.
    std::optional<uint16_t> pin_to_queue{};

    /// @brief Per-lcore mempool hint for `create_and_attach`. See
    /// `StreamConfig::pool_lcore_hint` for full semantics.
    ///
    /// Default `-1` (not set) preserves existing behavior: the helper
    /// uses `legacy.pool` as the caller populated it. When set
    /// `>= 0`, the helper overrides `legacy.pool` with
    /// `platform.pool_for_lcore(uint16_t(*pool_lcore_hint))` before
    /// sender construction.
    int pool_lcore_hint{-1};
};

// ---------------------------------------------------------------------------
// PollerConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `DpdkPoller::create`.
///
/// DPDK lcore burst poll has no epoll equivalent: the Poller simply calls
/// `rte_eth_rx_burst` on a single {port_id, rx_queue_id} and routes each
/// mbuf to a registered Pollable based on its 5-tuple.
///
/// Thread affinity is intentionally NOT a field here — `DpdkPoller` does
/// not spawn a thread of its own. The user calls `poll()` from their own
/// lcore loop and is responsible for pinning that thread themselves.
struct PollerConfig {
    /// @brief DPDK port ID on which to poll.
    uint16_t port_id{0};

    /// @brief DPDK RX queue index on the above port.
    uint16_t rx_queue_id{0};
};

} // namespace eph::net::dpdk
