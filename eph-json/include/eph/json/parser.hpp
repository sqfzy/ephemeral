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
#include "eph/core/parse_number.hpp"

namespace eph::json {

// ---------------------------------------------------------------------------
// Internal: whitespace lookup table (space, tab, newline, carriage return)
// ---------------------------------------------------------------------------
namespace detail {

/// @brief Build a 256-byte whitespace lookup table at compile time.
///
/// ws_lut[c] is true for ASCII whitespace chars (space, tab, newline, CR).
/// Replaces repeated (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
/// chains with a single indexed load -- one branch instead of four.
///
/// @return constexpr std::array<bool, 256> whitespace lookup table
inline constexpr auto make_ws_lut() noexcept {
    std::array<bool, 256> lut{};
    lut[' ']  = true;
    lut['\t'] = true;
    lut['\n'] = true;
    lut['\r'] = true;
    return lut;
}
/// @brief Compile-time whitespace lookup table instance.
inline constexpr auto kWsLut = make_ws_lut();

/// @brief Skip whitespace bytes using the LUT.
/// @param p    Current read position
/// @param end  One-past-end of the buffer
/// @return Pointer to the first non-whitespace byte, or @p end if none found
inline const char* skip_ws(const char* p, const char* end) noexcept {
    while (p < end && kWsLut[static_cast<unsigned char>(*p)]) ++p;
    return p;
}

/// @brief Scan forward to find the closing double-quote, handling escapes.
///
/// Byte-at-a-time is optimal for the short strings (1-10 chars) typical
/// in exchange JSON -- memchr's call overhead exceeds its SIMD benefit here.
///
/// @param p    Pointer to the first character after the opening '"'
/// @param end  One-past-end of the buffer
/// @return Pointer TO the closing '"', or @p end if not found
///
/// @note Does not validate escape sequences (e.g., \x is accepted).
///       This is a deliberate zero-copy trade-off: field.value contains
///       raw text including backslash sequences. Callers must re-validate
///       if strict RFC 8259 compliance is required.
inline const char* scan_string(const char* p, const char* end) noexcept {
    while (p < end) {
        if (*p == '"') return p;
        if (*p == '\\') [[unlikely]] {
            ++p; // skip escaped char
            if (p >= end) [[unlikely]] return end;
        }
        ++p;
    }
    return end;
}

/// @brief Build a 256-byte value-terminator lookup table at compile time.
///
/// Marks comma, closing brace, and whitespace as terminators. Used to
/// scan unquoted values (numbers, booleans, null) without multiple
/// branch conditions per byte.
///
/// @return constexpr std::array<bool, 256> value-terminator lookup table
inline constexpr auto make_val_term_lut() noexcept {
    std::array<bool, 256> lut{};
    lut[',']  = true;
    lut['}']  = true;
    lut[' ']  = true;
    lut['\t'] = true;
    lut['\n'] = true;
    lut['\r'] = true;
    return lut;
}
/// @brief Compile-time value-terminator lookup table instance.
inline constexpr auto kValTermLut = make_val_term_lut();

} // namespace detail

// ---------------------------------------------------------------------------
// Parse error
// ---------------------------------------------------------------------------

/// @brief Error codes returned by the JSON parser.
enum class ParseError : uint8_t {
    kIncomplete,      ///< No complete JSON object found (missing closing brace)
    kInvalidFormat,   ///< Malformed JSON (missing quotes, colons, etc.)
    kFieldOverflow,   ///< More fields than kMaxFields capacity
};

/// @brief Convert a ParseError to a human-readable string.
/// @param e  The parse error code
/// @return A string_view naming the error (e.g., "incomplete", "invalid format")
constexpr std::string_view parse_error_name(ParseError e) noexcept {
    switch (e) {
    case ParseError::kIncomplete:    return "incomplete";
    case ParseError::kInvalidFormat: return "invalid format";
    case ParseError::kFieldOverflow: return "field overflow";
    }
    return "unknown";
}

/// @brief ADL-discoverable alias for parse_error_name, used by ErrorEnumFormatter.
/// @param e  The parse error code
/// @return A string_view naming the error
constexpr std::string_view error_name(ParseError e) noexcept {
    return parse_error_name(e);
}

// ---------------------------------------------------------------------------
// JSON field and view
// ---------------------------------------------------------------------------

/// @brief Zero-copy JSON field -- string_views point into the original buffer.
///
/// Represents a single key-value pair extracted during parsing. Both @c key
/// and @c value are non-owning views, so the source buffer must outlive the Field.
struct Field {
    std::string_view key;    ///< Field name (without surrounding quotes)
    std::string_view value;  ///< Raw value text (without quotes for strings, raw text for numbers/bools)
    bool is_string = false;  ///< True if the value was a quoted string in the JSON source
};

/// @brief Zero-copy view into a flat JSON object.
///
/// All string_views point into the original parse buffer -- the caller
/// must ensure the buffer outlives the JsonView. Field lookup is O(n)
/// linear scan, which outperforms hash maps for the 5-15 field messages
/// typical in exchange data due to superior cache locality.
///
/// @warning Not thread-safe. A JsonView should be used from a single thread.
class JsonView {
public:
    /// @brief Maximum number of fields the parser can store per object.
    static constexpr size_t kMaxFields = 32;

