#pragma once

/// @file presets.hpp
/// Transport preset aliases — canonical payload/depth/framer combinations.
///
/// Backend modules (eph-net, eph-dpdk) specialize these with their TCP
/// implementation to create convenient user-facing type aliases.
///
/// Naming convention:
///   {Default|Small|Large|Evict|Raw}Transport<TcpImpl>       — threaded (default)
///   DirectTx{Default|...}Transport<TcpImpl>                 — direct TX
///   Direct{Default|...}Transport<TcpImpl>                   — full direct

#include "eph/transport/transport.hpp"
#include "eph/transport/direct_tx_transport.hpp"
#include "eph/transport/direct_transport.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/transport/raw_framer.hpp"
#include "eph/containers/evicting_queue.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// Threaded presets (default — TX thread + RX thread + SPSC queues)
// ---------------------------------------------------------------------------

template <typename TcpImpl>
using DefaultTransport = Transport<TcpImpl, WsFramer, 512, 1024>;

template <typename TcpImpl>
using SmallTransport = Transport<TcpImpl, WsFramer, 64, 256>;

template <typename TcpImpl>
using LargeTransport = Transport<TcpImpl, WsFramer, 4096, 512>;

template <typename TcpImpl>
using EvictTransport = Transport<TcpImpl, WsFramer, 512, 1024,
                                  eph::containers::EvictingQueue>;

template <typename TcpImpl>
using RawTransport = Transport<TcpImpl, RawFramer, 512, 1024>;

// ---------------------------------------------------------------------------
// DirectTx presets — app sends directly, RX thread for receive
// ---------------------------------------------------------------------------

template <typename TcpImpl>
using DirectTxDefaultTransport = DirectTxTransport<TcpImpl, WsFramer, 512, 1024>;

template <typename TcpImpl>
using DirectTxSmallTransport = DirectTxTransport<TcpImpl, WsFramer, 64, 256>;

template <typename TcpImpl>
using DirectTxRawTransport = DirectTxTransport<TcpImpl, RawFramer, 512, 1024>;

// ---------------------------------------------------------------------------
// Direct presets — app does both TX and RX, no background threads
// ---------------------------------------------------------------------------

template <typename TcpImpl>
using DirectDefaultTransport = DirectTransport<TcpImpl, WsFramer, 512>;

template <typename TcpImpl>
using DirectSmallTransport = DirectTransport<TcpImpl, WsFramer, 64>;

template <typename TcpImpl>
using DirectRawTransport = DirectTransport<TcpImpl, RawFramer, 512>;

} // namespace eph::net
