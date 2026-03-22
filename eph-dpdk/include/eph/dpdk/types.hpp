#pragma once

/// @file types.hpp
/// DPDK-specific type aliases for the eph-net generic transport.
///
/// Provides convenient aliases so users don't need to spell out
/// the full template parameters when using DPDK as the backend.

#include "eph/dpdk/tcp.hpp"
#include "eph/net/transport.hpp"
#include "eph/net/transport_types.hpp"

namespace eph::dpdk {

/// Default DPDK transport: 512-byte max payload, 1024-deep queue.
using DpdkTransport = eph::net::Transport<TcpSession<>, 512, 1024>;

/// Small DPDK transport for control messages.
using DpdkSmallTransport = eph::net::Transport<TcpSession<>, 64, 256>;

/// Large DPDK transport for bulk data.
using DpdkLargeTransport = eph::net::Transport<TcpSession<>, 4096, 512>;

/// Re-export generic types for convenience.
using eph::net::TransportConfig;
using eph::net::TransportStats;
using eph::net::SendError;
using eph::net::TransportEvent;
using eph::net::TransportState;

} // namespace eph::dpdk
