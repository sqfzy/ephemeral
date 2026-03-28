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
/// Assumes valid UTF-8 input — multibyte sequences are passed through unchanged.
/// Returns the input unmodified when no escaping is needed (common fast path).
[[nodiscard]] inline std::string json_escape(std::string_view sv) {
    // Fast path: scan for characters that need escaping
    bool needs_escape = false;
    for (char c : sv) {
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) {
            needs_escape = true;
            break;
        }
    }
    if (!needs_escape) return std::string(sv);

    std::string out;
    out.reserve(sv.size() + sv.size() / 4 + 4);
    for (char c : sv) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                // Control characters: \u00XX
                out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

} // namespace eph::core::detail
