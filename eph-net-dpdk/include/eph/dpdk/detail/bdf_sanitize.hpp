#pragma once

/// @file detail/bdf_sanitize.hpp
/// PCI BDF → MP file_prefix derivation helper.
///
/// The autojoin path (`Platform::join_dynamic`) auto-derives a DPDK
/// `--file-prefix` from the user-supplied PCI BDF so two processes
/// sharing one NIC naturally agree on the prefix without any explicit
/// coordination string. The derivation is `"eph_" + sanitize(bdf)`
/// where sanitize replaces the PCI separator characters (`:` `.`)
/// with `_` so the output is a valid filename component DPDK will
/// accept.
///
/// Examples:
///   "0000:28:00.0"  -> "0000_28_00_0"   (full domain form)
///   "28:00.0"       -> "28_00_0"        (short form)
///
/// Validation rules (precise — the autojoin path won't paper over a typo):
///   - non-empty
///   - length 7-12 (covers short `BB:DD.F` and full `DDDD:BB:DD.F`)
///   - characters are hex digits, `:`, or `.` (no whitespace, no
///     glob metas, no NUL byte)
///   - contains at least one `:` AND at least one `.` (PCI BDF
///     always has both)
///
/// Hot path: NONE. This is invoked exactly once per process during
/// `Platform::join_dynamic`'s cold setup.

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include "eph/core/error.hpp"

namespace eph::dpdk::detail {

inline constexpr size_t kBdfMinLen = 7;   // "BB:DD.F"
inline constexpr size_t kBdfMaxLen = 12;  // "DDDD:BB:DD.F"

/// @brief Validate a PCI BDF and return a `_`-substituted form
/// suitable for use as a DPDK file_prefix component.
///
/// Returns `Error::InvalidConfig` on any rule violation; the
/// `ErrorInfo::detail` string names the specific rule so a misuse
/// surfaces as a clear configuration error rather than a downstream
/// memzone-naming failure.
[[nodiscard]] inline std::expected<std::string, core::ErrorInfo>
sanitize_bdf_for_file_prefix(std::string_view bdf) noexcept {
    // Cold-path validation: each rule logs the offending input alongside the
    // typed error so a misconfigured `Platform::join_dynamic` surfaces in the
    // logs even if the caller wraps san.error().detail and never logs the
    // string form (see platform.hpp line ~2421 — autojoin forwards the detail
    // back as a `std::string` without a SPDLOG_ call on the failure path).
    if (bdf.empty()) {
        spdlog::error("sanitize_bdf: BDF must be non-empty");
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "sanitize_bdf: BDF must be non-empty"});
    }

    if (bdf.size() < kBdfMinLen || bdf.size() > kBdfMaxLen) {
        spdlog::error(
            "sanitize_bdf: BDF length {} out of range [7, 12] (bdf='{}')",
            bdf.size(), bdf);
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "sanitize_bdf: BDF length must be in [7, 12] "
            "(e.g. '0000:28:00.0' or '28:00.0')"});
    }

    bool has_colon = false;
    bool has_dot   = false;
    for (char c : bdf) {
        const bool is_hex = (c >= '0' && c <= '9') ||
                            (c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F');
        if (c == ':') { has_colon = true; continue; }
        if (c == '.') { has_dot   = true; continue; }
        if (!is_hex) {
            spdlog::error(
                "sanitize_bdf: BDF contains non-hex/':'/'.' char "
                "(byte={:#x}, bdf='{}')",
                static_cast<unsigned>(static_cast<unsigned char>(c)), bdf);
            return std::unexpected(core::ErrorInfo{
                core::Error::InvalidConfig,
                "sanitize_bdf: BDF contains a character that is not a "
                "hex digit, ':' or '.'"});
        }
    }
    if (!has_colon || !has_dot) {
        spdlog::error(
            "sanitize_bdf: BDF missing required separator "
            "(has_colon={} has_dot={} bdf='{}')",
            has_colon, has_dot, bdf);
        return std::unexpected(core::ErrorInfo{
            core::Error::InvalidConfig,
            "sanitize_bdf: BDF must contain both ':' and '.' "
            "(canonical PCI form requires the function separator)"});
    }

    std::string out;
    out.reserve(bdf.size());
    for (char c : bdf) {
        out.push_back((c == ':' || c == '.') ? '_' : c);
    }
    return out;
}

} // namespace eph::dpdk::detail
