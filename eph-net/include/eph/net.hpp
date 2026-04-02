#pragma once

/// @file net.hpp
/// Convenience header — includes the eph-net public API.
///
/// Provides the socket-based WebSocket transport and all supporting types.
/// For DPDK transport, include eph/dpdk.hpp instead.

#include "eph/core/tcp_concept.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/net/socket_config.hpp"
#include "eph/net/socket_transport.hpp"
#include "eph/net/socket_connect.hpp"
#include "eph/transport/transport.hpp"
#include "eph/core/framer_concept.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/core/length_prefix_framer.hpp"
#include "eph/transport/raw_framer.hpp"
