#pragma once

/// @file lcore_pin.hpp
/// EAL lcore × cpu pin integration — declarative LcorePin spec, pure-function
/// `--lcores` argv builder, and (in stage 5) RAII-managed registration into
/// `eph::utils`'s process-wide pin registry.
///
/// Why a typed spec instead of writing `EalConfig::lcores` strings directly?
/// DPDK's `--lcores` syntax has a long tail (`(0-3)@(8,9)` set-of-sets) that
/// is a footgun to parse correctly. By making the caller fill structured
/// `LcorePin{lcore_id, cpu_id, role}` values, we avoid writing a parser, get
/// pre-EAL validation against the same registry that `pin_thread` consults,
/// and feed `role` into conflict diagnostics (`"cpu 4 already occupied by
/// lcore-0(rx-worker)"`).
///
/// Scope: 1:1 lcore→cpu mapping. Set-of-sets / coremask / service-core
/// (`-s`) syntaxes are out of scope — fall back to the raw escape hatch via
/// `EalConfig::lcores` / `EalConfig::extra_args` if you need them, with the
/// understanding that the raw path does not participate in registry-aware
/// validation.

#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "eph/dpdk/detail/logger.hpp"
#include "eph/utils/cpu.hpp"  // CpuPinPolicy, register_external_pin, validate_pin_policy

