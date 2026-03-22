#pragma once

/// @file version.hpp
/// Compile-time version information for the ephemeral library.
///
/// Version components are derived from the project-level xmake.lua
/// set_version() declaration. Keep them in sync when bumping versions.
/// When bumping: update kVersionMajor/Minor/Patch AND kVersionString.

#include <string_view>

namespace eph {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

// Packed version requires minor/patch < 100 to avoid collisions.
static_assert(kVersionMinor < 100 && kVersionPatch < 100,
    "Packed version scheme requires minor and patch < 100");

/// Packed version number for compile-time comparisons.
/// Format: (major * 10000) + (minor * 100) + patch
/// Example: 1.2.3 => 10203
inline constexpr int kVersion =
    kVersionMajor * 10000 + kVersionMinor * 100 + kVersionPatch;

/// Human-readable version string — MUST match kVersionMajor.kVersionMinor.kVersionPatch.
inline constexpr std::string_view kVersionString = "1.0.0";

} // namespace eph