    /// @brief Get the raw value for a key.
    /// @param key  Field name to look up
    /// @return The raw value text, or an empty string_view if not found
    /// @note Returns empty string_view for BOTH missing keys and keys with
    ///       empty string values. Use get_string() to distinguish these cases.
    [[nodiscard]] std::string_view get(std::string_view key) const noexcept {
        auto* f = find_field(key);
        return f ? f->value : std::string_view{};
    }

    /// @brief Get a string value (unquoted), distinguishing missing from empty.
    /// @param key  Field name to look up
    /// @return The unquoted string value, or nullopt if the key is absent
    [[nodiscard]] std::optional<std::string_view>
    get_string(std::string_view key) const noexcept {
        auto* f = find_field(key);
        return f ? std::optional{f->value} : std::nullopt;
    }

    /// @brief Parse a field value as int64_t with overflow protection.
    /// @param key  Field name to look up
    /// @return Parsed integer, or nullopt if the key is absent or not a valid integer
    [[nodiscard]] std::optional<int64_t>
    get_int(std::string_view key) const noexcept {
        auto* f = find_field(key);
        if (!f) return std::nullopt;
        return parse_int(f->value);
    }

    /// @brief Parse a field value as double (integer + optional fraction + exponent).
    /// @param key  Field name to look up
    /// @return Parsed double, or nullopt if the key is absent or not a valid number
    [[nodiscard]] std::optional<double>
    get_double(std::string_view key) const noexcept {
        auto* f = find_field(key);
        if (!f) return std::nullopt;
        return parse_double(f->value);
    }

    /// @brief Parse a field value as boolean ("true"/"false" literals only).
    /// @param key  Field name to look up
    /// @return Parsed bool, or nullopt if the key is absent or not "true"/"false"
    [[nodiscard]] std::optional<bool>
    get_bool(std::string_view key) const noexcept {
        auto* f = find_field(key);
        if (!f) return std::nullopt;
        if (f->value == "true") return true;
        if (f->value == "false") return false;
        return std::nullopt;
    }

    /// @brief Return the number of fields successfully parsed.
    /// @return Field count (0 to kMaxFields)
    [[nodiscard]] size_t field_count() const noexcept { return count_; }

    /// @brief Check whether a key exists in the parsed object.
    /// @param key  Field name to look up
    /// @return true if the key is present
    [[nodiscard]] bool has(std::string_view key) const noexcept {
        return find_field(key) != nullptr;
    }

    /// @brief Access a field by positional index (for iteration).
    /// @param i  Zero-based field index
    /// @return Reference to the Field, or a static empty Field if @p i is out of bounds
    [[nodiscard]] const Field& field_at(size_t i) const noexcept {
        static constexpr Field kEmpty{};
        return i < count_ ? fields_[i] : kEmpty;
    }

private:
    friend std::expected<JsonView, ParseError>
    parse(const uint8_t* data, size_t len) noexcept;

    /// @brief Linear scan with first-char + length pre-filter.
    ///
    /// Most keys in exchange messages are 1-2 chars, so checking the first
    /// char and length before the full comparison eliminates most mismatches
    /// with a single comparison (packed into one branch).
    ///
    /// @param key  Field name to search for
    /// @return Pointer to the matching Field, or nullptr if not found
    [[nodiscard]] const Field* find_field(std::string_view key) const noexcept {
        if (key.empty()) return nullptr;
        const auto len = key.size();
        const auto c0  = key[0];
        for (size_t i = 0; i < count_; ++i) {
            const auto& k = fields_[i].key;
            if (k.size() == len && k[0] == c0 &&
                (len <= 1 || k == key))
                return &fields_[i];
        }
        return nullptr;
    }

