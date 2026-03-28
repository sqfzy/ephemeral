#pragma once

/// @file parser.hpp
/// Zero-copy JSON parser for flat key-value objects (HFT market data).
///
/// Designed for the JSON payloads sent by cryptocurrency exchanges
/// (Binance, OKX, Bybit) — flat objects with string/number/boolean values,
/// no nested objects or arrays. Parsing is O(n) single-pass with zero
/// heap allocation; field lookup is O(n) linear scan, which is faster
/// than hash-based lookup for typical 5–15 field messages due to cache
/// locality.
///
/// Usage:
///   auto result = eph::json::parse(data, len);
///   if (result) {
///       auto price = result->get_string("p");
///       auto qty   = result->get_string("q");
///   }

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include "eph/core/error_traits.hpp"

namespace eph::json {

// ---------------------------------------------------------------------------
// Parse error
// ---------------------------------------------------------------------------

enum class ParseError : uint8_t {
    kIncomplete,      ///< No complete JSON object found (missing closing brace)
    kInvalidFormat,   ///< Malformed JSON (missing quotes, colons, etc.)
    kFieldOverflow,   ///< More fields than kMaxFields capacity
};

constexpr std::string_view parse_error_name(ParseError e) noexcept {
    switch (e) {
    case ParseError::kIncomplete:    return "incomplete";
    case ParseError::kInvalidFormat: return "invalid format";
    case ParseError::kFieldOverflow: return "field overflow";
    }
    return "unknown";
}

constexpr std::string_view error_name(ParseError e) noexcept {
    return parse_error_name(e);
}

// ---------------------------------------------------------------------------
// JSON field and view
// ---------------------------------------------------------------------------

/// Zero-copy JSON field — string_views into the original buffer.
struct Field {
    std::string_view key;    ///< Field name (without quotes)
    std::string_view value;  ///< Raw value (without quotes for strings, raw for numbers/bools)
    bool is_string = false;  ///< True if value was quoted (string type)
};

/// Zero-copy view into a flat JSON object.
///
/// All string_views point into the original parse buffer — the caller
/// must ensure the buffer outlives the JsonView.
class JsonView {
public:
    static constexpr size_t kMaxFields = 32;

    /// Get raw value for a key. Returns empty string_view if not found.
    [[nodiscard]] std::string_view get(std::string_view key) const noexcept {
        auto* f = find_field(key);
        return f ? f->value : std::string_view{};
    }

    /// Get string value (unquoted). Returns nullopt if not found.
    [[nodiscard]] std::optional<std::string_view>
    get_string(std::string_view key) const noexcept {
        auto* f = find_field(key);
        return f ? std::optional{f->value} : std::nullopt;
    }

    /// Parse value as int64_t. Returns nullopt if not found or not a valid integer.
    [[nodiscard]] std::optional<int64_t>
    get_int(std::string_view key) const noexcept {
        auto* f = find_field(key);
        if (!f) return std::nullopt;
        return parse_int(f->value);
    }

    /// Parse value as double. Returns nullopt if not found or not a valid number.
    [[nodiscard]] std::optional<double>
    get_double(std::string_view key) const noexcept {
        auto* f = find_field(key);
        if (!f) return std::nullopt;
        return parse_double(f->value);
    }

    /// Parse value as boolean. Returns nullopt if not found or not true/false.
    [[nodiscard]] std::optional<bool>
    get_bool(std::string_view key) const noexcept {
        auto* f = find_field(key);
        if (!f) return std::nullopt;
        if (f->value == "true") return true;
        if (f->value == "false") return false;
        return std::nullopt;
    }

    /// Number of fields parsed.
    [[nodiscard]] size_t field_count() const noexcept { return count_; }

    /// Check if a key exists.
    [[nodiscard]] bool has(std::string_view key) const noexcept {
        return find_field(key) != nullptr;
    }

