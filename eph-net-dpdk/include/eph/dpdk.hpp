#pragma once

/// @file dpdk.hpp
/// Convenience umbrella header — includes the low-level DPDK primitives
/// that migrated into eph-net-dpdk from the deleted eph-dpdk module.
///
/// For the v3.3 high-level API (DpdkTcpStream / DpdkUdpSocket / DpdkPoller)
/// see `eph/net/dpdk/*.hpp` instead. This umbrella only exposes the raw
/// DPDK building blocks (EAL init, platform helpers, packet parse, UDP /
/// TCP byte-pipe primitives) that legacy tests and benchmarks still consume.

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/udp.hpp"
#include "eph/dpdk/tcp.hpp"
