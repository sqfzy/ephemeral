#pragma once

/// @file core/dpdk_env.hpp
/// Thin compat shim — the real implementation lives in
/// eph-dpdk/include/eph/dpdk/test/dpdk_env.hpp.
///
/// New consumers should #include "eph/dpdk/test/dpdk_env.hpp" directly
/// and use ::eph::dpdk::test::DpdkBenchEnv.

#ifdef EPH_USE_DPDK

#include "eph/dpdk/test/dpdk_env.hpp"

namespace bench {

using ::eph::dpdk::test::DpdkBenchEnv;

} // namespace bench

#endif // EPH_USE_DPDK
