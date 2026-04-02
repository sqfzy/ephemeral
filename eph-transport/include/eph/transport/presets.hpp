#pragma once

/// @file presets.hpp
/// Transport preset aliases — canonical payload/depth/framer combinations.
///
/// Backend modules (eph-net, eph-dpdk) specialize these with their TCP
/// implementation to create convenient user-facing type aliases.
///
/// Naming convention:
///   {Default|Small|Large|Evict|Raw}Transport<TcpImpl>
///   - Default = 512B payload / 1024 depth / WsFramer
///   - Small   = 64B  payload / 256  depth / WsFramer
///   - Large   = 4KB  payload / 512  depth / WsFramer
///   - Evict   = 512B payload / 1024 depth / WsFramer / EvictingQueue RX
///   - Raw     = 512B payload / 1024 depth / RawFramer

#include "eph/transport/transport.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/transport/raw_framer.hpp"
#include "eph/containers/evicting_queue.hpp"

namespace eph::net {

/// Default transport: WsFramer, 512-byte max payload, 1024-deep queue.
template <typename TcpImpl>
using DefaultTransport = Transport<TcpImpl, WsFramer, 512, 1024>;

/// Small transport for control messages: 64-byte payload, 256-deep queue.
template <typename TcpImpl>
using SmallTransport = Transport<TcpImpl, WsFramer, 64, 256>;

/// Large transport for bulk data: 4KB payload, 512-deep queue.
template <typename TcpImpl>
using LargeTransport = Transport<TcpImpl, WsFramer, 4096, 512>;

/// Latest-value transport — under backpressure, drops older messages.
template <typename TcpImpl>
using EvictTransport = Transport<TcpImpl, WsFramer, 512, 1024,
                                  eph::containers::EvictingQueue>;

/// Raw TCP transport (no framing overhead).
template <typename TcpImpl>
using RawTransport = Transport<TcpImpl, RawFramer, 512, 1024>;

} // namespace eph::net
