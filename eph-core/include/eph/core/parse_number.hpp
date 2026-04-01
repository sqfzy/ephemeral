#pragma once

/// @file parse_number.hpp
/// Fast decimal-to-double parser for exchange price/quantity fields.
///
/// Zero-allocation, branchless-friendly. Handles:
///   - Optional sign: "-123.45"
///   - Integer + optional fraction: "87245.30000000"
///   - Scientific notation: "1.5e-8" (common in crypto small-cap prices)
///
/// Rejects: NaN, infinity, empty strings, bare dots, bare exponents.
/// Used by eph-json, eph-fix, eph-book adapters — canonical implementation
/// lives here to avoid duplication across modules.

#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>

namespace eph::core {

/// Parse a decimal ASCII string as double.
/// Returns std::nullopt on malformed input, overflow to inf, or empty string.
[[nodiscard]] inline std::optional<double> parse_number(std::string_view sv) noexcept {
    if (sv.empty()) return std::nullopt;

    const char* p   = sv.data();
    const char* end = p + sv.size();

    // Sign
    bool negative = false;
    if (*p == '-') { negative = true; ++p; }
    if (p == end) return std::nullopt;

    // Integer part
    double result = 0.0;
    bool has_int_digit = false;
    while (p != end && *p != '.' && *p != 'e' && *p != 'E') {
        char c = *p++;
        if (c < '0' || c > '9') return std::nullopt;
        result = result * 10.0 + (c - '0');
        has_int_digit = true;
    }

    // Fractional part
    bool has_frac_digit = false;
    if (p != end && *p == '.') {
        ++p;
        double frac = 0.0;
        double divisor = 10.0;
        while (p != end && *p != 'e' && *p != 'E') {
            char c = *p++;
            if (c < '0' || c > '9') return std::nullopt;
            frac += (c - '0') / divisor;
            divisor *= 10.0;
            has_frac_digit = true;
        }
        if (!has_frac_digit) return std::nullopt;  // reject bare "1." or "."
        result += frac;
    }

    if (!has_int_digit && !has_frac_digit) return std::nullopt;

    // Exponent part (e.g., 1.5e10, 3e-8)
    if (p != end && (*p == 'e' || *p == 'E')) {
        ++p;
        bool exp_neg = false;
        if (p != end && *p == '-') { exp_neg = true; ++p; }
        else if (p != end && *p == '+') { ++p; }
        if (p == end || *p < '0' || *p > '9') return std::nullopt;  // bare "1e"

        int exp = 0;
        while (p != end) {
            char c = *p++;
            if (c < '0' || c > '9') return std::nullopt;
            exp = exp * 10 + (c - '0');
            if (exp > 308) return std::nullopt;  // IEEE 754 double max exponent
        }
        double factor = std::pow(10.0, exp);
        result = exp_neg ? result / factor : result * factor;
    }

    // Reject trailing characters (input not fully consumed)
    if (p != end) return std::nullopt;

    double final_val = negative ? -result : result;
    if (!std::isfinite(final_val)) return std::nullopt;
    return final_val;
}

} // namespace eph::core
