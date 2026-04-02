#pragma once

/// @file presets.hpp
/// Transport preset aliases — canonical payload/depth/framer/mode combinations.
///
/// Backend modules (eph-net, eph-dpdk) specialize these with their TCP
/// implementation to create convenient user-facing type aliases.
///
/// Naming convention:
///   {Default|Small|Large|Evict|Raw}Transport<TcpImpl>       — kThreaded (default)
///   DirectTx{Default|...}Transport<TcpImpl>                 — kDirectTx
///   Direct{Default|...}Transport<TcpImpl>                   — kDirect

#include "eph/transport/transport.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/transport/raw_framer.hpp"
#include "eph/containers/evicting_queue.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// kThreaded presets (default — backward compatible)
// ---------------------------------------------------------------------------

template <typename TcpImpl>
using DefaultTransport = Transport<TcpImpl, WsFramer, TransportMode::kThreaded, 512, 1024>;

template <typename TcpImpl>
using SmallTransport = Transport<TcpImpl, WsFramer, TransportMode::kThreaded, 64, 256>;

template <typename TcpImpl>
using LargeTransport = Transport<TcpImpl, WsFramer, TransportMode::kThreaded, 4096, 512>;

template <typename TcpImpl>
using EvictTransport = Transport<TcpImpl, WsFramer, TransportMode::kThreaded, 512, 1024,
                                  eph::containers::EvictingQueue>;

template <typename TcpImpl>
using RawTransport = Transport<TcpImpl, RawFramer, TransportMode::kThreaded, 512, 1024>;

// ---------------------------------------------------------------------------
// kDirectTx presets — app sends directly, RX thread for receive
// ---------------------------------------------------------------------------

template <typename TcpImpl>
using DirectTxTransport = Transport<TcpImpl, WsFramer, TransportMode::kDirectTx, 512, 1024>;

template <typename TcpImpl>
using DirectTxSmallTransport = Transport<TcpImpl, WsFramer, TransportMode::kDirectTx, 64, 256>;

template <typename TcpImpl>
using DirectTxRawTransport = Transport<TcpImpl, RawFramer, TransportMode::kDirectTx, 512, 1024>;

// ---------------------------------------------------------------------------
// kDirect presets — app does both TX and RX, no background threads
// ---------------------------------------------------------------------------

template <typename TcpImpl>
using DirectTransport = Transport<TcpImpl, WsFramer, TransportMode::kDirect, 512, 1024>;

template <typename TcpImpl>
using DirectSmallTransport = Transport<TcpImpl, WsFramer, TransportMode::kDirect, 64, 256>;

template <typename TcpImpl>
using DirectRawTransport = Transport<TcpImpl, RawFramer, TransportMode::kDirect, 512, 1024>;

} // namespace eph::net