    /// Access field by index (for iteration). Returns empty field if out of bounds.
    [[nodiscard]] const Field& field_at(size_t i) const noexcept {
        static constexpr Field kEmpty{};
        return i < count_ ? fields_[i] : kEmpty;
    }

private:
    friend std::expected<JsonView, ParseError>
    parse(const uint8_t* data, size_t len) noexcept;

    /// Single linear scan, used by all accessors. Returns nullptr if not found.
    [[nodiscard]] const Field* find_field(std::string_view key) const noexcept {
        for (size_t i = 0; i < count_; ++i) {
            if (fields_[i].key == key) return &fields_[i];
        }
        return nullptr;
    }

    std::array<Field, kMaxFields> fields_{};
    size_t count_ = 0;

    /// Parse a string_view as int64_t with overflow protection.
    /// Uses uint64_t accumulator to handle INT64_MIN correctly
    /// (its absolute value exceeds INT64_MAX by 1).
    static std::optional<int64_t> parse_int(std::string_view sv) noexcept {
        if (sv.empty()) return std::nullopt;
        bool negative = false;
        size_t pos = 0;
        if (sv[0] == '-') { negative = true; pos = 1; }
        if (pos >= sv.size()) return std::nullopt;

        uint64_t result = 0;
        constexpr uint64_t kMaxPos = static_cast<uint64_t>(INT64_MAX);
        // INT64_MIN has absolute value INT64_MAX + 1
        constexpr uint64_t kMaxNeg = kMaxPos + 1;
        uint64_t limit = negative ? kMaxNeg : kMaxPos;

        for (; pos < sv.size(); ++pos) {
            char c = sv[pos];
            if (c < '0' || c > '9') return std::nullopt;
            uint64_t digit = static_cast<uint64_t>(c - '0');
            if (result > (limit - digit) / 10) return std::nullopt;
            result = result * 10 + digit;
        }

        if (negative) {
            // Negate as unsigned THEN cast — avoids signed overflow UB
            // when result == INT64_MAX + 1 (i.e., parsing INT64_MIN).
            return static_cast<int64_t>(~result + 1u);
        }
        return static_cast<int64_t>(result);
    }

