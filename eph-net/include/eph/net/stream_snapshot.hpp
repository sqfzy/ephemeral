#pragma once

/// @file stream_snapshot.hpp
/// @brief Cross-backend post-create stream state snapshot.
///
/// `StreamSnapshot` is the **read counterpart** of `StreamConfig`:
/// the user wrote a `StreamConfig` to express intent, the library
/// returns a `StreamSnapshot` to disclose what actually resolved.
/// Sub-struct names mirror `StreamConfig`'s sub-structs verbatim
/// (`Tls` / `Ws` / `Keepalive` / `Dpdk`) so reading a snapshot is
/// the same vocabulary as writing the config — no translation tax.
///
/// Returned by `Stream::snapshot()` / `Datagram::snapshot()` on every
/// stream and socket type (kernel + DPDK, TCP + UDP). Backend-specific
/// fields (e.g. `Dpdk::rx_queue`) are zero-filled on backends where
/// they don't apply, so cross-backend code does not need to branch.
///
/// Semantics: by-value snapshot at the point of call. Fields like
/// `tcp.effective_mss` may evolve later (ICMP Frag Needed shrinks it),
/// so callers wanting fresh values should call `snapshot()` again.
/// The struct is cheap to copy (~120 bytes); not on any hot path.
///
/// `string_view` fields point into stream-owned storage (`cfg_.ws.path`,
/// `cfg_.tls.hostname`, etc.) and remain valid for the stream's lifetime.
/// Do not retain them past the owning stream.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace eph::net {

/// @brief Post-create stream state snapshot. See file header.
struct StreamSnapshot {
    /// @brief 5-tuple actually in use after factory resolution.
    ///
    /// `src_port_rewritten` is true when the library mutated the
    /// requested src_port (today only on DPDK + RSS-pinned where
    /// `find_src_port_for_queue` reverse-picks an ephemeral port that
    /// hashes to the target queue). Always false on Software / FD
    /// modes and on the kernel backend.
    struct Endpoint {
        uint32_t src_ip   = 0;   ///< host byte order
        uint16_t src_port = 0;
        uint32_t dst_ip   = 0;   ///< host byte order
        uint16_t dst_port = 0;
        bool     src_port_rewritten = false;
    } endpoint;

    /// @brief TCP-level runtime state. Inert (`enabled=false`) on
    ///        UDP sockets, where every other field is meaningless.
    struct Tcp {
        bool     enabled              = false;  ///< false on UDP
        uint16_t recv_window          = 0;      ///< advertised rwnd, no scaling
        uint16_t local_mss            = 0;      ///< requested MSS (cfg)
        uint16_t peer_mss             = 0;      ///< MSS from peer SYN-ACK, 0 if not negotiated
        uint16_t effective_mss        = 0;      ///< min(local, peer) further clamped by ICMP PMTU
        bool     peer_mss_negotiated  = false;  ///< true once SYN-ACK MSS option was applied
        bool     icmp_pmtu_shrunk     = false;  ///< true if any ICMP Frag-Needed shrunk effective_mss
    } tcp;

    /// @brief Keepalive state. `active=false` means the feature is
    ///        disabled (no probes will fire). `interval` and `probes`
    ///        echo the config that was applied; the kernel backend may
    ///        report values rounded to seconds (TCP_KEEPIDLE is integer
    ///        seconds), the DPDK backend reports them verbatim.
    struct Keepalive {
        bool                       active   = false;
        std::chrono::milliseconds  interval{};
        uint8_t                    probes   = 0;
    } keepalive;

    /// @brief TLS state. `enabled` reflects the compile-time
    ///        `EnableTls` template parameter — non-TLS stream types
    ///        always report `enabled=false` and ignore the rest.
    ///
    /// `sni` echoes the hostname sent in ClientHello (= cfg.tls.hostname).
    /// `was_resumed` is true if the just-completed handshake used a
    /// TLS 1.3 NewSessionTicket. `send_desynced` flips on a TLS
    /// partial-send sequence corruption — both backends now latch this
    /// (kernel via `KernelTcpStream::send`'s `tls_corrupt_` flag, DPDK
    /// via `DpdkTcpStream::send`'s `tls_corrupt_` field; both bump
    /// `kTlsSendDesyncs`). After `send_desynced=true` the stream rejects
    /// further send/recv and expects a reconnect.
    struct Tls {
        bool             enabled       = false;
        bool             was_resumed   = false;
        bool             send_desynced = false;
        std::string_view sni{};
    } tls;

    /// @brief WebSocket state. `enabled` is true when the user supplied
    ///        a non-empty `cfg.ws.path` and the handshake completed.
    ///        `permessage_deflate_active` is true only when the offer
    ///        was made AND the server accepted it.
    struct Ws {
        bool             enabled                   = false;
        std::string_view path{};
        std::string_view host{};
        bool             permessage_deflate_active = false;
    } ws;

    /// @brief DPDK backend-specific resolution. All fields are zero on
    ///        the kernel backend (the struct still exists so cross-
    ///        backend diagnostic code does not need to branch).
    ///
    /// `rx_queue` / `tx_queue` are the queues this stream actually
    /// landed on (after Software / RSS-pin / FlowDirector resolution).
    /// `pool_lcore_hint_resolved` is the per-lcore mempool index used
    /// (-1 = single shared pool). `flow_rule_handle` is non-empty only
    /// when a `rte_flow` 5-tuple steering rule was installed for this
    /// stream (FlowDirector mode); the value is the opaque rule handle
    /// for diagnostic-log correlation, not for direct manipulation.
    struct Dpdk {
        uint16_t                rx_queue                 = 0;
        uint16_t                tx_queue                 = 0;
        int                     pool_lcore_hint_resolved = -1;
        std::optional<uint64_t> flow_rule_handle{};
    } dpdk;
};

} // namespace eph::net
