#pragma once

/// @file parser.hpp
/// Zero-copy FIX message parser.
///
/// Parses FIX tag=value\x01 (SOH-delimited) messages into a stack-allocated
/// array of Field views. All string_view values point into the original buffer
/// -- no allocations, no copies.

#include <concepts>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/core/parse_number.hpp"
#include "eph/fix/tags.hpp"

namespace eph::fix {

/// @brief Default maximum allowed BodyLength (tag 9) value for FIX message parsing.
/// Protects against malformed messages with absurdly large BodyLength values
/// that would otherwise cause the parser/framer to wait for gigabytes of data.
/// 1 MB is generous for any real FIX message (typical max is ~10 KB).
/// Override via the MaxBodyLength template parameter on parse() or FixFramer.
inline constexpr size_t kDefaultMaxBodyLength = 1 * 1024 * 1024;

/// @brief Backward-compatible alias for kDefaultMaxBodyLength.
inline constexpr size_t kMaxBodyLength = kDefaultMaxBodyLength;

/// @brief Internal implementation details for the parser module.
namespace detail {
/// @brief Get or create the spdlog logger for the parser module.
/// @return Shared pointer to the "fix.parser" logger.
inline const std::shared_ptr<spdlog::logger>& fix_parser_logger() {
    static auto l = [] {
        auto lg = spdlog::get("fix.parser");
        if (!lg) lg = spdlog::stdout_color_mt("fix.parser");
        return lg;
    }();
    return l;
}

// ---------------------------------------------------------------------------
// JSON escaping helper for to_json() output
// ---------------------------------------------------------------------------

/// @brief Append a JSON-escaped string (RFC 8259 S7) to an existing buffer.
/// Handles: \", \\, and control chars U+0000–U+001F.
/// FIX field values are typically printable ASCII, so the fast path
/// (no escaping needed) is the common case.
inline void json_escape_append(std::string& out, std::string_view sv) {
    for (char c : sv) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            } else {
                out += c;
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Value-parsing helpers — shared by BasicMessageView and GroupEntry
// ---------------------------------------------------------------------------

/// @brief Parse a single-character value.
/// @param sv  The string_view to parse.
/// @return The character, or nullopt if not exactly 1 char.
[[nodiscard]] inline std::optional<char> parse_char_value(std::string_view sv) noexcept {
    if (sv.size() != 1) return std::nullopt;
    return sv[0];
}

/// @brief Parse a signed integer from decimal ASCII with overflow detection.
/// @param sv  The string_view to parse.
/// @return The parsed int64_t value, or nullopt on invalid input or overflow.
[[nodiscard]] inline std::optional<int64_t> parse_int_value(std::string_view sv) noexcept {
    return eph::core::parse_int(sv);
}

/// @brief Parse a double from decimal ASCII (integer + optional fractional part).
/// @param sv  The string_view to parse.
/// @return The parsed double value, or nullopt on invalid input or overflow to infinity.
[[nodiscard]] inline std::optional<double> parse_double_value(std::string_view sv) noexcept {
    return eph::core::parse_number(sv);
}

/// @brief Parse a FIX boolean (Y/N).
/// @param sv  The string_view to parse (must be exactly "Y" or "N").
/// @return true for "Y", false for "N", or nullopt for any other input.
[[nodiscard]] inline std::optional<bool> parse_bool_value(std::string_view sv) noexcept {
    auto c = parse_char_value(sv);
    if (!c) return std::nullopt;
    if (*c == 'Y') return true;
    if (*c == 'N') return false;
    return std::nullopt;
}

/// @brief Parse a FIX UTCTimestamp to nanoseconds since Unix epoch.
/// @param sv  The timestamp string in FIX format: "YYYYMMDD-HH:MM:SS[.fff[fff[fff]]]".
/// @return Nanoseconds since Unix epoch, or nullopt on parse failure.
/// @note Supports second (17 chars), millisecond (21 chars), microsecond (24 chars),
///       and nanosecond (27 chars) precision. Leap seconds are not supported.
[[nodiscard]] inline std::optional<uint64_t> parse_timestamp_value(std::string_view sv) noexcept {
    if (sv.size() != 17 && sv.size() != 21 &&
        sv.size() != 24 && sv.size() != 27) return std::nullopt;

    const char* p = sv.data();
    auto digit = [](char c) -> int { return (c >= '0' && c <= '9') ? (c - '0') : -1; };

    int y3 = digit(p[0]), y2 = digit(p[1]), y1 = digit(p[2]), y0 = digit(p[3]);
    int m1 = digit(p[4]), m0 = digit(p[5]);
    int d1 = digit(p[6]), d0 = digit(p[7]);
    if (y3 < 0 || y2 < 0 || y1 < 0 || y0 < 0) return std::nullopt;
    if (m1 < 0 || m0 < 0 || d1 < 0 || d0 < 0) return std::nullopt;
    if (p[8] != '-') return std::nullopt;

    uint32_t year  = static_cast<uint32_t>(y3 * 1000 + y2 * 100 + y1 * 10 + y0);
    uint32_t month = static_cast<uint32_t>(m1 * 10 + m0);
    uint32_t day   = static_cast<uint32_t>(d1 * 10 + d0);

    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;

    constexpr uint8_t kDaysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t max_day = kDaysInMonth[month - 1];
    if (month == 2) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        if (leap) max_day = 29;
    }
    if (day > max_day) return std::nullopt;

    int h1 = digit(p[9]),  h0 = digit(p[10]);
    int n1 = digit(p[12]), n0 = digit(p[13]);
    int s1 = digit(p[15]), s0 = digit(p[16]);
    if (h1 < 0 || h0 < 0 || n1 < 0 || n0 < 0 || s1 < 0 || s0 < 0) return std::nullopt;
    if (p[11] != ':' || p[14] != ':') return std::nullopt;

    uint32_t hour   = static_cast<uint32_t>(h1 * 10 + h0);
    uint32_t minute = static_cast<uint32_t>(n1 * 10 + n0);
    uint32_t second = static_cast<uint32_t>(s1 * 10 + s0);

    // Leap seconds (second=60) are not supported — FIX 4.4 spec uses UTC timestamps
    // without leap second handling. Accepting second=60 for arbitrary dates was incorrect.
    if (hour > 23 || minute > 59 || second > 59) return std::nullopt;

    uint64_t frac_ns = 0;
    if (sv.size() > 17) {
        if (p[17] != '.') return std::nullopt;
        size_t frac_len = sv.size() - 18;
        uint64_t frac_val = 0;
        for (size_t i = 0; i < frac_len; ++i) {
            int d = digit(p[18 + i]);
            if (d < 0) return std::nullopt;
            frac_val = frac_val * 10 + static_cast<uint64_t>(d);
        }
        if (frac_len == 3) frac_ns = frac_val * 1'000'000ULL;
        else if (frac_len == 6) frac_ns = frac_val * 1'000ULL;
        else if (frac_len == 9) frac_ns = frac_val;
    }

    int64_t y = static_cast<int64_t>(year);
    uint32_t m = month;
    if (m <= 2) --y;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = static_cast<uint64_t>(y - era * 400);
    uint64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + day - 1;
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;

    if (days < 0) return std::nullopt;

    uint64_t epoch_sec = static_cast<uint64_t>(days) * 86400
                       + hour * 3600 + minute * 60 + second;
    return epoch_sec * 1'000'000'000ULL + frac_ns;
}

} // namespace detail

/// @brief Error codes from parse().
enum class ParseError : uint8_t {
    kIncomplete,        ///< No complete message found (missing CheckSum tag)
    kInvalidFormat,     ///< Missing BeginString or BodyLength, or malformed tag=value
    kChecksumMismatch,  ///< Computed checksum does not match 10=XXX field
    kFieldOverflow,     ///< Message contains more fields than kMaxFields capacity
};

/// @brief Human-readable name for ParseError.
/// @param e  The error code to convert.
/// @return A string_view description of the error.
constexpr std::string_view parse_error_name(ParseError e) noexcept {
    switch (e) {
    case ParseError::kIncomplete:       return "incomplete";
    case ParseError::kInvalidFormat:    return "invalid format";
    case ParseError::kChecksumMismatch: return "checksum mismatch";
    case ParseError::kFieldOverflow:   return "field overflow";
    }
    return "unknown";
}

/// @brief A single FIX field: tag number + value (zero-copy view into source buffer).
struct Field {
    uint32_t         tag;    ///< FIX tag number (e.g. 55 for Symbol).
    std::string_view value;  ///< Field value as a view into the original buffer.
};

/// @brief Zero-copy view of a parsed FIX message.
///
/// Stores up to MaxFields fields on the stack. All string_view values
/// reference the original input buffer, so the buffer must outlive this object.
///
/// @tparam MaxFieldsV  Maximum number of fields (default 128). Increase for
///                     large FIX messages (e.g. market data snapshots with
///                     many repeating groups).
template <size_t MaxFieldsV = 128>
class BasicMessageView {
public:
    static constexpr size_t kMaxFields = MaxFieldsV;

