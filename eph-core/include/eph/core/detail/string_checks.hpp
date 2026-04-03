#pragma once

/// @file detail/string_checks.hpp
/// Constexpr string validation helpers for hostname/path sanitization.
///
/// Extracted from repeated inline checks across TransportConfig, SocketConfig,
/// and ProxyConfig to eliminate duplication and ensure consistent validation
/// rules. All functions are constexpr for use in compile-time config validation.

#include <string_view>

namespace eph::core::detail {

/// @brief Check if a string contains ASCII control characters (U+0000-U+001F, U+007F).
///
/// Used to reject hostnames and paths that could enable HTTP header injection
/// (CWE-93), request smuggling (CWE-113), or log injection. Control characters
/// have no legitimate use in hostnames, URL paths, or protocol field values.
///
/// Chars >= 0x80 are NOT flagged -- they are valid UTF-8 continuation bytes.
/// Cast to unsigned char prevents signed-char platforms from treating 0x80-0xFF
/// as negative (which would falsely match the < 0x20 check).
///
/// @param sv  The string to check.
/// @return true if the string contains at least one control character.
[[nodiscard]] constexpr bool contains_control_chars(std::string_view sv) noexcept {
    for (char c : sv) {
        auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f) return true;
    }
    return false;
}

} // namespace eph::core::detail
