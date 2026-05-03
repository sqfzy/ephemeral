#pragma once

/// @file config.hpp
/// Configuration aggregates for `DpdkTcpStream` / `DpdkUdpSocket` /
/// `DpdkPoller`.
///
/// Layout convention (post-T3.19): backend-shared concerns live at the top
/// level — `connect_timeout / reasm_capacity / tls / ws / keepalive` — and
/// DPDK-only knobs live inside a `dpdk` substruct. The kernel twin
/// (`eph::net::kernel::StreamConfig`) mirrors the same shape with a
/// `kernel` substruct.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "eph/dpdk/tcp.hpp"       // TcpConfig (the wire-level type)
#include "eph/dpdk/udp.hpp"       // wire::UdpConfig (wire-level UDP TX cfg;
                                  // moved under `eph::dpdk::wire::` post
                                  // Tier-1 rename to avoid the prior name
                                  // collision with the outer
                                  // `eph::net::dpdk::UdpConfig` below).
#include "eph/net/keepalive_config.hpp"      // KeepaliveConfig
#include "eph/net/detail/tls_constants.hpp"  // TlsConfig
#include "eph/net/ws_config.hpp"             // WsConfig

// The StreamConfig carries a real `eph::net::TlsConfig` field. aws-lc is
// the only OpenSSL flavour in eph-net-dpdk TUs; `RAND_bytes` call sites use
// `getrandom(2)` to avoid symbol conflicts.

namespace eph::net::dpdk {

// ---------------------------------------------------------------------------
// StreamConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `DpdkTcpStream::create_and_attach`.
///
/// The DPDK wire-level fields (4-tuple, MACs, MSS, recv_window, port/queue
/// IDs) live inside `cfg.dpdk.wire`; the surrounding `dpdk`
/// substruct also carries the mempool pointer and the pin / per-lcore
/// hints that only make sense in a userspace PMD. Top-level fields are
/// the backend-shared knobs.
struct StreamConfig {
    /// @brief TCP handshake deadline (SYN -> SYN/ACK -> ACK round trip).
    std::chrono::milliseconds connect_timeout{3000};

    /// @brief Reassembly buffer capacity in bytes. Sized larger than the
    ///        kernel default because typical L2 orderbook snapshots
    ///        (often 100–200 KiB in a single burst) must fit without
    ///        triggering the overflow error path. Mirrors the kernel
    ///        StreamConfig field with a different default — 256 KiB
    ///        here vs 64 KiB there.
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
    ///   - Memory footprint: per-Stream — N streams → N × capacity bytes.
    ///
    /// Boundary contract (DPDK-specific — diverges from the kernel
    /// backend):
    ///   * `0` → silently treated as "use default" (256 KiB) rather
    ///     than rejected. The kernel `KernelTcpStream::create` rejects
    ///     `0` outright with `Error::InvalidConfig` ("must be > 0"),
    ///     so callers porting between backends should set an explicit
    ///     value rather than relying on the default-on-zero behaviour.
    ///   * `(0, 4096)` → rejected at `DpdkTcpStream::create` with
    ///     `Error::InvalidConfig` (would otherwise overflow on the
    ///     first MSS-sized burst). Use `0` to opt into the 256 KiB
    ///     default, or pass `>= 4096` explicitly.
    ///   * `>= 4096` → used verbatim.
    std::size_t reasm_capacity{256 * 1024};

    /// @brief TLS 1.3 handshake configuration. Ignored when the template
    ///        parameter `EnableTls=false`.
    ::eph::net::TlsConfig tls{};

    /// @brief Optional WebSocket Upgrade. Default-constructed `WsConfig`
    ///        (empty `path`) disables it — the stream behaves as plain
    ///        TCP / TLS. Non-empty `path` triggers the RFC 6455
    ///        handshake inside `DpdkTcpStream::create_and_attach`.
    ::eph::net::WsConfig ws{};

    /// @brief Optional TCP keepalive. Default-constructed (interval == 0)
    ///        disables it. When `interval > 0`, the value is lowered into
    ///        `dpdk.wire.keepalive_interval / keepalive_probes`
    ///        at factory time so the existing DPDK PMD machinery
    ///        (`TcpSession::tick_keepalive`) honours it.
    ///
    ///        Previously this knob was reachable only via
    ///        `cfg.dpdk.wire.keepalive_interval`; it has been
    ///        promoted to the top level (T3.19 reshape) so the surface
    ///        is symmetric with the kernel backend.
    ::eph::net::KeepaliveConfig keepalive{};

    // ── DPDK-only knobs ──────────────────────────────────────────────────

