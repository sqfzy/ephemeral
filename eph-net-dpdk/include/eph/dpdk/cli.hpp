#pragma once

/// @file cli.hpp
/// CLI helpers for the EAL-flavored command-line conventions used across
/// `examples/*.cpp` and standalone tools that drive `eph-net-dpdk`.
///
/// Scope: parsing, validating, and lowering the small set of EAL-related
/// flags that every DPDK-backed program needs (`--pci`, `--pin`, `--lcores`,
/// `--port-id`/`--dpdk-port`). The helper is **opt-in**: examples are free
/// to keep their own argv loop and call `consume_one` per token, OR keep
/// hand-written parsing entirely. Application-specific flags (`--host`,
/// `--duration`, etc.) are always the caller's job — this header has no
/// opinion about them.
///
/// Why a separate header rather than folding into `eal.hpp`: parsing pulls
/// in `<string_view>` and atoi-style logic that EAL bring-up itself does
/// not need. Keeping it isolated means consumers of `EalConfig` /
/// `Platform::launch` don't pay for the CLI surface unless they
/// also want it.
///
/// Typical use:
///
/// @code
///     eph::dpdk::cli::EalCliConfig eal;
///     for (int i = 1; i < argc; ++i) {
///         auto consumed = eph::dpdk::cli::consume_one(
///             eal, argv[i], (i + 1 < argc) ? argv[i + 1] : nullptr);
///         if (!consumed) { spdlog::error("{}", consumed.error()); return 1; }
///         if (*consumed > 0) { i += *consumed - 1; continue; }
///         // ... app-specific flags here ...
///     }
///     if (auto v = eph::dpdk::cli::validate(eal); !v) {
///         spdlog::error("{}", v.error());
///         return 1;
///     }
///     auto eal_cfg = eph::dpdk::cli::to_eal_config(std::move(eal),
///                                                  "my_program");
/// @endcode

#include <cstdint>
#include <cstdlib>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "eph/dpdk/eal.hpp"          // EalConfig
#include "eph/dpdk/lcore_pin.hpp"    // LcorePin / parse_pin_spec

namespace eph::dpdk::cli {

/// @brief Accumulator for EAL-related CLI flags.
///
/// The fields mirror the EAL-relevant subset of `EalConfig` plus the
/// `port_id` value (which is per-Platform, not per-EAL, but is so
/// universally needed alongside the EAL flags that it earns a slot
/// here). `program_name` is supplied at lowering time, not parse time,
/// so the same `EalCliConfig` could in principle drive two Platforms.
struct EalCliConfig {
    /// One entry per `--pci` token (passed through to `EalConfig::allowed_devs`).
    std::vector<std::string>  pci;
    /// Typed pins from `--pin lcore=cpu[:role]`. Mutually exclusive with
    /// `lcores_raw` — see `validate`.
    std::vector<LcorePin>     pins;
    /// Raw `--lcores '<spec>'` token (e.g. `(0-3)@(4-7)`). Mutually
    /// exclusive with `pins`.
    std::string               lcores_raw;
    /// `--port-id` / `--dpdk-port` (aliases). 0 is also the EalConfig
    /// default, so callers should consult `port_id_set` to distinguish
    /// "explicitly chose 0" from "left at default".
    std::uint16_t             port_id     = 0;
    bool                      port_id_set = false;
};

/// @brief Try to consume one EAL-related CLI token.
///
/// @param args  accumulator to write into
/// @param flag  current argv token (e.g. `"--pin"`)
/// @param next  argv[i+1] or nullptr if argv ended at `flag`
/// @return number of argv tokens this call consumed:
///           - `0` if `flag` is not an EAL flag (caller handles it)
///           - `2` if `flag` is a value-bearing EAL flag (consumed `flag` + `next`)
///         Or `std::unexpected<std::string>` if `flag` IS an EAL flag but
///         `next` is missing or unparseable.
///
/// All current EAL flags are value-bearing (consume 2 tokens). The
/// `int` return type leaves room for future boolean flags without
/// changing the signature. Callers should advance their loop index by
/// `*consumed - 1` after a successful match (or 0 if no match).
[[nodiscard]] inline std::expected<int, std::string>
consume_one(EalCliConfig& args, std::string_view flag, char const* next) {
    auto need_value = [&](std::string_view f) -> std::expected<char const*, std::string> {
        if (next == nullptr) {
            return std::unexpected(std::string{f} + " requires a value");
        }
        return next;
    };

    if (flag == "--pci") {
        auto v = need_value(flag);
        if (!v) return std::unexpected(v.error());
        args.pci.emplace_back(*v);
        return 2;
    }
    if (flag == "--pin") {
        auto v = need_value(flag);
        if (!v) return std::unexpected(v.error());
        auto p = parse_pin_spec(*v);
        if (!p) return std::unexpected(p.error());
        args.pins.push_back(std::move(*p));
        return 2;
    }
    if (flag == "--lcores") {
        auto v = need_value(flag);
        if (!v) return std::unexpected(v.error());
        args.lcores_raw = *v;
        return 2;
    }
    if (flag == "--port-id" || flag == "--dpdk-port") {
        auto v = need_value(flag);
        if (!v) return std::unexpected(v.error());
        char* end = nullptr;
        long n = std::strtol(*v, &end, 10);
        if (end == *v || *end != '\0' || n < 0 || n > UINT16_MAX) {
            return std::unexpected(std::string{flag} + ": expected uint16, got '"
                                   + std::string{*v} + "'");
        }
        args.port_id     = static_cast<std::uint16_t>(n);
        args.port_id_set = true;
        return 2;
    }
    return 0;  // not an EAL flag — caller handles it
}

/// @brief Reject the typed-vs-raw lcore spec collision.
///
/// `--pin` (typed, registry-aware) and `--lcores` (raw EAL spec) drive
/// disjoint code paths inside `EalGuard::init_with_pins` /
/// `Platform::launch`. Allowing both at once would silently
/// drop one. Surface the conflict at parse time with the same diagnostic
/// every example used to print by hand.
[[nodiscard]] inline std::expected<void, std::string>
validate(EalCliConfig const& a) {
    const bool typed = !a.pins.empty();
    const bool raw   = !a.lcores_raw.empty();
    if (typed && raw) {
        return std::unexpected(std::string{
            "--pin and --lcores are mutually exclusive (typed vs raw EAL "
            "paths); pick one"});
    }
    return {};
}

/// @brief Lower an `EalCliConfig` into an `EalConfig` ready for
/// `Platform::launch` / `EalGuard::init_with_pins`.
///
/// `pci` -> `allowed_devs` (`-a` passthrough). `lcores_raw` -> single
/// `lcores` entry (only when set; the typed-pin path leaves `lcores`
/// empty so `init_with_pins` can emit `--lcores=...` itself).
///
/// `pins` is **not** lowered: the typed pin span is passed alongside
/// `EalConfig` into `Platform::launch(... , pins, policy)`.
/// Pass `args.pins` directly there.
///
/// `port_id` is **not** lowered into `EalConfig` either — it lives on
/// `PlatformConfig`, not the EAL session. Read `args.port_id` directly
/// when filling `PlatformConfig::port_id`.
[[nodiscard]] inline EalConfig
to_eal_config(EalCliConfig args, std::string program_name) {
    EalConfig cfg{};
    cfg.program_name = std::move(program_name);
    cfg.allowed_devs = std::move(args.pci);
    if (!args.lcores_raw.empty()) {
        cfg.lcores = {std::move(args.lcores_raw)};
    }
    return cfg;
}

} // namespace eph::dpdk::cli