    /// @brief Number of parsed fields (excluding BeginString, BodyLength, CheckSum).
    [[nodiscard]] size_t field_count() const noexcept { return count_; }

    /// @brief Total consumed bytes of the raw FIX message (including CheckSum SOH).
    [[nodiscard]] size_t total_len() const noexcept { return total_len_; }

    /// @brief BeginString value (tag 8), e.g. "FIX.4.4". Points into the original buffer.
    /// Useful for multi-version FIX handlers that need to dispatch by protocol version.
    [[nodiscard]] std::string_view begin_string() const noexcept { return begin_string_; }

    /// @brief Look up the first field with the given tag.
    /// @param t  FIX tag number to search for.
    /// @return The field value, or nullopt if the tag is not present.
    [[nodiscard]] std::optional<std::string_view> get(uint32_t t) const noexcept {
        for (size_t i = 0; i < count_; ++i) {
            if (fields_[i].tag == t) return fields_[i].value;
        }
        return std::nullopt;
    }

    /// @brief Check if a tag exists in the message.
    /// @param t  FIX tag number to search for.
    [[nodiscard]] bool has(uint32_t t) const noexcept {
        return get(t).has_value();
    }

    /// @brief Convenience: get MsgType (tag 35) value.
    [[nodiscard]] std::optional<std::string_view> msg_type() const noexcept {
        return get(tag::MsgType);
    }

    /// @brief Look up a tag and return its single-character value.
    /// Returns nullopt if the tag is missing or the value is not exactly 1 char.
    /// Useful for FIX single-char enum fields (Side, OrdType, ExecType, etc.).
    [[nodiscard]] std::optional<char> get_char(uint32_t t) const noexcept {
        auto sv = get(t);
        if (!sv) return std::nullopt;
        return detail::parse_char_value(*sv);
    }

    /// @brief Look up a tag and parse its value as int64_t.
    /// Returns nullopt if the value overflows int64_t range.
    [[nodiscard]] std::optional<int64_t> get_int(uint32_t t) const noexcept {
        auto sv = get(t);
        if (!sv) return std::nullopt;
        return detail::parse_int_value(*sv);
    }

    /// @brief Look up a tag and parse its value as double.
    /// Returns nullopt if the value overflows to infinity.
    [[nodiscard]] std::optional<double> get_double(uint32_t t) const noexcept {
        auto sv = get(t);
        if (!sv) return std::nullopt;
        return detail::parse_double_value(*sv);
    }

    /// @brief Look up a tag and parse its value as a FIX boolean (Y/N).
    /// Returns nullopt if the tag is missing or the value is not exactly "Y" or "N".
    [[nodiscard]] std::optional<bool> get_bool(uint32_t t) const noexcept {
        auto sv = get(t);
        if (!sv) return std::nullopt;
        return detail::parse_bool_value(*sv);
    }

    /// @brief Look up a tag and parse its value as a FIX UTCTimestamp.
    ///
    /// Expected format: "YYYYMMDD-HH:MM:SS" or "YYYYMMDD-HH:MM:SS.sss"
    /// @param t  FIX tag number (e.g. tag::SendingTime, tag::TransactTime).
    /// @return Nanoseconds since Unix epoch, or nullopt on parse failure.
    [[nodiscard]] std::optional<uint64_t> get_timestamp(uint32_t t) const noexcept {
        auto sv = get(t);
        if (!sv) return std::nullopt;
        return detail::parse_timestamp_value(*sv);
    }

    /// @brief Random-access iterator over parsed fields.
    using iterator       = const Field*;
    using const_iterator = const Field*;

