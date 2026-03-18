#pragma once

/// @file eal.hpp
/// EAL lifecycle — intentionally separated from per-port Platform.
///
/// EAL is a once-per-process global; port configuration is per-NIC.
/// Mixing the two in one class forces awkward ownership semantics and
/// prevents multi-port setups.

#include <expected>
#include <format>
#include <string>

#include <rte_eal.h>
#include <rte_errno.h>

#include <spdlog/spdlog.h>

namespace eph::dpdk {

/// Initialize DPDK EAL.  Must be called exactly once per process,
/// before any other rte_* API.
///
/// @return Number of argv entries consumed by EAL on success.
inline std::expected<int, std::string> eal_init(int argc, char** argv) {
    SPDLOG_TRACE("Calling rte_eal_init (argc={})", argc);

    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        return std::unexpected(std::format(
            "rte_eal_init failed (ret={}, rte_errno={}): {}",
            ret, rte_errno, rte_strerror(rte_errno)));
    }

    SPDLOG_DEBUG("EAL initialized, {} args consumed", ret);
    return ret;
}

/// Clean up EAL resources.  Call once, after all ports are closed.
inline void eal_cleanup() noexcept {
    SPDLOG_DEBUG("Calling rte_eal_cleanup");
    rte_eal_cleanup();
}

} // namespace eph::dpdk
