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
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/fix/tags.hpp"

namespace eph::fix {

namespace detail {
inline std::shared_ptr<spdlog::logger> fix_parser_logger() {
    static auto l = [] {
        auto lg = spdlog::get("fix.parser");
        if (!lg) lg = spdlog::stdout_color_mt("fix.parser");
        return lg;
    }();
    return l;
}
} // namespace detail

/// Error codes from parse().
enum class ParseError : uint8_t {
    kIncomplete,        ///< No complete message found (missing CheckSum tag)
    kInvalidFormat,     ///< Missing BeginString or BodyLength, or malformed tag=value
    kChecksumMismatch,  ///< Computed checksum does not match 10=XXX field
    kFieldOverflow,     ///< Message contains more fields than kMaxFields capacity
};

/// Human-readable name for ParseError.
constexpr std::string_view parse_error_name(ParseError e) noexcept {
    switch (e) {
    case ParseError::kIncomplete:       return "incomplete";
    case ParseError::kInvalidFormat:    return "invalid format";
    case ParseError::kChecksumMismatch: return "checksum mismatch";
    case ParseError::kFieldOverflow:   return "field overflow";
    }
    return "unknown";
}

/// A single FIX field: tag number + value (zero-copy view into source buffer).
struct Field {
    uint32_t         tag;
    std::string_view value;
};

/// Zero-copy view of a parsed FIX message.
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

    /// Number of parsed fields (excluding BeginString, BodyLength, CheckSum).
    [[nodiscard]] size_t field_count() const noexcept { return count_; }

    /// Total consumed bytes of the raw FIX message (including CheckSum SOH).
    [[nodiscard]] size_t total_len() const noexcept { return total_len_; }

    /// Look up the first field with the given tag.
    [[nodiscard]] std::optional<std::string_view> get(uint32_t t) const noexcept {
        for (size_t i = 0; i < count_; ++i) {
            if (fields_[i].tag == t) return fields_[i].value;
        }
        return std::nullopt;
    }

    /// Check if a tag exists in the message.
    [[nodiscard]] bool has(uint32_t t) const noexcept {
        return get(t).has_value();
    }

    /// Convenience: get MsgType (tag 35) value.
    [[nodiscard]] std::optional<std::string_view> msg_type() const noexcept {
        return get(tag::MsgType);
    }

    /// Look up a tag and return its single-character value.
    /// Returns nullopt if the tag is missing or the value is not exactly 1 char.
    /// Useful for FIX single-char enum fields (Side, OrdType, ExecType, etc.).
    [[nodiscard]] std::optional<char> get_char(uint32_t t) const noexcept {
        auto sv = get(t);
        if (!sv || sv->size() != 1) return std::nullopt;
        return (*sv)[0];
    }

    /// Look up a tag and parse its value as int64_t.
    /// Returns nullopt if the value overflows int64_t range.
    [[nodiscard]] std::optional<int64_t> get_int(uint32_t t) const noexcept {
        auto sv = get(t);
        if (!sv || sv->empty()) return std::nullopt;

        const char* p   = sv->data();
        const char* end = p + sv->size();
        bool neg = false;
        if (*p == '-') { neg = true; ++p; }
        if (p == end) return std::nullopt;

        // Parse as uint64_t with overflow detection, then apply sign.
        // Max positive: 9223372036854775807 (INT64_MAX)
        // Max negative magnitude: 9223372036854775808 (|INT64_MIN|)
        constexpr uint64_t kMaxPositive = static_cast<uint64_t>(INT64_MAX);
        constexpr uint64_t kMaxNegative = kMaxPositive + 1;
        constexpr uint64_t kOverflowThreshold = UINT64_MAX / 10;

        uint64_t val = 0;
        while (p != end) {
            char c = *p++;
            if (c < '0' || c > '9') return std::nullopt;
            uint64_t digit = static_cast<uint64_t>(c - '0');
            // Check for multiplication overflow
            if (val > kOverflowThreshold) return std::nullopt;
            val *= 10;
            if (val > UINT64_MAX - digit) return std::nullopt;
            val += digit;
        }

        if (neg) {
            if (val > kMaxNegative) return std::nullopt;
            // Safe: kMaxNegative == |INT64_MIN|, and -uint64_t is well-defined
            return static_cast<int64_t>(-val);
        }
        if (val > kMaxPositive) return std::nullopt;
        return static_cast<int64_t>(val);
    }