    [[nodiscard]] const_iterator begin() const noexcept { return fields_; }
    [[nodiscard]] const_iterator end()   const noexcept { return fields_ + count_; }

    /// @brief Iterate over all parsed fields, invoking callback(uint32_t tag, std::string_view value).
    /// @tparam Fn  Callable with signature `void(uint32_t, std::string_view)`.
    /// @param fn   The callback to invoke for each field.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (size_t i = 0; i < count_; ++i) {
            fn(fields_[i].tag, fields_[i].value);
        }
    }

    /// @brief Count how many times a tag appears in the message.
    /// Useful for repeating groups (e.g. NoMDEntries, NoMDEntryTypes).
    [[nodiscard]] size_t count(uint32_t t) const noexcept {
        size_t n = 0;
        for (size_t i = 0; i < count_; ++i) {
            if (fields_[i].tag == t) ++n;
        }
        return n;
    }

    /// @brief Look up the nth occurrence (0-based) of a tag.
    /// Returns nullopt if fewer than n+1 occurrences exist.
    ///
    /// @note For iterating all occurrences, prefer for_each_matching() which
    ///       is O(n) total vs. O(n*k) when calling get_nth() in a loop.
    [[nodiscard]] std::optional<std::string_view> get_nth(uint32_t t, size_t n) const noexcept {
        size_t seen = 0;
        for (size_t i = 0; i < count_; ++i) {
            if (fields_[i].tag == t) {
                if (seen == n) return fields_[i].value;
                ++seen;
            }
        }
        return std::nullopt;
    }

    /// @brief Invoke callback for every occurrence of a tag. O(n) single pass.
    /// Preferred over count()+get_nth() loop for iterating repeating groups.
    ///
    /// Usage:
    ///   msg.for_each_matching(tag::MDEntryPx, [](std::string_view value) {
    ///       double px = ...;
    ///   });
    template <typename Fn>
    void for_each_matching(uint32_t t, Fn&& fn) const {
        for (size_t i = 0; i < count_; ++i) {
            if (fields_[i].tag == t) fn(fields_[i].value);
        }
    }

    /// @brief A single entry in a FIX repeating group -- a span of Field pointers
    /// from the delimiter tag to the next delimiter (or group end).
    struct GroupEntry {
        const Field* fields;  ///< Pointer to first field of this entry
        size_t       count;   ///< Number of fields in this entry

        /// Look up a field within this group entry.
        [[nodiscard]] std::optional<std::string_view> get(uint32_t t) const noexcept {
            for (size_t i = 0; i < count; ++i) {
                if (fields[i].tag == t) return fields[i].value;
            }
            return std::nullopt;
        }

        /// Check if a tag exists in this entry.
        [[nodiscard]] bool has(uint32_t t) const noexcept {
            return get(t).has_value();
        }

        /// Look up a field and return its single-character value.
        [[nodiscard]] std::optional<char> get_char(uint32_t t) const noexcept {
            auto sv = get(t);
            if (!sv) return std::nullopt;
            return detail::parse_char_value(*sv);
        }

        /// Look up a field and parse its value as int64_t.
        [[nodiscard]] std::optional<int64_t> get_int(uint32_t t) const noexcept {
            auto sv = get(t);
            if (!sv) return std::nullopt;
            return detail::parse_int_value(*sv);
        }

        /// Look up a field and parse its value as double.
        [[nodiscard]] std::optional<double> get_double(uint32_t t) const noexcept {
            auto sv = get(t);
            if (!sv) return std::nullopt;
            return detail::parse_double_value(*sv);
        }

        /// Look up a field and parse its value as a FIX boolean (Y/N).
        [[nodiscard]] std::optional<bool> get_bool(uint32_t t) const noexcept {
            auto sv = get(t);
            if (!sv) return std::nullopt;
            return detail::parse_bool_value(*sv);
        }

        /// Look up a field and parse its value as a FIX UTCTimestamp.
        /// Returns nanoseconds since Unix epoch, or nullopt on parse failure.
        [[nodiscard]] std::optional<uint64_t> get_timestamp(uint32_t t) const noexcept {
            auto sv = get(t);
            if (!sv) return std::nullopt;
            return detail::parse_timestamp_value(*sv);
        }

        /// Iterate over fields in this entry.
        [[nodiscard]] const Field* begin() const noexcept { return fields; }
        [[nodiscard]] const Field* end()   const noexcept { return fields + count; }
    };

    /// @brief View over a FIX repeating group -- provides structured iteration.
    ///
    /// FIX repeating groups follow the pattern:
    ///   count_tag=N | delim_tag=v1 | field=v | ... | delim_tag=v2 | field=v | ...
    ///
    /// Each entry starts at a delimiter tag occurrence and extends to the
    /// next delimiter tag or the end of the group's fields.
    struct RepeatingGroupView {
        const GroupEntry* entries;  ///< Pointer to entries array
        size_t            count;    ///< Number of group entries

        [[nodiscard]] size_t size()  const noexcept { return count; }
        [[nodiscard]] bool   empty() const noexcept { return count == 0; }

        [[nodiscard]] const GroupEntry& operator[](size_t i) const noexcept {
            return entries[i];
        }

        [[nodiscard]] const GroupEntry* begin() const noexcept { return entries; }
        [[nodiscard]] const GroupEntry* end()   const noexcept { return entries + count; }
    };

    /// @brief Extract a repeating group by its count tag and delimiter tag.
    ///
    /// @param count_tag  The tag that specifies the number of entries
    ///                   (e.g., tag::NoMDEntries = 268)
    /// @param delim_tag  The first tag of each group entry
    ///                   (e.g., tag::MDEntryType = 269)
    /// @param out_entries Caller-provided buffer for GroupEntry results
    /// @param max_entries Size of out_entries buffer
    /// @return RepeatingGroupView over the populated entries, or empty if
    ///         count_tag is missing or no delimiter tags found
    ///
    /// Usage:
    ///   GroupEntry entries[64];
    ///   auto group = msg.get_group(tag::NoMDEntries, tag::MDEntryType,
    ///                              entries, 64);
    ///   for (auto& entry : group) {
    ///       auto px = entry.get(tag::MDEntryPx);
    ///       auto sz = entry.get(tag::MDEntrySize);
    ///   }
    [[nodiscard]] RepeatingGroupView get_group(
        uint32_t count_tag, uint32_t delim_tag,
        GroupEntry* out_entries, size_t max_entries) const noexcept {

        // Find expected count
        auto count_val = get_int(count_tag);
        if (!count_val || *count_val < 0) {
            return {out_entries, 0};
        }
        // count=0 is valid in FIX (empty group)
        if (*count_val == 0) {
            return {out_entries, 0};
        }

        size_t expected = static_cast<size_t>(*count_val);
        size_t found = 0;

        // Scan for delimiter tag occurrences — each starts a new entry.
        // Entry boundary: next delimiter tag or end of fields.
        // Note: the last entry may include trailing non-group fields —
        // this is by design since we cannot know the group's tag set.
        // Use GroupEntry::get() to extract specific fields safely.
        for (size_t i = 0; i < count_ && found < max_entries; ++i) {
            if (fields_[i].tag == delim_tag) {
                // Find end of this entry: next delimiter or end of fields
                size_t entry_end = count_;
                for (size_t j = i + 1; j < count_; ++j) {
                    if (fields_[j].tag == delim_tag) {
                        entry_end = j;
                        break;
                    }
                }
                out_entries[found] = GroupEntry{
                    .fields = &fields_[i],
                    .count  = entry_end - i,
                };
                ++found;
                i = entry_end - 1;  // loop increment handles +1
            }
        }

        // Warn if we hit the caller-provided buffer limit before scanning all entries
        if (found == max_entries && found < expected) {
            SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
                "get_group: repeating group truncated — found {} entries but "
                "expected {} (max_entries buffer={})", found, expected, max_entries);
        }

        // Clamp to declared count (don't return more entries than the
        // count tag promised, even if extra delimiter tags exist)
        if (found > expected) {
            SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
                "get_group: found {} delimiter tags but count tag declared {}, "
                "clamping to declared count", found, expected);
            found = expected;
        }

        return {out_entries, found};
    }

    // -----------------------------------------------------------------------
    // Diagnostic output
    // -----------------------------------------------------------------------

    /// @brief Multi-line formatted dump for logging/debugging.
    ///
    /// Includes MsgType name, field count, and all tag=value pairs with
    /// human-readable tag names.
    /// @return A formatted multi-line string representation of the message.
    [[nodiscard]] std::string dump() const {
        auto mt = msg_type();
        std::string s;
        s.reserve(80 + count_ * 48);
        std::format_to(std::back_inserter(s),
            "FIX MessageView ({} fields, {} bytes):\n"
            "  BeginString: {}\n"
            "  MsgType: {} ({})",
            count_, total_len_,
            begin_string_,
            mt.value_or("?"),
            mt ? tag::msg_type_name(*mt) : "unknown");
        for (size_t i = 0; i < count_; ++i) {
            std::format_to(std::back_inserter(s), "\n  [{}] {}({})={}", i,
                             tag::tag_name(fields_[i].tag),
                             fields_[i].tag, fields_[i].value);
        }
        return s;
    }

    /// @brief JSON-formatted message for monitoring system integration.
    /// Produces {"msg_type":"D","field_count":5,"total_len":87,"fields":[...]}.
    /// Field values are JSON-escaped (RFC 8259 §7) to prevent malformed output.
    [[nodiscard]] std::string to_json() const {
        auto mt = msg_type();
        // Pre-reserve to avoid repeated reallocations: ~64 bytes per field
        // plus ~80 bytes for the envelope is a reasonable estimate.
        std::string s;
        s.reserve(80 + count_ * 64);
        std::format_to(std::back_inserter(s),
            "{{\"begin_string\":\"{}\","
            "\"msg_type\":\"{}\",\"msg_type_name\":\"{}\","
            "\"field_count\":{},\"total_len\":{},\"fields\":[",
            begin_string_,
            mt.value_or("?"),
            mt ? tag::msg_type_name(*mt) : "unknown",
            count_, total_len_);
        for (size_t i = 0; i < count_; ++i) {
            if (i > 0) s += ',';
            std::format_to(std::back_inserter(s),
                           "{{\"tag\":{},\"name\":\"{}\",\"value\":\"",
                           fields_[i].tag,
                           tag::tag_name(fields_[i].tag));
            detail::json_escape_append(s, fields_[i].value);
            s += "\"}";
        }
        s += "]}";
        return s;
    }

    // -- Internal (used by parse()) --
    // Kept public because MessageView is a value type produced by parse().

    Field  fields_[kMaxFields]{};
    size_t count_     = 0;
    size_t total_len_ = 0;
    std::string_view begin_string_{};

    /// @brief Append a field. Returns false if capacity exceeded.
    /// @param t  FIX tag number.
    /// @param v  Field value (view into original buffer).
    bool push(uint32_t t, std::string_view v) noexcept {
        if (count_ >= kMaxFields) return false;
        fields_[count_++] = {t, v};
        return true;
    }
};

