#pragma once

/// @file version.hpp
/// Compile-time version information for the ephemeral library.
///
/// Version components are derived from the project-level xmake.lua
/// set_version() declaration. Keep them in sync when bumping versions.
/// When bumping: update kVersionMajor/Minor/Patch only — strings are
/// generated automatically.

#include <array>
#include <cstddef>
#include <string_view>

namespace eph {

/// @brief Major version component (breaking API changes).
inline constexpr int kVersionMajor = 1;
/// @brief Minor version component (backward-compatible additions).
inline constexpr int kVersionMinor = 0;
/// @brief Patch version component (backward-compatible fixes).
inline constexpr int kVersionPatch = 0;

// Packed version requires minor/patch < 100 to avoid collisions.
static_assert(kVersionMinor < 100 && kVersionPatch < 100,
    "Packed version scheme requires minor and patch < 100");

/// @brief Packed version number for compile-time comparisons.
///
/// Format: (major * 10000) + (minor * 100) + patch.
/// Example: 1.2.3 => 10203.
inline constexpr int kVersion =
    kVersionMajor * 10000 + kVersionMinor * 100 + kVersionPatch;

namespace detail {

/// @brief Write a non-negative int into a char buffer at compile time.
/// @param buf  Output buffer to write decimal digits into.
/// @param value  Non-negative integer to convert.
/// @return Number of characters written.
constexpr std::size_t write_int(char* buf, int value) {
    if (value == 0) {
        buf[0] = '0';
        return 1;
    }
    // Max digits for a 32-bit int is 10.
    char tmp[10]{};
    std::size_t len = 0;
    int v = value;
    while (v > 0) {
        tmp[len++] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    // Reverse into buf.
    for (std::size_t i = 0; i < len; ++i) {
        buf[i] = tmp[len - 1 - i];
    }
    return len;
}

/// @brief Build "M.m.p" version string at compile time.
/// @return Pair of (char array, length) representing the version string.
constexpr auto make_version_string() {
    std::array<char, 32> buf{};
    std::size_t pos = 0;
    pos += write_int(buf.data() + pos, kVersionMajor);
    buf[pos++] = '.';
    pos += write_int(buf.data() + pos, kVersionMinor);
    buf[pos++] = '.';
    pos += write_int(buf.data() + pos, kVersionPatch);
    return std::pair{buf, pos};
}

/// @brief Build "ephemeral/M.m.p" full version string at compile time.
/// @return Pair of (char array, length) representing the full version string.
constexpr auto make_version_full() {
    constexpr std::string_view prefix = "ephemeral/";
    std::array<char, 64> buf{};
    std::size_t pos = 0;
    for (char c : prefix) buf[pos++] = c;
    pos += write_int(buf.data() + pos, kVersionMajor);
    buf[pos++] = '.';
    pos += write_int(buf.data() + pos, kVersionMinor);
    buf[pos++] = '.';
    pos += write_int(buf.data() + pos, kVersionPatch);
    return std::pair{buf, pos};
}

inline constexpr auto kVersionStringData = make_version_string();
inline constexpr auto kVersionFullData   = make_version_full();

} // namespace detail

/// @brief Human-readable version string "M.m.p", generated from kVersionMajor/Minor/Patch.
inline constexpr std::string_view kVersionString{
    detail::kVersionStringData.first.data(),
    detail::kVersionStringData.second};

/// @brief Runtime version string "ephemeral/M.m.p".
///
/// Suitable for User-Agent headers, log preambles, and CLI --version output.
inline constexpr std::string_view kVersionFull{
    detail::kVersionFullData.first.data(),
    detail::kVersionFullData.second};

/// @brief Check at compile time whether the library version is at least the given version.
///
/// Useful for feature-gating in downstream code:
///   static_assert(eph::version_at_least(1, 2, 0), "Requires ephemeral >= 1.2.0");
///
/// @param major  Required major version.
/// @param minor  Required minor version.
/// @param patch  Required patch version.
/// @return true if the library version >= the specified version.
consteval bool version_at_least(int major, int minor, int patch) noexcept {
    return kVersion >= (major * 10000 + minor * 100 + patch);
}

} // namespace eph