    /// Look up a tag and parse its value as double.
    /// Returns nullopt if the value overflows to infinity.
    [[nodiscard]] std::optional<double> get_double(uint32_t t) const noexcept {
        auto sv = get(t);
        if (!sv || sv->empty()) return std::nullopt;

        const char* p   = sv->data();
        const char* end = p + sv->size();
        bool neg = false;
        if (*p == '-') { neg = true; ++p; }
        if (p == end) return std::nullopt;

        double val = 0.0;
        // Integer part
        while (p != end && *p != '.') {
            char c = *p++;
            if (c < '0' || c > '9') return std::nullopt;
            val = val * 10.0 + (c - '0');
        }
        // Fractional part
        if (p != end && *p == '.') {
            ++p;
            double frac = 0.0;
            double divisor = 1.0;
            while (p != end) {
                char c = *p++;
                if (c < '0' || c > '9') return std::nullopt;
                frac = frac * 10.0 + (c - '0');
                divisor *= 10.0;
            }
            val += frac / divisor;
        }
        // Guard against overflow to infinity from extremely large inputs.
        // Use direct comparison instead of std::isfinite for portability.
        if (val == std::numeric_limits<double>::infinity()) return std::nullopt;
        return neg ? -val : val;
    }

    /// Look up a tag and parse its value as a FIX boolean (Y/N).
    /// Returns nullopt if the tag is missing or the value is not exactly "Y" or "N".
    /// Useful for FIX boolean fields (PossDupFlag, PossResend, ResetSeqNumFlag, etc.).
    [[nodiscard]] std::optional<bool> get_bool(uint32_t t) const noexcept {
        auto c = get_char(t);
        if (!c) return std::nullopt;
        if (*c == 'Y') return true;
        if (*c == 'N') return false;
        return std::nullopt;
    }

    /// Look up a tag and parse its value as a FIX UTCTimestamp.
    /// Expected format: "YYYYMMDD-HH:MM:SS" or "YYYYMMDD-HH:MM:SS.sss"
    /// Returns nanoseconds since Unix epoch, or nullopt on parse failure.
    [[nodiscard]] std::optional<uint64_t> get_timestamp(uint32_t t) const noexcept {
        auto sv = get(t);
        if (!sv) return std::nullopt;

        // Minimum: "YYYYMMDD-HH:MM:SS" (17 chars)
        // With millis: "YYYYMMDD-HH:MM:SS.sss" (21 chars)
        // With micros: "YYYYMMDD-HH:MM:SS.ssssss" (24 chars)
        // With nanos:  "YYYYMMDD-HH:MM:SS.sssssssss" (27 chars)
        if (sv->size() != 17 && sv->size() != 21 &&
            sv->size() != 24 && sv->size() != 27) return std::nullopt;

        const char* p = sv->data();

        // Parse date: YYYYMMDD
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

        // Validate day-of-month for the specific month/year
        // (prevents accepting nonsense like Feb 31)
        constexpr uint8_t kDaysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        uint32_t max_day = kDaysInMonth[month - 1];
        if (month == 2) {
            bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
            if (leap) max_day = 29;
        }
        if (day > max_day) return std::nullopt;

        // Parse time: HH:MM:SS
        int h1 = digit(p[9]),  h0 = digit(p[10]);
        int n1 = digit(p[12]), n0 = digit(p[13]);
        int s1 = digit(p[15]), s0 = digit(p[16]);
        if (h1 < 0 || h0 < 0 || n1 < 0 || n0 < 0 || s1 < 0 || s0 < 0) return std::nullopt;
        if (p[11] != ':' || p[14] != ':') return std::nullopt;

        uint32_t hour   = static_cast<uint32_t>(h1 * 10 + h0);
        uint32_t minute = static_cast<uint32_t>(n1 * 10 + n0);
        uint32_t second = static_cast<uint32_t>(s1 * 10 + s0);

        if (hour > 23 || minute > 59 || second > 60) return std::nullopt;

        // Parse optional sub-second fraction (ms, us, or ns)
        uint64_t frac_ns = 0;
        if (sv->size() > 17) {
            if (p[17] != '.') return std::nullopt;
            size_t frac_len = sv->size() - 18; // digits after '.'
            uint64_t frac_val = 0;
            for (size_t i = 0; i < frac_len; ++i) {
                int d = digit(p[18 + i]);
                if (d < 0) return std::nullopt;
                frac_val = frac_val * 10 + static_cast<uint64_t>(d);
            }
            // Scale to nanoseconds: ms(*1e6), us(*1e3), ns(*1)
            if (frac_len == 3) frac_ns = frac_val * 1'000'000ULL;      // ms
            else if (frac_len == 6) frac_ns = frac_val * 1'000ULL;     // us
            else if (frac_len == 9) frac_ns = frac_val;                 // ns
        }

        // Civil date → days since epoch (inverse of Howard Hinnant algorithm)
        // Adjust month so March = 0 (era-based calendar)
        int64_t y = static_cast<int64_t>(year);
        uint32_t m = month;
        if (m <= 2) --y;
        int64_t era = (y >= 0 ? y : y - 399) / 400;
        uint64_t yoe = static_cast<uint64_t>(y - era * 400);
        uint64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + day - 1;
        uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;

        // Reject pre-epoch timestamps (return type is uint64_t, cannot represent negative)
        if (days < 0) return std::nullopt;

        uint64_t epoch_sec = static_cast<uint64_t>(days) * 86400
                           + hour * 3600 + minute * 60 + second;
        return epoch_sec * 1'000'000'000ULL + frac_ns;
    }

