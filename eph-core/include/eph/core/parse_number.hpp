#pragma once

/// @file parse_number.hpp
/// Fast decimal string-to-number parsers for exchange price/quantity fields.
///
/// Zero-allocation. Handles:
///   parse_number: double — sign + integer + fraction + scientific notation
///   parse_int:    int64_t — sign + integer, overflow-safe for full int64 range
///
/// Rejects: NaN, infinity, empty strings, bare dots, bare exponents, overflow.
/// Used by eph-json, eph-fix, eph-book adapters — canonical implementations
/// live here to avoid duplication across modules.

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace eph::core {

/// @brief Parse a decimal ASCII string as double.
///
/// Supports sign, integer part, fractional part, and scientific notation
/// (e.g., "-1.5e10", "3e-8", "0.001"). Zero-allocation; operates entirely
/// on the input view.
///
/// @param sv  The ASCII string to parse. Must be a complete decimal number
///            with no leading/trailing whitespace.
/// @return The parsed double value, or std::nullopt on malformed input,
///         overflow to infinity, or empty string.
///
/// @note Rejects: NaN, infinity, empty strings, bare dots ("1.", "."),
///       bare exponents ("1e"), and overflow beyond IEEE 754 double range.
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

/// @brief Parse a decimal ASCII string as int64_t.
///
/// Handles optional leading '-' sign. Overflow-safe for the full int64 range
/// including INT64_MIN (-9223372036854775808). Zero-allocation.
///
/// @param sv  The ASCII string to parse. Must contain only an optional '-'
///            followed by one or more decimal digits, with no whitespace.
/// @return The parsed int64_t value, or std::nullopt on malformed input
///         or overflow.
///
/// @note Uses unsigned arithmetic internally to avoid signed overflow UB
///       when parsing INT64_MIN.
[[nodiscard]] inline std::optional<int64_t> parse_int(std::string_view sv) noexcept {
    if (sv.empty()) return std::nullopt;

    const char* p   = sv.data();
    const char* end = p + sv.size();

    bool negative = false;
    if (*p == '-') { negative = true; ++p; }
    if (p == end) return std::nullopt;

    constexpr uint64_t kMaxPos = static_cast<uint64_t>(INT64_MAX);
    constexpr uint64_t kMaxNeg = kMaxPos + 1;  // |INT64_MIN| == INT64_MAX + 1
    const uint64_t limit = negative ? kMaxNeg : kMaxPos;

    uint64_t result = 0;
    while (p != end) {
        char c = *p++;
        if (c < '0' || c > '9') return std::nullopt;
        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (result > (limit - digit) / 10) return std::nullopt;
        result = result * 10 + digit;
    }

    if (negative) {
        // Negate as unsigned then cast — avoids signed overflow UB
        // when result == INT64_MAX + 1 (i.e., parsing INT64_MIN).
        return static_cast<int64_t>(~result + 1u);
    }
    return static_cast<int64_t>(result);
}

} // namespace eph::core