    /// @brief Knobs only meaningful to the userspace PMD backend. The
    ///        kernel twin uses a `kernel` substruct of its own knobs.
    struct Dpdk {
        /// @brief Underlying wire-level DPDK TcpSession configuration:
        ///        4-tuple, MAC addresses, MSS, recv_window, port/queue
        ///        IDs, max_rx_burst. Validated via `wire.validate()`
        ///        at factory time. (Renamed from `legacy` in T3.19 — the
        ///        old name was non-semantic; this is the wire-level
        ///        TcpConfig the PMD ingests.)
        ::eph::dpdk::TcpConfig wire{};

        /// @brief The DPDK mempool used for TcpSession mbuf allocations.
        ///        Top-level so callers can use designated initialization
        ///        even though the wire-level TcpConfig does not itself
        ///        hold a mempool pointer.
        ::rte_mempool* pool{nullptr};

        /// @brief When using `DpdkTcpStream::create_and_attach`, force the
        ///        connection to land on a specific RX queue.
        ///
        /// nullopt (default):
        ///   * RssPartitioned mode → let the NIC's hash decide.
        ///   * FlowDirector mode  → round-robin across queues.
        ///   * Software mode      → attach to queue 0 (the only queue).
        ///
        /// uint16_t value:
        ///   * RssPartitioned mode → search the ephemeral src-port range
        ///     for a port whose Toeplitz hash lands on `pin_to_queue`.
        ///   * FlowDirector mode  → install an rte_flow rule steering
        ///     the 5-tuple to `pin_to_queue`.
        ///   * Software mode      → must be 0 (or nullopt).
        std::optional<uint16_t> pin_to_queue{};

        /// @brief Per-lcore mempool hint for `create_and_attach`.
        ///
        /// When set (>= 0), the helper resolves `dpdk.pool` via
        /// `platform.pool_for_lcore(*pool_lcore_hint)` so callers stay
        /// within their lcore's NUMA-local pool when
        /// `PlatformConfig::per_lcore_pools > 0`. Default `-1` keeps
        /// `dpdk.pool` as the caller populated it.
        int pool_lcore_hint{-1};
    } dpdk;
};

// ---------------------------------------------------------------------------
// UdpConfig
// ---------------------------------------------------------------------------

/// @brief Configuration for `DpdkUdpSocket::create`.
///
/// Mirrors `StreamConfig`'s post-T3.19 shape: the DPDK-only knobs
/// (wire-level UdpConfig + pin / per-lcore hints) live inside a `Dpdk`
/// substruct. Top-level fields are reserved for future backend-shared
/// concerns (UDP has none today).
struct UdpConfig {
    // ── DPDK-only knobs ──────────────────────────────────────────────────

    /// @brief Knobs only meaningful to the userspace PMD backend. Shape
    ///        mirrors `StreamConfig::Dpdk` so TCP and UDP have the same
    ///        post-T3.19 layout.
    struct Dpdk {
        /// @brief Underlying wire-level DPDK UdpSender configuration:
        ///        4-tuple, MACs, port/queue IDs, mempool, hw_cksum flag.
        ///        Validated via `wire.validate()` at factory
        ///        time. (Renamed from `legacy` in the Tier-1 audit
        ///        follow-up — same rationale as TCP's `wire`:
        ///        the old name was non-semantic; this is the wire-level
        ///        UdpConfig the PMD ingests.)
        ::eph::dpdk::wire::UdpConfig wire{};

        /// @brief See `StreamConfig::Dpdk::pin_to_queue`. UDP has no connect
        /// handshake, so the pin is honoured at attach time — the user is
        /// expected to have already set `wire.src_port` such
        /// that the (src/dst) tuple hashes to the desired queue under
        /// RSS, or the helper installs an rte_flow rule under
        /// FlowDirector. nullopt = auto.
        std::optional<uint16_t> pin_to_queue{};

        /// @brief Per-lcore mempool hint for `create_and_attach`.
        ///        Default `-1` keeps `wire.pool` as the caller
        ///        populated it.
        int pool_lcore_hint{-1};
    } dpdk;
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

    /// @brief Maximum simultaneously-registered Pollables.
    ///
    /// Default 16 preserves the historical capacity for backwards compat;
    /// raise up to `DpdkPoller<void>::kMaxConnHard` (currently 64) when a
    /// deployment fans out more streams onto a single lcore. The storage
    /// (`entries_` + open-addressed route table) is sized at compile time
    /// to `kMaxConnHard`, so raising this knob is a free policy decision —
    /// no allocation, no per-instance memory regression for callers who
    /// stay at the default.
    ///
    /// Values outside `[1, kMaxConnHard]` are rejected by `create()` with
    /// `Error::InvalidConfig`.
    std::size_t max_connections{16};
};

} // namespace eph::net::dpdk
