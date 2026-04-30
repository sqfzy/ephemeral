#pragma once

/// @file eal.hpp
/// DPDK EAL RAII guard re-exported under `eph::net::dpdk`.
///
/// The underlying implementation lives in `eph-net-dpdk/include/eph/dpdk/eal.hpp`
/// (the directory was renamed from `eph-dpdk/` to `eph-net-dpdk/` in phase 7,
/// while the inner `eph::dpdk::` namespace was preserved).
/// This header provides a typedef so user code can write
///
///     eph::net::dpdk::Eal eal = ...;
///
/// without including the internal namespace.

#include "eph/dpdk/eal.hpp"

namespace eph::net::dpdk {

/// @brief Move-only RAII guard for DPDK EAL lifetime.
///
/// Same semantics as `eph::dpdk::EalGuard::init(argc, argv)`:
/// returns `std::expected<Eal, std::string>` from the static factory and
/// calls `eal_cleanup()` on destruction.
using Eal = ::eph::dpdk::EalGuard;

/// @brief Free-function EAL initializer (stateless). Prefer `Eal::init`
///        for RAII safety; this alias exists for parity with the design
///        doc example which calls the bare function.
using ::eph::dpdk::eal_init;

/// @brief Free-function EAL teardown (stateless). Prefer `Eal` RAII.
using ::eph::dpdk::eal_cleanup;

} // namespace eph::net::dpdk
