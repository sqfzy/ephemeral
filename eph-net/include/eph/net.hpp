#pragma once

/// @file net.hpp
/// Convenience header — includes the eph-net public API.
///
/// Provides the socket-based WebSocket transport and all supporting types.
/// For DPDK transport, include eph/dpdk.hpp instead.

// Core concepts and types
#include "eph/core/tcp_concept.hpp"           // TcpTransport concept
#include "eph/transport/transport_types.hpp"   // TcpState, RttStats, ConnectionError, etc.

// Socket (POSIX) backend
#include "eph/net/socket_config.hpp"           // SocketConfig
#include "eph/net/socket_transport.hpp"        // SocketTransport
#include "eph/net/socket_connect.hpp"          // connect(), socket_wss_connect(), type aliases

// Transport templates (queue-backed, direct-TX, full-direct)
#include "eph/transport/transport.hpp"         // Transport<Tcp, Framer, ...>
#include "eph/transport/direct_tx_transport.hpp" // DirectTxTransport<...>
#include "eph/transport/direct_transport.hpp"  // DirectTransport<...>

// Framer implementations
#include "eph/core/framer_concept.hpp"         // Framer concept
#include "eph/transport/ws_framer.hpp"         // WsFramer (WebSocket RFC 6455)
#include "eph/core/length_prefix_framer.hpp"   // LengthPrefixFramer
#include "eph/transport/raw_framer.hpp"        // RawFramer (pass-through)
