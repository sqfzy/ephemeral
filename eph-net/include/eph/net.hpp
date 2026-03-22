#pragma once

/// @file net.hpp
/// Convenience header — includes the eph-net public API.
///
/// Provides the socket-based WebSocket transport and all supporting types.
/// For DPDK transport, include eph/dpdk.hpp instead.

#include "eph/net/tcp_concept.hpp"
#include "eph/net/socket_transport.hpp"
