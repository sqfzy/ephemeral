#pragma once

/// @file errors.hpp
/// Parse-error vocabulary shared across the eph-sbe decode path.
///
/// Defined in its own header so both the schema-independent header/parser layer
/// and the schema-specific message accessors (e.g. binance/book_ticker.hpp) can
/// reference a single error type without a circular include.

#include <cstdint>
#include <string_view>

namespace eph::sbe {

/// @brief Error codes returned by the SBE message parser and group iterators.
enum class ParseError : uint8_t {
    kIncomplete,     ///< Fewer bytes than the 8-byte SBE message header require.
    kTruncated,      ///< Declared block / group / var-data extends past the buffer.
    kMalformedGroup, ///< Repeating-group dimensions are internally inconsistent.
};

/// @brief Human-readable name for a ParseError value.
/// @param e The parse error code.
/// @return String view describing the error (e.g. "incomplete", "truncated").
constexpr std::string_view parse_error_name(ParseError e) noexcept {
    switch (e) {
    case ParseError::kIncomplete:     return "incomplete";
    case ParseError::kTruncated:      return "truncated";
    case ParseError::kMalformedGroup: return "malformed group";
    }
    return "unknown";
}

} // namespace eph::sbe
