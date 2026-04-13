#pragma once

/// @file eal.hpp
/// EAL lifecycle — intentionally separated from per-port Platform.
///
/// EAL is a once-per-process global; port configuration is per-NIC.
/// Mixing the two in one class forces awkward ownership semantics and
/// prevents multi-port setups.

#include <atomic>
#include <expected>
#include <format>
#include <string>

#include <rte_eal.h>
#include <rte_errno.h>

#include "eph/dpdk/detail/logger.hpp"

namespace eph::dpdk {

namespace detail {
inline spdlog::logger* eal_logger() { return get_logger<LoggerName{"dpdk.eal"}>(); }
} // namespace detail

/// Process-wide EAL initialization state. Guards against double-init
/// which is undefined behaviour in DPDK.
inline std::atomic<bool>& eal_initialized_flag() noexcept {
    static std::atomic<bool> flag{false};
    return flag;
}

/// @brief Initialize DPDK EAL (Environment Abstraction Layer).
///
/// Must be called exactly once per process, before any other rte_* API.
/// A second call returns an error without invoking `rte_eal_init()`.
/// Parses DPDK-specific command-line arguments (e.g., -l, --vdev, -a).
///
/// @param argc  Argument count (from main)
/// @param argv  Argument vector (from main)
/// @return Number of argv entries consumed by EAL on success, or error string
[[nodiscard]] inline std::expected<int, std::string> eal_init(int argc, char** argv) {
    [[maybe_unused]] auto log = detail::eal_logger();

    bool expected = false;
    if (!eal_initialized_flag().compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        SPDLOG_LOGGER_ERROR(log, "eal_init called twice — DPDK EAL is already initialized");
        return std::unexpected("EAL already initialized (double-init is DPDK UB)");
    }

    SPDLOG_LOGGER_TRACE(log, "Calling rte_eal_init (argc={})", argc);

    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        // Roll back the flag so a subsequent attempt can retry after
        // the caller fixes the EAL arguments.
        eal_initialized_flag().store(false, std::memory_order_release);
        return std::unexpected(std::format(
            "rte_eal_init failed (ret={}, rte_errno={}): {}",
            ret, rte_errno, rte_strerror(rte_errno)));
    }

    SPDLOG_LOGGER_DEBUG(log, "EAL initialized, {} args consumed", ret);
    return ret;
}

/// @brief Clean up EAL resources. Call once, after all ports are stopped and closed.
///
/// @warning After calling this function, no further rte_* API calls are valid.
/// @return true if cleanup succeeded, false on error (logged at ERROR level)
[[nodiscard]] inline bool eal_cleanup() noexcept {
    [[maybe_unused]] auto log = detail::eal_logger();
    SPDLOG_LOGGER_DEBUG(log, "Calling rte_eal_cleanup");
    int ret = rte_eal_cleanup();
    if (ret != 0) [[unlikely]] {
        SPDLOG_LOGGER_ERROR(log, "rte_eal_cleanup failed (ret={}, rte_errno={}): {}",
                     ret, rte_errno, rte_strerror(rte_errno));
        return false;
    }
    // Reset the process-wide flag so a subsequent init (in tests or
    // process-restart-in-place scenarios) is allowed.
    eal_initialized_flag().store(false, std::memory_order_release);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// RAII guard
// ─────────────────────────────────────────────────────────────────────────────

/// Move-only RAII guard for EAL lifetime.
///
/// Calls eal_cleanup() on destruction, ensuring cleanup even on error paths.
/// Use the static factory `init()` instead of the constructor.
///
///   auto eal = EalGuard::init(argc, argv);
///   if (!eal) { spdlog::error("{}", eal.error()); return 1; }
///   // ... use DPDK APIs ...
///   // eal_cleanup() called automatically when `eal` goes out of scope
class EalGuard {
public:
    /// Initialize EAL and return a guard that cleans up on destruction.
    [[nodiscard]] static std::expected<EalGuard, std::string> init(int argc, char** argv) {
        auto result = eal_init(argc, argv);
        if (!result) return std::unexpected(result.error());
        SPDLOG_LOGGER_DEBUG(detail::eal_logger(), "EalGuard created ({} args consumed)", *result);
        return EalGuard{*result};
    }

    ~EalGuard() {
        if (initialized_) {
            SPDLOG_LOGGER_DEBUG(detail::eal_logger(), "EalGuard destroying, calling eal_cleanup");
            [[maybe_unused]] bool ok = eal_cleanup();
        }
    }

    EalGuard(EalGuard&& other) noexcept
        : initialized_{other.initialized_}
        , args_consumed_{other.args_consumed_} {
        other.initialized_ = false;
    }

    /// @warning Move-assign calls eal_cleanup() on the target before
    ///          transferring ownership. Only safe when no DPDK lcores are
    ///          running on the target guard.
    EalGuard& operator=(EalGuard&& other) noexcept {
        if (this != &other) {
            if (initialized_) [[maybe_unused]] bool ok = eal_cleanup();
            initialized_ = other.initialized_;
            args_consumed_ = other.args_consumed_;
            other.initialized_ = false;
        }
        return *this;
    }

    EalGuard(const EalGuard&)            = delete;
    EalGuard& operator=(const EalGuard&) = delete;

    /// Number of argv entries consumed by EAL during initialization.
    [[nodiscard]] int args_consumed() const noexcept { return args_consumed_; }

    /// Check whether this guard holds an initialized EAL instance.
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    /// Convenience bool conversion for validity checking.
    /// Usage: if (eal) { /* EAL is initialized */ }
    [[nodiscard]] explicit operator bool() const noexcept { return initialized_; }

private:
    explicit EalGuard(int args_consumed) noexcept
        : initialized_{true}, args_consumed_{args_consumed} {}

    bool initialized_   = false;
    int  args_consumed_  = 0;
};

} // namespace eph::dpdk
