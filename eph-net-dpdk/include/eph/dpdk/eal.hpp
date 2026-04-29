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
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rte_eal.h>
#include <rte_errno.h>

#include "eph/dpdk/detail/logger.hpp"
#include "eph/dpdk/lcore_pin.hpp"   // LcorePin / pin_lcore / pin_lcores / build_lcore_argv
#include "eph/dpdk/proc_type.hpp"   // ProcType + to_eal_string (M3 fix)

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
        , args_consumed_{other.args_consumed_}
        , pin_guards_{std::move(other.pin_guards_)} {
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
            pin_guards_ = std::move(other.pin_guards_);
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

    /// @brief Single-call factory: validate lcore pins, register them,
    ///        build EAL argv, call rte_eal_init, transfer pin ownership
    ///        into the returned EalGuard.
    ///
    /// Sequence (any failure rolls back atomically):
    ///   1. `pin_lcores(pins, policy)` — pre-EAL pin validation + registry
    ///      write. On failure no `rte_eal_*` API was touched; the partial
    ///      `vector<PinGuard>` going out of scope unregisters every cpu
    ///      staged so far via each guard's dtor.
    ///   2. If `pins` is non-empty, append `build_lcore_argv(pins)` as a
    ///      single `--lcores=...` token to `cfg.extra_args`. **`cfg.lcores`
    ///      must be empty** when `pins` is non-empty — mixing the typed
    ///      and raw paths in one call is a configuration bug and is
    ///      rejected with `unexpected`.
    ///   3. `build_eal_argv(cfg)` and `eal_init(argc, argv)`. On EAL
    ///      failure the local `vector<PinGuard>` destructor unregisters
    ///      every cpu so the registry returns to its pre-call state.
    ///   4. On success, transfer the `vector<PinGuard>` into the returned
    ///      `EalGuard` (`pin_guards_` field).
    ///
    /// Destruction order in the returned guard: `~EalGuard()` body runs
    /// `eal_cleanup()` first, then field destruction (in reverse declaration
    /// order) destroys `pin_guards_`, whose elements unregister the lcore cpus.
    ///
    /// @param cfg     EAL configuration. `cfg.lcores` must be empty when
    ///                @p pins is non-empty (escape-hatch / typed-path mutual
    ///                exclusion).
    /// @param pins    1:1 lcore→cpu spec. Empty span behaves identically
    ///                to the legacy `init()` path.
    /// @param policy  validation strictness; default `CpuPinPolicy{}`
    ///                (relaxed) matches every existing in-repo pin_thread
    ///                caller's default.
    [[nodiscard]] static std::expected<EalGuard, std::string>
    init_with_pins(struct EalConfig          cfg,
                   std::span<LcorePin const> pins,
                   eph::utils::CpuPinPolicy  policy = {});

private:
    explicit EalGuard(int args_consumed) noexcept
        : initialized_{true}, args_consumed_{args_consumed} {}

    /// Used by `init_with_pins` to transfer pin ownership into the guard.
    EalGuard(int args_consumed,
             std::vector<eph::utils::PinGuard>&& pin_guards) noexcept
        : initialized_{true}
        , args_consumed_{args_consumed}
        , pin_guards_{std::move(pin_guards)} {}

    bool                  initialized_    = false;
    int                   args_consumed_  = 0;
    /// Owns the cpus registered via `init_with_pins`. The
    /// EAL-cleanup-then-pin-release ordering is guaranteed by C++
    /// destruction rules: `~EalGuard()`'s body runs `eal_cleanup()`
    /// before any field destructor fires, then each `PinGuard` in the
    /// vector unregisters its cpu from the process-wide pin registry.
    /// Position among the other fields is therefore irrelevant.
    std::vector<eph::utils::PinGuard> pin_guards_{};
};

// ─────────────────────────────────────────────────────────────────────────────
// Multi-process EAL argv assembly
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Declarative EAL args for multi-process setups.
///
/// Assembles into a DPDK-compatible argv via `build_eal_argv()`. Exists so
/// callers don't hand-splice argv strings and accidentally put `--file-prefix`
/// in the wrong slot or forget `--proc-type=secondary`. Nothing here is
/// surfaced to DPDK at construction time — it's a pure struct that gets
/// serialized on demand.
///
/// Always includes argv[0] (program name) so the result is usable directly
/// with `eal_init` / `rte_eal_init`.
struct EalConfig {
    std::string              program_name{"eph_app"};   ///< argv[0]
    ProcType                 proc_type   {};            ///< Primary if default
    bool                     proc_type_set{false};      ///< explicit opt-in
    std::string_view         file_prefix {};            ///< --file-prefix
    std::vector<std::string> lcores      {};            ///< one per -l entry
    std::vector<std::string> allowed_devs{};            ///< one per -a entry
    std::vector<std::string> extra_args  {};            ///< raw passthrough
};