/// @brief Default MessageView with 128-field capacity (sufficient for most FIX messages).
using MessageView = BasicMessageView<>;

/// @brief Parse a raw tag number from decimal ASCII digits.
/// Advances `p` past the '=' delimiter. Returns 0 on failure.
/// FIX tag numbers are small (1–99999 in practice), but we guard against
/// overflow from malformed input to avoid silent wraparound.
///
/// @note Tag 0 is used as the parse-failure sentinel. FIX spec reserves tag 0
///       as undefined, so this is safe. A field "0=value" will be rejected as
///       kInvalidFormat, which is the correct behavior.
inline uint32_t parse_tag_number(const char*& p, const char* end) noexcept {
    constexpr uint32_t kMaxTag = UINT32_MAX / 10;
    uint32_t num = 0;
    if (p == end || *p < '0' || *p > '9') return 0;
    while (p != end && *p != '=') {
        char c = *p++;
        if (c < '0' || c > '9') return 0;
        uint32_t digit = static_cast<uint32_t>(c - '0');
        if (num > kMaxTag) return 0;
        num *= 10;
        if (num > UINT32_MAX - digit) return 0;
        num += digit;
    }
    if (p == end || *p != '=') return 0;
    ++p; // skip '='
    return num;
}

/// @brief Compute FIX checksum: sum of all bytes modulo 256.
/// @param data  Pointer to the bytes to checksum.
/// @param len   Number of bytes.
/// @return The checksum value (0-255).
inline uint8_t compute_checksum(const uint8_t* data, size_t len) noexcept {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum & 0xFF);
}