namespace eph::dpdk {

namespace detail {
inline spdlog::logger* lcore_pin_logger() {
    return get_logger<LoggerName{"dpdk.lcore_pin"}>();
}
} // namespace detail

/// @brief Declarative spec for "EAL lcore N runs pinned on physical cpu M".
///
/// The fields are deliberately POD so the caller can construct vectors /
/// arrays at compile time (`constexpr std::array pins = { LcorePin{...},
/// ... };`). `role` is a free-form short label (e.g. `"rx-worker"`,
/// `"tx-shaper"`) used purely for diagnostic messages — keep it under
/// ~20 characters for readable conflict errors.
///
/// Validation (cpu_id >= 0, no duplicate lcore_id, no duplicate cpu_id,
/// SMT/NUMA/IRQ checks) is the job of `pin_lcore` / `pin_lcores`;
/// `build_lcore_argv` itself does *no* semantic checking.
struct LcorePin {
    std::uint16_t lcore_id;   ///< EAL-visible logical core id (0..RTE_MAX_LCORE-1)
    int           cpu_id;     ///< Physical cpu id EAL will bind the lcore to
    std::string   role;       ///< Short diagnostic label; empty allowed
};

/// @brief Serialize a LcorePin span into a single EAL `--lcores=...` argv token.
///
/// Output shape (DPDK-recommended modern syntax):
///
/// @code
///   build_lcore_argv({}) ==                         ""
///   build_lcore_argv({{0,4,"rx"}}) ==              "--lcores=0@4"
///   build_lcore_argv({{0,4,"rx"},{1,5,"tx"}}) ==   "--lcores=0@4,1@5"
/// @endcode
///
/// The result is a single token suitable for appending to the EAL argv
/// vector via `EalConfig::extra_args.push_back(build_lcore_argv(pins))`.
///
/// @note Pure function: no validation, no registry side effects, no logging.
///       `lcore_id` collisions / negative `cpu_id` / 1:N mappings are not
///       detected here — `pin_lcores` is the validation gate.
///
/// @param pins  span of LcorePin entries (any size, including zero)
/// @return single argv token, or empty string if `pins` is empty
[[nodiscard]] inline std::string
build_lcore_argv(std::span<LcorePin const> pins) {
    if (pins.empty()) return {};
    std::string out;
    out.reserve(12 + pins.size() * 8);  // "--lcores=" + ~8 chars per entry
    out.append("--lcores=");
    bool first = true;
    for (auto const& p : pins) {
        if (!first) out.push_back(',');
        first = false;
        out.append(std::to_string(p.lcore_id));
        out.push_back('@');
        out.append(std::to_string(p.cpu_id));
    }
    return out;
}

// ──────────────────────────────────────────────────────────────────────
// pin_lcore + pin_lcores — registry-only counterparts to eph::utils::pin_thread.
//
// Both functions are placeholder-registration only: they validate and
// claim cpu(s) in the process-wide registry BEFORE rte_eal_init. The
// actual setaffinity happens later, inside rte_eal_init, when EAL either
// repurposes the calling thread (main lcore) or pthread_create-s a new
// worker thread (worker lcores). See examples/simple_hft_dpdk_rss.cpp
// for the multi-lcore launch pattern.
//
// Both return eph::utils::PinGuard (the same RAII type pin_thread uses)
// so EalGuard::pin_guards_ can hold them in a vector. The role string
// written to the registry is "lcore-{lcore_id}({label})" — the prefix
// is automatic, so caller-side conflict errors read e.g.
// "pin_thread: cpu 4 already pinned by lcore-0(rx-worker)".
// ──────────────────────────────────────────────────────────────────────

/// @brief Pre-EAL: validate one LcorePin under @p policy and register its
///        cpu into the process-wide pin registry. Single-pin primitive.
///
/// Returns a `PinGuard` on success: destruction unregisters the cpu;
/// `.release()` opts out of RAII (used by `pin_lcores` for transactional
/// rollback handling and by `EalGuard::init_with_pins` to transfer
/// ownership into the guard vector that lives as long as EAL).
///
/// @param lcore_id  EAL-visible lcore id; only affects the registry role
///                  label "lcore-{lcore_id}({label})", not setaffinity.
/// @param cpu       physical cpu id (must be >= 0)
/// @param label     short diagnostic role (e.g. "rx-worker"); empty allowed
/// @param policy    isolcpus / SMT / NUMA / IRQ checks; default relaxed
[[nodiscard]] inline std::expected<eph::utils::PinGuard, std::string>
pin_lcore(std::uint16_t lcore_id, int cpu, std::string_view label,
          eph::utils::CpuPinPolicy policy = {}) {
    [[maybe_unused]] auto* log = detail::lcore_pin_logger();
    if (cpu < 0) {
        SPDLOG_LOGGER_ERROR(log,
            "pin_lcore: lcore={} has invalid cpu_id={} label='{}'",
            lcore_id, cpu, label);
        return std::unexpected(std::format(
            "pin_lcore: lcore={} has invalid cpu_id={}", lcore_id, cpu));
    }
    if (auto v = eph::utils::detail::validate_pin_policy(cpu, policy); !v) {
        SPDLOG_LOGGER_ERROR(log,
            "pin_lcore: policy validation failed lcore={} cpu={} label='{}': {}",
            lcore_id, cpu, label, v.error());
        return std::unexpected(std::format(
            "pin_lcore: lcore={},cpu={}: {}", lcore_id, cpu, v.error()));
    }
    std::string role = std::format("lcore-{}({})", lcore_id, label);
    if (auto r = eph::utils::register_external_pin(cpu, std::move(role)); !r) {
        // Most common failure: cpu already pinned by another lcore /
        // bench thread / external owner. Caller's diagnostic includes
        // the existing role, but the log line gives the operator a
        // stable trace of the registry contention without parsing the
        // unexpected message.
        SPDLOG_LOGGER_ERROR(log,
            "pin_lcore: register_external_pin failed lcore={} cpu={} "
            "label='{}': {}", lcore_id, cpu, label, r.error());
        return std::unexpected(std::format(
            "pin_lcore: lcore={},cpu={}: {}", lcore_id, cpu, r.error()));
    }
    return eph::utils::PinGuard::adopt(cpu);
}

/// @brief Pre-EAL: validate every LcorePin under @p policy and register
///        their cpus. Transactional batch wrapper around `pin_lcore`.
///
/// Transaction semantics:
///   - On full success: returns a `vector<PinGuard>` with one entry per
///     pin; the vector's destruction unregisters every cpu (RAII via
///     each PinGuard) — equivalent to the previous `RegisteredLcoreGuard`
///     batch behaviour.
///   - On any failure: every previously-staged guard is dropped (RAII
///     unregister) before returning `unexpected`. The error message names
///     the offending pin index and inherits the diagnostic from `pin_lcore`.
///
/// Used by `EalGuard::init_with_pins`. Empty span returns an empty vector.
[[nodiscard]] inline std::expected<std::vector<eph::utils::PinGuard>, std::string>
pin_lcores(std::span<LcorePin const> pins,
           eph::utils::CpuPinPolicy policy = {}) {
    [[maybe_unused]] auto* log = detail::lcore_pin_logger();
    std::vector<eph::utils::PinGuard> guards;
    guards.reserve(pins.size());

    for (std::size_t i = 0; i < pins.size(); ++i) {
        auto const& p = pins[i];

        // Reject duplicate lcore_id BEFORE staging any registration:
        // the cpu-collision path (via register_external_pin) catches
        // 1:1 cpu duplicates, but DPDK's --lcores argv requires each
        // lcore_id to appear at most once. Passing 0@4,0@5 would
        // assemble cleanly, the registry would happily accept both
        // (different cpus), and rte_eal_init would later reject the
        // argv with a generic "invalid lcore mask" — the diagnostic
        // would name neither the offending index nor the duplicate
        // id. Fail-fast here with the index pin_lcores already uses
        // for cpu errors so the call site sees a coherent message.
        for (std::size_t j = 0; j < i; ++j) {
            if (pins[j].lcore_id == p.lcore_id) {
                SPDLOG_LOGGER_ERROR(log,
                    "pin_lcores: pin[{}] lcore_id={} duplicates pin[{}] "
                    "(prev cpu={} role='{}', new cpu={} role='{}')",
                    i, p.lcore_id, j, pins[j].cpu_id, pins[j].role,
                    p.cpu_id, p.role);
                return std::unexpected(std::format(
                    "pin_lcores: pin[{}]: lcore_id={} duplicates pin[{}] "
                    "(cpu={}, role='{}'); each EAL lcore must map to one cpu",
                    i, p.lcore_id, j, pins[j].cpu_id, pins[j].role));
            }
        }

        auto g = pin_lcore(p.lcore_id, p.cpu_id, p.role, policy);
        if (!g) {
            // pin_lcore already ERROR-logged with the granular reason; the
            // batch-level WARN here adds the index + total so the operator
            // can map the failure back into the LcorePin span without
            // re-parsing nested error strings. `guards` going out of scope
            // unregisters every staged cpu.
            SPDLOG_LOGGER_WARN(log,
                "pin_lcores: aborting batch at pin[{}/{}]: {}",
                i, pins.size(), g.error());
            return std::unexpected(std::format(
                "pin_lcores: pin[{}]: {}", i, g.error()));
        }
        guards.push_back(std::move(*g));
    }

    SPDLOG_LOGGER_DEBUG(log,
        "pin_lcores: registered {} lcore(s)", guards.size());
    return guards;
}

} // namespace eph::dpdk