    /// Parse a string_view as double (simple: integer + optional fraction).
    static std::optional<double> parse_double(std::string_view sv) noexcept {
        if (sv.empty()) return std::nullopt;
        bool negative = false;
        size_t pos = 0;
        if (sv[0] == '-') { negative = true; pos = 1; }
        if (pos >= sv.size()) return std::nullopt;

        double result = 0.0;
        // Integer part
        for (; pos < sv.size() && sv[pos] != '.' && sv[pos] != 'e' && sv[pos] != 'E'; ++pos) {
            char c = sv[pos];
            if (c < '0' || c > '9') return std::nullopt;
            result = result * 10.0 + (c - '0');
        }
        // Fractional part
        if (pos < sv.size() && sv[pos] == '.') {
            ++pos;
            double frac = 0.0;
            double divisor = 10.0;
            for (; pos < sv.size() && sv[pos] != 'e' && sv[pos] != 'E'; ++pos) {
                char c = sv[pos];
                if (c < '0' || c > '9') return std::nullopt;
                frac += (c - '0') / divisor;
                divisor *= 10.0;
            }
            result += frac;
        }
        // Exponent part (e.g., 1.5e10)
        if (pos < sv.size() && (sv[pos] == 'e' || sv[pos] == 'E')) {
            ++pos;
            bool exp_neg = false;
            if (pos < sv.size() && sv[pos] == '-') { exp_neg = true; ++pos; }
            else if (pos < sv.size() && sv[pos] == '+') { ++pos; }
            int exp = 0;
            for (; pos < sv.size(); ++pos) {
                char c = sv[pos];
                if (c < '0' || c > '9') return std::nullopt;
                exp = exp * 10 + (c - '0');
            }
            double factor = 1.0;
            for (int i = 0; i < exp; ++i) factor *= 10.0;
            result = exp_neg ? result / factor : result * factor;
        }
        return negative ? -result : result;
    }
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

/// Parse a flat JSON object into a JsonView.
///
/// Single-pass O(n) parser. Handles: strings (with escape sequences),
/// numbers, booleans, null. Does NOT handle nested objects or arrays —
/// these are skipped as opaque values.
///
/// @param data  JSON bytes (must start with '{')
/// @param len   Length of available data
/// @return JsonView on success, ParseError on failure
[[nodiscard]] inline std::expected<JsonView, ParseError>
parse(const uint8_t* data, size_t len) noexcept {
    if (len == 0) return std::unexpected(ParseError::kIncomplete);

    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + len;

    // Skip leading whitespace
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;

    if (p >= end || *p != '{') return std::unexpected(ParseError::kInvalidFormat);
    ++p; // skip '{'

    JsonView view;

    while (p < end) {
        // Skip whitespace
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        if (p >= end) return std::unexpected(ParseError::kIncomplete);

        // End of object
        if (*p == '}') return view;

        // Comma between fields
        if (view.count_ > 0) {
            if (*p != ',') return std::unexpected(ParseError::kInvalidFormat);
            ++p;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
            if (p >= end) return std::unexpected(ParseError::kIncomplete);
        }

        // Field overflow check
        if (view.count_ >= JsonView::kMaxFields) {
            return std::unexpected(ParseError::kFieldOverflow);
        }

        // Parse key (must be a quoted string)
        if (*p != '"') return std::unexpected(ParseError::kInvalidFormat);
        ++p;
        const char* key_start = p;
        while (p < end && *p != '"') {
            if (*p == '\\') { ++p; if (p >= end) return std::unexpected(ParseError::kIncomplete); }
            ++p;
        }
        if (p >= end) return std::unexpected(ParseError::kIncomplete);
        std::string_view key(key_start, static_cast<size_t>(p - key_start));
        ++p; // skip closing quote

        // Skip whitespace + colon
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        if (p >= end || *p != ':') return std::unexpected(ParseError::kInvalidFormat);
        ++p;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        if (p >= end) return std::unexpected(ParseError::kIncomplete);

        // Parse value
        auto& field = view.fields_[view.count_];
        field.key = key;

        if (*p == '"') {
            // String value
            ++p;
            const char* val_start = p;
            while (p < end && *p != '"') {
                if (*p == '\\') { ++p; if (p >= end) return std::unexpected(ParseError::kIncomplete); }
                ++p;
            }
            if (p >= end) return std::unexpected(ParseError::kIncomplete);
            field.value = std::string_view(val_start, static_cast<size_t>(p - val_start));
            field.is_string = true;
            ++p; // skip closing quote
        } else if (*p == '{' || *p == '[') {
            // Nested object/array — skip as opaque value (count braces/brackets)
            char open = *p;
            char close = (open == '{') ? '}' : ']';
            const char* val_start = p;
            int depth = 1;
            ++p;
            while (p < end && depth > 0) {
                if (*p == '"') {
                    // Skip string content (handles escaped quotes)
                    ++p;
                    while (p < end && *p != '"') {
                        if (*p == '\\') ++p;
                        if (p < end) ++p;
                    }
                    if (p < end) ++p; // skip closing quote
                    continue;
                }
                if (*p == open) ++depth;
                else if (*p == close) --depth;
                ++p;
            }
            if (depth != 0) return std::unexpected(ParseError::kIncomplete);
            field.value = std::string_view(val_start, static_cast<size_t>(p - val_start));
            field.is_string = false;
        } else {
            // Number, boolean, or null
            const char* val_start = p;
            while (p < end && *p != ',' && *p != '}' && *p != ' ' &&
                   *p != '\t' && *p != '\n' && *p != '\r') {
                ++p;
            }
            field.value = std::string_view(val_start, static_cast<size_t>(p - val_start));
            field.is_string = false;
        }

        view.count_++;
    }

    return std::unexpected(ParseError::kIncomplete);
}

} // namespace eph::json

// std::formatter for ParseError
template <>
struct std::formatter<eph::json::ParseError>
    : eph::net::ErrorEnumFormatter<eph::json::ParseError> {};