/// @brief Verify the checksum of a complete FIX message.
/// `data` must point to the start of the message ("8=..."),
/// `len` must be the total message length including the trailing "10=XXX\x01".
///
/// Returns true if the checksum matches.
inline bool verify_checksum(const uint8_t* data, size_t len) noexcept {
    // Find the start of the CheckSum field "10="
    // We search backwards for "\x0110=" from the end.
    if (len < 7) return false; // minimum: "10=000\x01"

    // The checksum field is the last field: "10=XXX\x01"
    // Find it by scanning backwards from the end.
    const char* msg = reinterpret_cast<const char*>(data);
    const char* end = msg + len;

    // Last byte must be SOH
    if (*(end - 1) != '\x01') return false;

    // Find the "10=" prefix of the last field
    // Walk backwards to find the SOH before the checksum field
    const char* cs_start = end - 2; // skip trailing SOH
    while (cs_start > msg && *cs_start != '\x01') --cs_start;
    if (cs_start == msg) {
        // No SOH found -- the checksum field is the first field (malformed)
        return false;
    }
    ++cs_start; // skip SOH, now points at "10=..."

    if (cs_start + 3 >= end || cs_start[0] != '1' || cs_start[1] != '0' || cs_start[2] != '=') {
        return false;
    }

    // Parse the declared checksum value
    std::string_view cs_val(cs_start + 3, static_cast<size_t>(end - 1 - (cs_start + 3)));
    if (cs_val.size() != 3) return false;

    uint32_t declared = 0;
    for (char c : cs_val) {
        if (c < '0' || c > '9') return false;
        declared = declared * 10 + static_cast<uint32_t>(c - '0');
    }

    // Compute checksum over everything before the checksum field (including the SOH before "10=")
    size_t body_len = static_cast<size_t>(cs_start - msg);
    uint8_t computed = compute_checksum(data, body_len);

    return computed == static_cast<uint8_t>(declared);
}

/// @brief Parse a FIX message from raw bytes.
///
/// The parser scans for SOH (0x01) delimiters, extracts tag=value pairs,
/// and validates the message structure (BeginString, BodyLength, CheckSum).
///
/// @tparam MaxFields      Maximum number of fields in the parsed view (default 128)
/// @tparam MaxBodyLength  Maximum allowed BodyLength value (default 1 MB)
/// @param data  Pointer to raw FIX bytes (may contain multiple messages)
/// @param len   Number of available bytes
/// @return BasicMessageView on success, ParseError on failure
///
/// On kIncomplete, the caller should wait for more data and retry.
template <size_t MaxFields = 128, size_t MaxBodyLength = kDefaultMaxBodyLength>
[[nodiscard]] inline std::expected<BasicMessageView<MaxFields>, ParseError>
parse(const uint8_t* data, size_t len) noexcept {
    if (len == 0) return std::unexpected(ParseError::kIncomplete);

    const char* msg = reinterpret_cast<const char*>(data);
    const char* end = msg + len;
    const char* p   = msg;

    // -- First field must be BeginString (tag 8) --
    uint32_t t1 = parse_tag_number(p, end);
    if (t1 != tag::BeginString) {
        SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
            "FIX parse: first tag is {} (expected 8=BeginString), len={}", t1, len);
        return std::unexpected(ParseError::kInvalidFormat);
    }

    const char* v1_start = p;
    while (p != end && *p != '\x01') ++p;
    if (p == end) return std::unexpected(ParseError::kIncomplete);
    std::string_view begin_string(v1_start, static_cast<size_t>(p - v1_start));
    ++p; // skip SOH

    // -- Second field must be BodyLength (tag 9) --
    if (p >= end) return std::unexpected(ParseError::kIncomplete);
    uint32_t t2 = parse_tag_number(p, end);
    if (t2 != tag::BodyLength) {
        SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
            "FIX parse: second tag is {} (expected 9=BodyLength), len={}", t2, len);
        return std::unexpected(ParseError::kInvalidFormat);
    }

    const char* v2_start = p;
    while (p != end && *p != '\x01') ++p;
    if (p == end) return std::unexpected(ParseError::kIncomplete);
    std::string_view body_len_str(v2_start, static_cast<size_t>(p - v2_start));
    ++p; // skip SOH

    // Parse body length value with overflow protection.
    // Validate against kMaxBodyLength inside the loop to prevent
    // integer overflow on maliciously large digit strings.
    size_t body_length = 0;
    for (char c : body_len_str) {
        if (c < '0' || c > '9') {
            SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
                "FIX parse: BodyLength contains non-digit char=0x{:02x}", static_cast<uint8_t>(c));
            return std::unexpected(ParseError::kInvalidFormat);
        }
        if (body_length > MaxBodyLength / 10) {
            SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
                "FIX parse: BodyLength overflow before multiply, body_length={}", body_length);
            return std::unexpected(ParseError::kInvalidFormat);
        }
        body_length = body_length * 10 + static_cast<size_t>(c - '0');
        if (body_length > MaxBodyLength) {
            SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
                "FIX parse: BodyLength exceeds maximum allowed {} bytes",
                MaxBodyLength);
            return std::unexpected(ParseError::kInvalidFormat);
        }
    }

    // Body starts after "9=NNN\x01" and runs for body_length bytes.
    // CheckSum "10=XXX\x01" (7 bytes) follows the body.
    const char* body_start = p;
    size_t header_len = static_cast<size_t>(body_start - msg);

    // Overflow-safe bounds check: verify len >= header_len + body_length + 7
    // without computing a sum that could wrap.
    if (len < header_len + 7 || body_length > len - header_len - 7)
        return std::unexpected(ParseError::kIncomplete);

    size_t total_needed = header_len + body_length + 7; // 7 = "10=XXX\x01"

    // Verify the checksum
    const char* cs_field = body_start + body_length;
    if (cs_field[0] != '1' || cs_field[1] != '0' || cs_field[2] != '=') {
        SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
            "FIX parse: CheckSum field malformed at offset {}, body_length={}",
            header_len + body_length, body_length);
        return std::unexpected(ParseError::kInvalidFormat);
    }
    if (cs_field[6] != '\x01') {
        SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
            "FIX parse: CheckSum field missing trailing SOH, body_length={}", body_length);
        return std::unexpected(ParseError::kInvalidFormat);
    }

    // Parse declared checksum
    uint32_t declared_cs = 0;
    for (int i = 3; i < 6; ++i) {
        char c = cs_field[i];
        if (c < '0' || c > '9') {
            SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
                "FIX parse: CheckSum value contains non-digit char=0x{:02x}", static_cast<uint8_t>(c));
            return std::unexpected(ParseError::kInvalidFormat);
        }
        declared_cs = declared_cs * 10 + static_cast<uint32_t>(c - '0');
    }

    // Compute checksum over everything up to (but not including) "10=..."
    size_t cs_body_len = static_cast<size_t>(cs_field - msg);
    uint8_t computed_cs = compute_checksum(data, cs_body_len);

    if (computed_cs != static_cast<uint8_t>(declared_cs)) {
        SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
            "FIX parse: checksum mismatch: declared={}, computed={}, body_length={}",
            declared_cs, computed_cs, body_length);
        return std::unexpected(ParseError::kChecksumMismatch);
    }

    // Now parse body fields into BasicMessageView
    BasicMessageView<MaxFields> view;
    view.total_len_ = total_needed;
    view.begin_string_ = begin_string;

    const char* bp = body_start;
    const char* body_end = body_start + body_length;

    while (bp < body_end) {
        uint32_t field_tag = parse_tag_number(bp, body_end);
        if (field_tag == 0) {
            SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
                "FIX parse: malformed tag at body offset {}", static_cast<size_t>(bp - body_start));
            return std::unexpected(ParseError::kInvalidFormat);
        }

        const char* val_start = bp;
        while (bp < body_end && *bp != '\x01') ++bp;
        if (bp >= body_end) {
            SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
                "FIX parse: field tag={} value missing SOH delimiter", field_tag);
            return std::unexpected(ParseError::kInvalidFormat);
        }

        std::string_view val(val_start, static_cast<size_t>(bp - val_start));
        ++bp; // skip SOH

        if (!view.push(field_tag, val)) {
            SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
                "FIX parse: field overflow at tag={}, count={}", field_tag, view.field_count());
            return std::unexpected(ParseError::kFieldOverflow);
        }
    }

    return view;
}

