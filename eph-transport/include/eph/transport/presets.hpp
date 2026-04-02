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
// Threaded presets (default -- TX thread + RX thread + SPSC queues)
// ---------------------------------------------------------------------------

/// @brief Default threaded transport: WS framing, 512B payload, 1024-deep queues.
template <typename TcpImpl>
using DefaultTransport = Transport<TcpImpl, WsFramer, 512, 1024>;

/// @brief Small-message threaded transport: WS framing, 64B payload, 256-deep queues.
template <typename TcpImpl>
using SmallTransport = Transport<TcpImpl, WsFramer, 64, 256>;

/// @brief Large-message threaded transport: WS framing, 4096B payload, 512-deep queues.
template <typename TcpImpl>
using LargeTransport = Transport<TcpImpl, WsFramer, 4096, 512>;

/// @brief Evicting threaded transport: overwrites oldest unread message on RX overflow.
/// @note Ideal for market data streams where only the latest value matters.
template <typename TcpImpl>
using EvictTransport = Transport<TcpImpl, WsFramer, 512, 1024,
                                  eph::containers::EvictingQueue>;

/// @brief Raw (non-WebSocket) threaded transport: no framing overhead, 512B payload.
template <typename TcpImpl>
using RawTransport = Transport<TcpImpl, RawFramer, 512, 1024>;

// ---------------------------------------------------------------------------
// DirectTx presets -- app sends directly, RX thread for receive
// ---------------------------------------------------------------------------

/// @brief Direct-TX default: app thread sends synchronously, RX thread handles receive.
template <typename TcpImpl>
using DirectTxDefaultTransport = DirectTxTransport<TcpImpl, WsFramer, 512, 1024>;

/// @brief Direct-TX small: 64B payload, 256-deep RX queue.
template <typename TcpImpl>
using DirectTxSmallTransport = DirectTxTransport<TcpImpl, WsFramer, 64, 256>;

/// @brief Direct-TX raw: no WS framing, app thread sends directly.
template <typename TcpImpl>
using DirectTxRawTransport = DirectTxTransport<TcpImpl, RawFramer, 512, 1024>;

// ---------------------------------------------------------------------------
// Direct presets -- app does both TX and RX, no background threads
// ---------------------------------------------------------------------------

/// @brief Fully direct (threadless) transport: app thread handles all I/O.
/// @note Best for single-threaded event loops (Reactor, io_uring, DPDK poll-mode).
template <typename TcpImpl>
using DirectDefaultTransport = DirectTransport<TcpImpl, WsFramer, 512>;

/// @brief Fully direct small: 64B payload, threadless.
template <typename TcpImpl>
using DirectSmallTransport = DirectTransport<TcpImpl, WsFramer, 64>;

/// @brief Fully direct raw: no WS framing, threadless.
template <typename TcpImpl>
using DirectRawTransport = DirectTransport<TcpImpl, RawFramer, 512>;

} // namespace eph::net
