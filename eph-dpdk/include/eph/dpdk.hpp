#pragma once

/// @file dpdk.hpp
/// Convenience header — includes the eph-dpdk public API.
///
/// Provides DPDK-based user-space TCP transport, platform initialization,
/// and the high-level connector for one-shot DPDK connection setup.

#include "eph/dpdk/eal.hpp"
#include "eph/dpdk/platform.hpp"
#include "eph/dpdk/connector.hpp"
#include "eph/dpdk/udp.hpp"
#include "eph/dpdk/types.hpp"
