#pragma once

/// @file detail/json_escape.hpp
/// JSON string escaping (RFC 8259 §7) — shared utility for all eph headers.
///
/// Extracted to eph-core so that transport_errors.hpp and downstream modules
/// can use JSON escaping without depending on eph-net's TLS/WS headers.

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace eph::core::detail {

/// Escape a string for safe embedding in JSON values (RFC 8259 §7).
/// Handles: \", \\, \b, \f, \n, \r, \t, and control chars U+0000–U+001F.
/// Handles valid UTF-8 multibyte sequences (passed through unchanged).
/// Invalid UTF-8 bytes (0x80–0xFF that are not part of a valid multi-byte
/// sequence) are escaped as \\uXXXX to prevent JSON injection.
/// Returns the input unmodified when no escaping is needed (common fast path).
[[nodiscard]] inline std::string json_escape(std::string_view sv) {
    // Fast path: scan for characters that need escaping
    bool needs_escape = false;
    for (char c : sv) {
        auto uc = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\' || uc < 0x20 || uc == 0x7f) {
            needs_escape = true;
            break;
        }
    }
    if (!needs_escape) return std::string(sv);

    std::string out;
    out.reserve(sv.size() + sv.size() / 4 + 4);
    for (std::size_t i = 0; i < sv.size(); ++i) {
        auto uc = static_cast<unsigned char>(sv[i]);
        switch (sv[i]) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (uc < 0x20 || uc == 0x7f) {
                // Control characters including DEL: \u00XX
                out += std::format("\\u{:04x}", uc);
            } else if (uc >= 0x80) {
                // Valid UTF-8 lead byte: pass through the full sequence
                if ((uc & 0xE0) == 0xC0 && i + 1 < sv.size()) {
                    out += sv[i]; out += sv[++i];
                } else if ((uc & 0xF0) == 0xE0 && i + 2 < sv.size()) {
                    out += sv[i]; out += sv[++i]; out += sv[++i];
                } else if ((uc & 0xF8) == 0xF0 && i + 3 < sv.size()) {
                    out += sv[i]; out += sv[++i]; out += sv[++i]; out += sv[++i];
                } else {
                    // Invalid/orphan continuation byte — escape it
                    out += std::format("\\u{:04x}", uc);
                }
            } else {
                out += sv[i];
            }
            break;
        }
    }
    return out;
}

} // namespace eph::core::detail
