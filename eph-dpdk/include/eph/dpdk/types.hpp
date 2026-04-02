#pragma once

/// @file types.hpp
/// DPDK-specific type aliases for the eph-transport generic transport.
///
/// Provides convenient aliases so users don't need to spell out
/// the full template parameters when using DPDK as the backend.

#include "eph/dpdk/tcp.hpp"
#include "eph/transport/presets.hpp"
#include "eph/transport/transport_types.hpp"

namespace eph::dpdk {

// ---------------------------------------------------------------------------
// Type aliases — DPDK backend
// ---------------------------------------------------------------------------
//
// Naming convention:
//   Dpdk{Wss|Raw}{Small|Large|Evict}Transport
//   - Wss = WebSocket over TLS (default)
//   - Raw = no framing (FIX or custom protocols)
//   - Small = 64B payload / 256 depth (compact single-symbol)
//   - Large = 4KB payload / 512 depth (JSON market data)
//   - Evict = EvictingQueue RX (drop stale, market data streams)
//   - (none) = 512B payload / 1024 depth (default)

/// @brief Default DPDK transport: WebSocket framing, 512-byte max payload, 1024-deep queue.
///
/// Suitable for typical exchange WebSocket feeds (Binance, Coinbase, etc.)
/// where individual messages fit in a single WebSocket frame under 512 bytes.
using DpdkTransport = eph::net::DefaultTransport<TcpSession<>>;

/// @brief Compact DPDK transport for control/heartbeat messages.
///
/// 64-byte max payload, 256-deep queue. Use for low-bandwidth control channels
/// (e.g., order acknowledgements, heartbeats) where memory footprint matters.
using DpdkSmallTransport = eph::net::SmallTransport<TcpSession<>>;

/// @brief Large DPDK transport for bulk data feeds.
///
/// 4KB max payload, 512-deep queue. Suitable for JSON market data (full order
/// book snapshots) or REST-over-WebSocket responses.
using DpdkLargeTransport = eph::net::LargeTransport<TcpSession<>>;

/// @brief Evicting DPDK transport for latest-value market data streams.
///
/// Under backpressure, drops the oldest unprocessed messages rather than
/// blocking the producer. Ideal for Level 1 quote feeds where only the
/// latest price matters.
using DpdkEvictTransport = eph::net::EvictTransport<TcpSession<>>;

/// @brief Raw TCP transport (no WebSocket framing) over DPDK.
///
/// Use for protocols that run directly over TCP without WebSocket framing,
/// such as FIX, SBE, or proprietary binary protocols.
using DpdkRawTransport = eph::net::RawTransport<TcpSession<>>;

/// @brief Direct TX DPDK transport: application calls send() directly, RX thread delivers via callback/queue.
///
/// Bypasses the internal TX thread — the caller's thread executes rte_eth_tx_burst.
/// Useful when the caller already runs on a dedicated lcore and wants to avoid
/// an extra thread hop on the send path.
using DpdkDirectTxTransport = eph::net::DirectTxDefaultTransport<TcpSession<>>;

/// @brief Direct TX raw DPDK transport (no WebSocket framing, no TX thread).
using DpdkDirectTxRawTransport = eph::net::DirectTxRawTransport<TcpSession<>>;

/// @brief Fully direct DPDK transport: no background threads, app calls send() + poll().
///
/// The application is responsible for both sending and polling. No internal
/// threads are created. Best for single-threaded event loop architectures
/// where the application manages all I/O on one lcore.
using DpdkDirectTransport = eph::net::DirectDefaultTransport<TcpSession<>>;

/// @brief Fully direct raw DPDK transport (no WebSocket framing, no threads).
using DpdkDirectRawTransport = eph::net::DirectRawTransport<TcpSession<>>;

/// @brief Re-exported generic transport types for convenience.
///
/// These are the backend-agnostic types from eph::net, re-exported into
/// eph::dpdk so callers don't need to import both namespaces.
using eph::net::TransportConfig;
using eph::net::TransportStats;
using eph::net::SendError;
using eph::net::TransportEvent;
using eph::net::TransportState;

} // namespace eph::dpdk