/// @brief Parse consecutive FIX messages from a buffer, invoking a callback for each.
///
/// Processes messages sequentially from the start of the buffer.
/// Stops on the first parse error or when the buffer is exhausted.
/// Return false from the callback to stop early.
///
/// @tparam MaxFields  Maximum number of fields per message (default 128)
/// @param data     Pointer to a buffer of concatenated FIX messages
/// @param len      Number of available bytes
/// @param callback Called with (const BasicMessageView<MaxFields>&) for each parsed message.
///                 Return true to continue, false to stop early.
/// @return Number of bytes successfully consumed (sum of parsed message lengths)
template <size_t MaxFields = 128, typename Fn>
    requires std::invocable<Fn, const BasicMessageView<MaxFields>&>
size_t parse_all(const uint8_t* data, size_t len, Fn&& callback) noexcept(
    noexcept(callback(std::declval<const BasicMessageView<MaxFields>&>()))) {
    size_t offset = 0;
    while (offset < len) {
        auto result = parse<MaxFields>(data + offset, len - offset);
        if (!result) break;

        if constexpr (std::is_same_v<std::invoke_result_t<Fn, const BasicMessageView<MaxFields>&>, bool>) {
            if (!callback(*result)) {
                offset += result->total_len();
                break;
            }
        } else {
            callback(*result);
        }
        offset += result->total_len();
    }
    return offset;
}

/// Non-owning parser class for stateless usage or future extension.
///
/// @tparam MaxFields  Maximum number of fields per message (default 128)
template <size_t MaxFields = 128>
class BasicParser {
public:
    /// Parse a FIX message from raw bytes.
    [[nodiscard]] std::expected<BasicMessageView<MaxFields>, ParseError>
    operator()(const uint8_t* data, size_t len) const noexcept {
        return parse<MaxFields>(data, len);
    }
};

/// Default Parser with 128-field capacity.
using Parser = BasicParser<>;

/// @overload Convenience overload accepting std::span.
template <size_t MaxFields = 128, size_t MaxBodyLength = kDefaultMaxBodyLength>
[[nodiscard]] inline std::expected<BasicMessageView<MaxFields>, ParseError>
parse(std::span<const uint8_t> data) noexcept {
    return parse<MaxFields, MaxBodyLength>(data.data(), data.size());
}

/// @overload Convenience overload accepting std::span.
template <size_t MaxFields = 128, typename Fn>
    requires std::invocable<Fn, const BasicMessageView<MaxFields>&>
size_t parse_all(std::span<const uint8_t> data, Fn&& callback) noexcept(
    noexcept(callback(std::declval<const BasicMessageView<MaxFields>&>()))) {
    return parse_all<MaxFields>(data.data(), data.size(), std::forward<Fn>(callback));
}

// ---------------------------------------------------------------------------
// Parser statistics
// ---------------------------------------------------------------------------

/// Lightweight counters for monitoring parse throughput and error rates.
///
/// Not thread-safe — use one instance per thread or protect with a mutex.
/// Designed for hot-path accumulation with a periodic snapshot to a monitoring
/// system (Prometheus, StatsD, etc.).
///
/// Usage:
///   fix::ParserStats stats;
///   fix::parse_all(data, len, [&](const auto& msg) {
///       // handle msg...
///   }, stats);
///   // stats.messages_parsed, stats.bytes_consumed are updated automatically
struct ParserStats {
    uint64_t messages_parsed = 0;   ///< Successfully parsed messages
    uint64_t parse_errors    = 0;   ///< Failed parse attempts
    uint64_t bytes_consumed  = 0;   ///< Total bytes consumed by successful parses
    size_t   first_error_offset = 0;       ///< Byte offset of the first parse error (0 if no errors)
    ParseError first_error_type = {};      ///< Type of the first parse error
    uint8_t    first_error_tag_byte = 0;   ///< First byte at the error location (for diagnostics)

    /// Record a successful parse.
    void on_message(size_t msg_bytes) noexcept {
        ++messages_parsed;
        bytes_consumed += msg_bytes;
    }

