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
// Naming convention:
//   Dpdk{Wss|Raw}{Small|Large|Evict}Transport
//   - Wss = WebSocket over TLS (default)
//   - Raw = no framing (FIX or custom protocols)
//   - Small = 64B payload / 256 depth (compact single-symbol)
//   - Large = 4KB payload / 512 depth (JSON market data)
//   - Evict = EvictingQueue RX (drop stale, market data streams)
//   - (none) = 512B payload / 1024 depth (default)

/// Default DPDK transport: WsFramer, 512-byte max payload, 1024-deep queue.
using DpdkTransport = eph::net::DefaultTransport<TcpSession<>>;

/// Small DPDK transport for control messages.
using DpdkSmallTransport = eph::net::SmallTransport<TcpSession<>>;

/// Large DPDK transport for bulk data.
using DpdkLargeTransport = eph::net::LargeTransport<TcpSession<>>;

/// Latest-value transport — under backpressure, drops older messages.
using DpdkEvictTransport = eph::net::EvictTransport<TcpSession<>>;

/// Raw TCP transport (no WebSocket framing) over DPDK.
using DpdkRawTransport = eph::net::RawTransport<TcpSession<>>;

// Direct TX mode: app sends directly, RX thread delivers via callback/queue.
using DpdkDirectTxTransport = eph::net::DirectTxDefaultTransport<TcpSession<>>;
using DpdkDirectTxRawTransport = eph::net::DirectTxRawTransport<TcpSession<>>;

// Full direct mode: no background threads, app calls send() + poll().
using DpdkDirectTransport = eph::net::DirectDefaultTransport<TcpSession<>>;
using DpdkDirectRawTransport = eph::net::DirectRawTransport<TcpSession<>>;

/// Re-export generic types for convenience.
using eph::net::TransportConfig;
using eph::net::TransportStats;
using eph::net::SendError;
using eph::net::TransportEvent;
using eph::net::TransportState;

} // namespace eph::dpdk
