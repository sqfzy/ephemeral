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
#include <span>
#include <string>

#include "eph/dpdk/detail/logger.hpp"

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
/// SMT/NUMA/IRQ checks) is the job of `register_lcore_pins` (stage 5);
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
///       detected here — `register_lcore_pins` is the validation gate.
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

} // namespace eph::dpdk