    /// Record a parse error with offset context for debugging.
    void on_error(size_t offset, ParseError err, uint8_t tag_byte) noexcept {
        if (parse_errors == 0) {
            first_error_offset = offset;
            first_error_type = err;
            first_error_tag_byte = tag_byte;
        }
        ++parse_errors;
    }

    /// Reset all counters to zero.
    void reset() noexcept {
        messages_parsed = 0;
        parse_errors    = 0;
        bytes_consumed  = 0;
        first_error_offset = 0;
        first_error_type = {};
        first_error_tag_byte = 0;
    }

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        std::string s = std::format(
            "FIX ParserStats:\n"
            "  messages_parsed: {}\n"
            "  parse_errors: {}\n"
            "  bytes_consumed: {}",
            messages_parsed, parse_errors, bytes_consumed);
        if (parse_errors > 0) {
            s += std::format(
                "\n  first_error: {} at offset {} (tag_byte=0x{:02x})",
                parse_error_name(first_error_type),
                first_error_offset, first_error_tag_byte);
        }
        return s;
    }

    /// JSON-formatted stats for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"messages_parsed\":{},\"parse_errors\":{},\"bytes_consumed\":{},"
            "\"first_error_offset\":{},\"first_error_type\":\"{}\","
            "\"first_error_tag_byte\":{}}}",
            messages_parsed, parse_errors, bytes_consumed,
            first_error_offset, parse_error_name(first_error_type),
            first_error_tag_byte);
    }

    /// Compute delta between two snapshots for interval-based monitoring.
    /// Counter fields are diffed; first_error_* fields are taken from the
    /// later (lhs) snapshot since they are point-in-time diagnostics.
    [[nodiscard]] friend ParserStats operator-(const ParserStats& lhs,
                                               const ParserStats& rhs) noexcept {
        return ParserStats{
            .messages_parsed    = lhs.messages_parsed - rhs.messages_parsed,
            .parse_errors       = lhs.parse_errors    - rhs.parse_errors,
            .bytes_consumed     = lhs.bytes_consumed  - rhs.bytes_consumed,
            .first_error_offset = lhs.first_error_offset,
            .first_error_type   = lhs.first_error_type,
            .first_error_tag_byte = lhs.first_error_tag_byte,
        };
    }

    /// Messages parsed per second over a measurement interval.
    /// Apply to a delta snapshot: `auto delta = t2 - t1; delta.throughput(elapsed_ns)`.
    [[nodiscard]] double throughput(uint64_t duration_ns) const noexcept {
        return duration_ns > 0
            ? static_cast<double>(messages_parsed) * 1e9 / static_cast<double>(duration_ns)
            : 0.0;
    }

    /// Parse error rate as a fraction [0.0, 1.0].
    [[nodiscard]] double error_rate() const noexcept {
        uint64_t total = messages_parsed + parse_errors;
        return total > 0
            ? static_cast<double>(parse_errors) / static_cast<double>(total)
            : 0.0;
    }

    [[nodiscard]] friend bool operator==(const ParserStats&, const ParserStats&) = default;
};

/// Parse consecutive FIX messages with statistics accumulation.
///
/// Behaves like parse_all() but also populates a ParserStats struct.
/// The stats are updated for both successes and the final parse failure
/// (if any). Useful for production monitoring of parse throughput.
///
/// @param data     Pointer to a buffer of concatenated FIX messages
/// @param len      Number of available bytes
/// @param callback Called with (const BasicMessageView<MaxFields>&) for each parsed message.
///                 Return true to continue, false to stop early.
/// @param stats    [out] Statistics accumulator
/// @return Number of bytes successfully consumed
template <size_t MaxFields = 128, typename Fn>
    requires std::invocable<Fn, const BasicMessageView<MaxFields>&>
size_t parse_all(const uint8_t* data, size_t len, Fn&& callback, ParserStats& stats) noexcept(
    noexcept(callback(std::declval<const BasicMessageView<MaxFields>&>()))) {
    size_t offset = 0;
    while (offset < len) {
        auto result = parse<MaxFields>(data + offset, len - offset);
        if (!result) {
            // kIncomplete is normal (partial trailing message), not an error
            if (result.error() != ParseError::kIncomplete) {
                uint8_t err_byte = (offset < len) ? data[offset] : 0;
                stats.on_error(offset, result.error(), err_byte);
                SPDLOG_LOGGER_DEBUG(detail::fix_parser_logger(),
                    "FIX parse error at offset {}: {} (byte=0x{:02x})",
                    offset, parse_error_name(result.error()), err_byte);
            }
            break;
        }

        stats.on_message(result->total_len());

        if constexpr (std::is_same_v<std::invoke_result_t<Fn, const BasicMessageView<MaxFields>&>, bool>) {
            if (!callback(*result)) {
                offset += result->total_len();
                break;
            }
        } else {
            callback(*result);
        }
        offset += result->total_len();
    }
    return offset;
}

// ---------------------------------------------------------------------------
// Tag types for type-safe dispatch
// ---------------------------------------------------------------------------

/// @brief Empty tag structs used for compile-time MsgType discrimination.
///
/// Each struct corresponds to a FIX 4.4 MsgType (tag 35) value and is used
/// as an overload tag by dispatch(). They carry no runtime state — overload
/// resolution selects the correct handler at compile time with zero runtime
/// cost. Mirrors the eph::itch::msg pattern.
namespace msg {
struct Heartbeat {};             ///< MsgType '0' — session heartbeat (tag 35=0).
struct TestRequest {};           ///< MsgType '1' — session TestRequest (tag 35=1).
struct Logon {};                 ///< MsgType 'A' — session Logon (tag 35=A).
struct Logout {};                ///< MsgType '5' — session Logout (tag 35=5).
struct NewOrderSingle {};        ///< MsgType 'D' — new single-instrument order (tag 35=D).
struct OrderCancelRequest {};    ///< MsgType 'F' — cancel request (tag 35=F).
struct OrderCancelReplace {};    ///< MsgType 'G' — cancel/replace request (tag 35=G).
struct ExecutionReport {};       ///< MsgType '8' — execution/ack report (tag 35=8).
struct OrderCancelReject {};     ///< MsgType '9' — cancel/replace reject (tag 35=9).
struct MarketDataRequest {};     ///< MsgType 'V' — market data subscription request (tag 35=V).
struct MarketDataSnapshot {};    ///< MsgType 'W' — market data snapshot / full refresh (tag 35=W).
struct MarketDataIncRefresh {};  ///< MsgType 'X' — market data incremental refresh (tag 35=X).
// Multi-character MsgType (FIX 4.4+)
struct TradeCaptureReport {};    ///< MsgType "AE" — trade capture report (FIX 4.4+).
struct TradeCaptureReportAck {}; ///< MsgType "AR" — trade capture report ack (FIX 4.4+).
struct SecurityDefinition {};    ///< MsgType 'd' — security definition (FIX 4.4+).
struct SecurityStatus {};        ///< MsgType 'f' — security status (FIX 4.4+).
struct PositionReport {};        ///< MsgType "AP" — position report (FIX 4.4+).
struct MassQuote {};             ///< MsgType 'i' — mass quote (FIX 4.4+).
struct QuoteCancel {};           ///< MsgType 'Z' — quote cancel (FIX 4.4+).
struct SecurityList {};          ///< MsgType 'y' — security list response (FIX 4.4+).
struct SecurityListRequest {};   ///< MsgType 'x' — security list request (FIX 4.4+).
struct Unknown {};               ///< Fallback tag for missing or unrecognized MsgType values.
} // namespace msg