    std::array<Field, kMaxFields> fields_{};
    size_t count_ = 0;

    /// @brief Parse a string_view as int64_t with overflow protection.
    /// @param sv  String representation of an integer
    /// @return Parsed value, or nullopt on overflow or invalid input
    static std::optional<int64_t> parse_int(std::string_view sv) noexcept {
        return eph::core::parse_int(sv);
    }

    /// @brief Parse a string_view as double (integer + optional fraction + exponent).
    /// @param sv  String representation of a floating-point number
    /// @return Parsed value, or nullopt on invalid input
    static std::optional<double> parse_double(std::string_view sv) noexcept {
        return eph::core::parse_number(sv);
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
    if (len == 0) [[unlikely]] return std::unexpected(ParseError::kIncomplete);

    const char* p = reinterpret_cast<const char*>(data);
    const char* end = p + len;

    // Skip leading whitespace (LUT-based)
    p = detail::skip_ws(p, end);

    if (p >= end || *p != '{') [[unlikely]]
        return std::unexpected(ParseError::kInvalidFormat);
    ++p; // skip '{'

    JsonView view;

    while (p < end) [[likely]] {
        // Skip whitespace
        p = detail::skip_ws(p, end);
        if (p >= end) [[unlikely]] return std::unexpected(ParseError::kIncomplete);

        // End of object
        if (*p == '}') [[unlikely]] return view;

        // Comma between fields
        if (view.count_ > 0) [[likely]] {
            if (*p != ',') [[unlikely]]
                return std::unexpected(ParseError::kInvalidFormat);
            ++p;
            p = detail::skip_ws(p, end);
            if (p >= end) [[unlikely]]
                return std::unexpected(ParseError::kIncomplete);
        }

        // Field overflow check
        if (view.count_ >= JsonView::kMaxFields) [[unlikely]] {
            return std::unexpected(ParseError::kFieldOverflow);
        }

        // Parse key (must be a quoted string)
        if (*p != '"') [[unlikely]]
            return std::unexpected(ParseError::kInvalidFormat);
        ++p;
        const char* key_start = p;
        p = detail::scan_string(p, end);
        if (p >= end) [[unlikely]] return std::unexpected(ParseError::kIncomplete);
        std::string_view key(key_start, static_cast<size_t>(p - key_start));
        ++p; // skip closing quote

        // Skip whitespace + colon. In minified JSON (the common case for
        // exchange data), the colon immediately follows the key — skip_ws
        // returns instantly when the first char is ':'.
        p = detail::skip_ws(p, end);
        if (p >= end || *p != ':') [[unlikely]]
            return std::unexpected(ParseError::kInvalidFormat);
        ++p;
        p = detail::skip_ws(p, end);
        if (p >= end) [[unlikely]] return std::unexpected(ParseError::kIncomplete);

        // Parse value
        auto& field = view.fields_[view.count_];
        field.key = key;

        if (*p == '"') [[likely]] {
            // String value — most common in Binance/OKX data.
            ++p;
            const char* val_start = p;
            p = detail::scan_string(p, end);
            if (p >= end) [[unlikely]]
                return std::unexpected(ParseError::kIncomplete);
            field.value = std::string_view(val_start, static_cast<size_t>(p - val_start));
            field.is_string = true;
            ++p; // skip closing quote
        } else if (*p == '{' || *p == '[') [[unlikely]] {
            // Nested object/array — skip as opaque value (count braces/brackets)
            char open = *p;
            char close = (open == '{') ? '}' : ']';
            const char* val_start = p;
            int depth = 1;
            constexpr int kMaxNestingDepth = 64;
            ++p;
            while (p < end && depth > 0) {
                if (depth > kMaxNestingDepth) [[unlikely]]
                    return std::unexpected(ParseError::kInvalidFormat);
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
            if (depth != 0) [[unlikely]]
                return std::unexpected(ParseError::kIncomplete);
            field.value = std::string_view(val_start, static_cast<size_t>(p - val_start));
            field.is_string = false;
        } else {
            // Number, boolean, or null — scan until delimiter (single LUT check)
            const char* val_start = p;
            while (p < end && !detail::kValTermLut[static_cast<unsigned char>(*p)]) {
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

/// @brief std::formatter specialization for ParseError, enabling std::format("{}", err).
template <>
struct std::formatter<eph::json::ParseError>
    : eph::core::ErrorEnumFormatter<eph::json::ParseError> {};