    /// Random-access iterator over parsed fields.
    using iterator       = const Field*;
    using const_iterator = const Field*;

    [[nodiscard]] const_iterator begin() const noexcept { return fields_; }
    [[nodiscard]] const_iterator end()   const noexcept { return fields_ + count_; }

    /// Iterate over all parsed fields, invoking callback(uint32_t tag, std::string_view value).
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (size_t i = 0; i < count_; ++i) {
            fn(fields_[i].tag, fields_[i].value);
        }
    }

    /// Count how many times a tag appears in the message.
    /// Useful for repeating groups (e.g. NoMDEntries, NoMDEntryTypes).
    [[nodiscard]] size_t count(uint32_t t) const noexcept {
        size_t n = 0;
        for (size_t i = 0; i < count_; ++i) {
            if (fields_[i].tag == t) ++n;
        }
        return n;
    }

    /// Look up the nth occurrence (0-based) of a tag.
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

    /// Invoke callback for every occurrence of a tag. O(n) single pass.
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

    // -- Internal (used by parse()) --
    // Kept public because MessageView is a value type produced by parse().

    Field  fields_[kMaxFields]{};
    size_t count_     = 0;
    size_t total_len_ = 0;

    /// Append a field. Returns false if capacity exceeded.
    bool push(uint32_t t, std::string_view v) noexcept {
        if (count_ >= kMaxFields) return false;
        fields_[count_++] = {t, v};
        return true;
    }
};

/// Default MessageView with 128-field capacity (sufficient for most FIX messages).
using MessageView = BasicMessageView<>;

/// Parse a raw tag number from decimal ASCII digits.
/// Advances `p` past the '=' delimiter. Returns 0 on failure.
/// FIX tag numbers are small (1–99999 in practice), but we guard against
/// overflow from malformed input to avoid silent wraparound.
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

/// Compute FIX checksum: sum of all bytes modulo 256.
inline uint8_t compute_checksum(const uint8_t* data, size_t len) noexcept {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return static_cast<uint8_t>(sum & 0xFF);
}

/// Verify the checksum of a complete FIX message.
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

/// Parse a FIX message from raw bytes.
///
/// The parser scans for SOH (0x01) delimiters, extracts tag=value pairs,
/// and validates the message structure (BeginString, BodyLength, CheckSum).
///
/// @tparam MaxFields  Maximum number of fields in the parsed view (default 128)
/// @param data  Pointer to raw FIX bytes (may contain multiple messages)
/// @param len   Number of available bytes
/// @return BasicMessageView on success, ParseError on failure
///
/// On kIncomplete, the caller should wait for more data and retry.
template <size_t MaxFields = 128>
inline std::expected<BasicMessageView<MaxFields>, ParseError>
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

    // Parse body length value
    size_t body_length = 0;
    for (char c : body_len_str) {
        if (c < '0' || c > '9') {
            SPDLOG_LOGGER_WARN(detail::fix_parser_logger(),
                "FIX parse: BodyLength contains non-digit char=0x{:02x}", static_cast<uint8_t>(c));
            return std::unexpected(ParseError::kInvalidFormat);
        }
        body_length = body_length * 10 + static_cast<size_t>(c - '0');
    }

    // Body starts after "9=NNN\x01" and runs for body_length bytes.
    // CheckSum "10=XXX\x01" (7 bytes) follows the body.
    const char* body_start = p;
    size_t header_len = static_cast<size_t>(body_start - msg);
    size_t total_needed = header_len + body_length + 7; // 7 = "10=XXX\x01"

    if (len < total_needed) return std::unexpected(ParseError::kIncomplete);

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

/// Parse consecutive FIX messages from a buffer, invoking a callback for each.
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

// ---------------------------------------------------------------------------
// Tag types for type-safe dispatch
// ---------------------------------------------------------------------------

/// Tag types — empty structs used for compile-time MsgType discrimination.
/// Mirrors the eph::itch::msg pattern for zero-overhead dispatch.
namespace msg {
struct Heartbeat {};
struct TestRequest {};
struct Logon {};
struct Logout {};
struct NewOrderSingle {};
struct OrderCancelRequest {};
struct OrderCancelReplace {};
struct ExecutionReport {};
struct OrderCancelReject {};
struct MarketDataRequest {};
struct MarketDataSnapshot {};
struct MarketDataIncRefresh {};
// Multi-character MsgType (FIX 4.4+)
struct TradeCaptureReport {};
struct TradeCaptureReportAck {};
struct SecurityDefinition {};
struct SecurityStatus {};
struct PositionReport {};
struct MassQuote {};
struct QuoteCancel {};
struct SecurityList {};
struct SecurityListRequest {};
struct Unknown {};
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
