#pragma once

/// @file types.hpp
/// DPDK-specific type aliases for the eph-net generic transport.
///
/// Provides convenient aliases so users don't need to spell out
/// the full template parameters when using DPDK as the backend.

// ARCHITECTURE NOTE: eph-dpdk includes eph-net headers directly via
// add_includedirs("eph-net/include") in xmake.lua instead of add_deps("eph-net").
// This is intentional: eph-net depends on aws-lc for TLS, and DPDK brings its
// own OpenSSL via vcpkg. Linking both causes symbol conflicts. By including
// only the header path, eph-dpdk gets the type definitions it needs (Transport,
// RawFramer, TransportConfig/Stats/etc.) without inheriting aws-lc.
//
// The headers used here (raw_framer.hpp, transport.hpp, transport_types.hpp)
// live in eph-net proper (not forwarding headers to eph-core). tcp_concept.hpp
// (used by tcp.hpp) is a forwarding header that redirects to eph-core.
//
// MAINTENANCE: if new eph-net headers are added here, verify they do not
// transitively pull in aws-lc / TLS headers. If they do, consider extracting
// the needed types into eph-core first.
#include "eph/dpdk/tcp.hpp"
#include "eph/net/raw_framer.hpp"
#include "eph/net/transport.hpp"
#include "eph/net/transport_types.hpp"

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
using DpdkTransport = eph::net::Transport<TcpSession<>, eph::net::WsFramer, 512, 1024>;

/// Small DPDK transport for control messages.
using DpdkSmallTransport = eph::net::Transport<TcpSession<>, eph::net::WsFramer, 64, 256>;

/// Large DPDK transport for bulk data.
using DpdkLargeTransport = eph::net::Transport<TcpSession<>, eph::net::WsFramer, 4096, 512>;

/// Latest-value transport — under backpressure, drops older messages
/// to deliver only the most recent. Same queue depth as DpdkTransport;
/// adjust if staleness-sensitive workloads need shallower queues.
using DpdkEvictTransport = eph::net::Transport<TcpSession<>, eph::net::WsFramer, 512, 1024,
                                                eph::containers::EvictingQueue>;

/// Raw TCP transport (no WebSocket framing) over DPDK.
using DpdkRawTransport = eph::net::Transport<TcpSession<>, eph::net::RawFramer, 512, 1024>;

/// Re-export generic types for convenience.
using eph::net::TransportConfig;
using eph::net::TransportStats;
using eph::net::SendError;
using eph::net::TransportEvent;
using eph::net::TransportState;

} // namespace eph::dpdk