/// @brief Serialize an EalConfig to a flat argv vector.
///
/// The returned vector owns every string; pass `argv.data()` / `argv.size()`
/// to `eal_init`. Empty / default-constructed fields are skipped (so a
/// Primary config with no file_prefix / lcores emits just `{program_name}`).
[[nodiscard]] inline std::vector<std::string>
build_eal_argv(const EalConfig& cfg) {
    std::vector<std::string> argv;
    argv.reserve(2 + cfg.lcores.size() * 2 + cfg.allowed_devs.size() * 2
                   + cfg.extra_args.size() + 4);
    argv.push_back(cfg.program_name);

    if (cfg.proc_type_set) {
        argv.emplace_back("--proc-type");
        // Single source of truth in proc_type.hpp — adding a new enum
        // value automatically reaches this serializer (and triggers
        // -Wswitch on the to_eal_string switch if not handled).
        argv.emplace_back(std::string{to_eal_string(cfg.proc_type)});
    }
    if (!cfg.file_prefix.empty()) {
        argv.emplace_back("--file-prefix");
        argv.emplace_back(cfg.file_prefix);
    }
    for (const auto& lc : cfg.lcores) {
        argv.emplace_back("-l");
        argv.push_back(lc);
    }
    for (const auto& dev : cfg.allowed_devs) {
        argv.emplace_back("-a");
        argv.push_back(dev);
    }
    for (const auto& e : cfg.extra_args) argv.push_back(e);
    return argv;
}

// ─────────────────────────────────────────────────────────────────────────────
// EalGuard::init_with_pins out-of-line definition.
// EalConfig must be visible (declared above), so this lives after EalConfig
// rather than inside the class body.
// ─────────────────────────────────────────────────────────────────────────────
inline std::expected<EalGuard, std::string>
EalGuard::init_with_pins(EalConfig                 cfg,
                         std::span<LcorePin const> pins,
                         eph::utils::CpuPinPolicy  policy) {
    [[maybe_unused]] auto* log = detail::eal_logger();

    if (!pins.empty() && !cfg.lcores.empty()) {
        return std::unexpected(
            "init_with_pins: cfg.lcores must be empty when typed pins "
            "are supplied (escape hatch and typed path are mutually "
            "exclusive in one call)");
    }

    // Step 1: pre-EAL pin registration. This is where SMT / NUMA / IRQ
    // / duplicate-cpu policy is enforced. Failure here means no DPDK
    // resources have been touched yet.
    auto pin_guards = pin_lcores(pins, policy);
    if (!pin_guards) {
        return std::unexpected(std::format(
            "init_with_pins: {}", pin_guards.error()));
    }

    // Step 2: inject --lcores=... into argv (only if user supplied pins).
    if (!pins.empty()) {
        cfg.extra_args.push_back(build_lcore_argv(pins));
    }

    // Step 3: build argv and call rte_eal_init. argv strings must outlive
    // the rte_eal_init call; we keep them alive in `argv_strings` and pass
    // `argv_ptrs.data()`.
    auto argv_strings = build_eal_argv(cfg);
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argv_strings.size());
    for (auto& s : argv_strings) argv_ptrs.push_back(s.data());

    SPDLOG_LOGGER_DEBUG(log,
        "init_with_pins: calling rte_eal_init (argc={}, lcores={})",
        argv_ptrs.size(), pins.size());

    auto eal_result = eal_init(static_cast<int>(argv_ptrs.size()),
                               argv_ptrs.data());
    if (!eal_result) {
        // pin_guards destructor (here on stack) unregisters every cpu via
        // each PinGuard's dtor as we return. Caller sees a clean slate.
        return std::unexpected(std::format(
            "init_with_pins: {}", eal_result.error()));
    }

    // Step 4: success — transfer pin ownership into the EalGuard.
    return EalGuard{*eal_result, std::move(*pin_guards)};
}

} // namespace eph::dpdk