// ---------------------------------------------------------------------------
// dispatch() — type-safe visitor for FIX messages
// ---------------------------------------------------------------------------

/// Dispatch a parsed FIX message to a handler using tag-type overload resolution.
///
/// The handler is invoked as `handler(Tag{}, view)` where:
///   - Tag is one of the msg:: structs above (compile-time MsgType)
///   - view is the parsed MessageView (by const reference)
///
/// Supports both single-char (e.g. "D", "8") and multi-char (e.g. "AE", "AP")
/// MsgType values. Multi-char types are matched first via string comparison;
/// single-char types fall through to a fast switch.
///
/// Usage with overload set:
///   struct MyHandler {
///       void operator()(fix::msg::NewOrderSingle, const auto& v) { ... }
///       void operator()(fix::msg::ExecutionReport, const auto& v) { ... }
///       template <typename T, typename V>
///       void operator()(T, const V&) { /* default: ignore */ }
///   };
///   fix::dispatch(msg_view, MyHandler{});
///
/// If MsgType is missing or unrecognized, msg::Unknown is dispatched.
template <size_t MaxFields = 128, typename Handler>
decltype(auto) dispatch(const BasicMessageView<MaxFields>& view, Handler&& handler) {
    auto mt = view.msg_type();
    if (!mt || mt->empty()) {
        return handler(msg::Unknown{}, view);
    }

    // Multi-char MsgType dispatch (FIX 4.4+)
    if (mt->size() > 1) {
        if (*mt == tag::msg_type::TradeCaptureReport)    return handler(msg::TradeCaptureReport{}, view);
        if (*mt == tag::msg_type::TradeCaptureReportAck) return handler(msg::TradeCaptureReportAck{}, view);
        if (*mt == tag::msg_type::PositionReport)        return handler(msg::PositionReport{}, view);
        return handler(msg::Unknown{}, view);
    }

    // Single-char MsgType dispatch (fast path)
    char c = (*mt)[0];
    switch (c) {
    case tag::msg_type::Heartbeat:            return handler(msg::Heartbeat{}, view);
    case tag::msg_type::TestRequest:          return handler(msg::TestRequest{}, view);
    case tag::msg_type::Logon:                return handler(msg::Logon{}, view);
    case tag::msg_type::Logout:               return handler(msg::Logout{}, view);
    case tag::msg_type::NewOrderSingle:       return handler(msg::NewOrderSingle{}, view);
    case tag::msg_type::OrderCancelRequest:   return handler(msg::OrderCancelRequest{}, view);
    case tag::msg_type::OrderCancelReplace:   return handler(msg::OrderCancelReplace{}, view);
    case tag::msg_type::ExecutionReport:      return handler(msg::ExecutionReport{}, view);
    case tag::msg_type::OrderCancelReject:    return handler(msg::OrderCancelReject{}, view);
    case tag::msg_type::MarketDataRequest:    return handler(msg::MarketDataRequest{}, view);
    case tag::msg_type::MarketDataSnapshot:   return handler(msg::MarketDataSnapshot{}, view);
    case tag::msg_type::MarketDataIncRefresh: return handler(msg::MarketDataIncRefresh{}, view);
    case tag::msg_type::SecurityDefinition:   return handler(msg::SecurityDefinition{}, view);
    case tag::msg_type::SecurityStatus:       return handler(msg::SecurityStatus{}, view);
    case tag::msg_type::MassQuote:            return handler(msg::MassQuote{}, view);
    case tag::msg_type::QuoteCancel:          return handler(msg::QuoteCancel{}, view);
    case tag::msg_type::SecurityList:         return handler(msg::SecurityList{}, view);
    case tag::msg_type::SecurityListRequest:  return handler(msg::SecurityListRequest{}, view);
    default:                                  return handler(msg::Unknown{}, view);
    }
}

} // namespace eph::fix

/// std::formatter specialization for fix::ParseError.
template <>
struct std::formatter<eph::fix::ParseError> : std::formatter<std::string_view> {
    auto format(eph::fix::ParseError e, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::fix::parse_error_name(e), ctx);
    }
};

/// std::formatter specialization for fix::BasicMessageView.
///
/// Formats a parsed FIX message as "tag=value|tag=value|..." with pipe delimiters.
/// Tag numbers are emitted as-is (use tag::tag_name() separately for human-readable names).
/// Example output: "35=D|49=SENDER|56=TARGET|55=AAPL|54=1|44=150.50"
template <size_t N>
struct std::formatter<eph::fix::BasicMessageView<N>> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(const eph::fix::BasicMessageView<N>& msg, auto& ctx) const {
        auto out = ctx.out();
        for (size_t i = 0; i < msg.field_count(); ++i) {
            if (i > 0) *out++ = '|';
            out = std::format_to(out, "{}={}", msg.fields_[i].tag, msg.fields_[i].value);
        }
        return out;
    }
};

/// std::formatter specialization for fix::ParserStats.
template <>
struct std::formatter<eph::fix::ParserStats> : std::formatter<std::string> {
    auto format(const eph::fix::ParserStats& s, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("FIX(parsed={} errors={} bytes={})",
                s.messages_parsed, s.parse_errors, s.bytes_consumed),
            ctx);
    }
};
