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

#include "eph/fix/tags.hpp"

namespace eph::fix {

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
/// Stores up to kMaxFields fields on the stack. All string_view values
/// reference the original input buffer, so the buffer must outlive this object.
class MessageView {
public:
    static constexpr size_t kMaxFields = 128;

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
        for (size_t i = 0; i < count_; ++i) {
            if (fields_[i].tag == t) return true;
        }
        return false;
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
/// @param data  Pointer to raw FIX bytes (may contain multiple messages)
/// @param len   Number of available bytes
/// @return MessageView on success, ParseError on failure
///
/// On kIncomplete, the caller should wait for more data and retry.
inline std::expected<MessageView, ParseError>
parse(const uint8_t* data, size_t len) noexcept {
    if (len == 0) return std::unexpected(ParseError::kIncomplete);

    const char* msg = reinterpret_cast<const char*>(data);
    const char* end = msg + len;
    const char* p   = msg;

    // -- First field must be BeginString (tag 8) --
    uint32_t t1 = parse_tag_number(p, end);
    if (t1 != tag::BeginString) return std::unexpected(ParseError::kInvalidFormat);

    const char* v1_start = p;
    while (p != end && *p != '\x01') ++p;
    if (p == end) return std::unexpected(ParseError::kIncomplete);
    std::string_view begin_string(v1_start, static_cast<size_t>(p - v1_start));
    ++p; // skip SOH

    // -- Second field must be BodyLength (tag 9) --
    if (p >= end) return std::unexpected(ParseError::kIncomplete);
    uint32_t t2 = parse_tag_number(p, end);
    if (t2 != tag::BodyLength) return std::unexpected(ParseError::kInvalidFormat);

    const char* v2_start = p;
    while (p != end && *p != '\x01') ++p;
    if (p == end) return std::unexpected(ParseError::kIncomplete);
    std::string_view body_len_str(v2_start, static_cast<size_t>(p - v2_start));
    ++p; // skip SOH

    // Parse body length value
    size_t body_length = 0;
    for (char c : body_len_str) {
        if (c < '0' || c > '9') return std::unexpected(ParseError::kInvalidFormat);
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
        return std::unexpected(ParseError::kInvalidFormat);
    }
    if (cs_field[6] != '\x01') {
        return std::unexpected(ParseError::kInvalidFormat);
    }

    // Parse declared checksum
    uint32_t declared_cs = 0;
    for (int i = 3; i < 6; ++i) {
        char c = cs_field[i];
        if (c < '0' || c > '9') return std::unexpected(ParseError::kInvalidFormat);
        declared_cs = declared_cs * 10 + static_cast<uint32_t>(c - '0');
    }

    // Compute checksum over everything up to (but not including) "10=..."
    size_t cs_body_len = static_cast<size_t>(cs_field - msg);
    uint8_t computed_cs = compute_checksum(data, cs_body_len);

    if (computed_cs != static_cast<uint8_t>(declared_cs)) {
        return std::unexpected(ParseError::kChecksumMismatch);
    }

    // Now parse body fields into MessageView
    MessageView view;
    view.total_len_ = total_needed;

    const char* bp = body_start;
    const char* body_end = body_start + body_length;

    while (bp < body_end) {
        uint32_t field_tag = parse_tag_number(bp, body_end);
        if (field_tag == 0) return std::unexpected(ParseError::kInvalidFormat);

        const char* val_start = bp;
        while (bp < body_end && *bp != '\x01') ++bp;
        if (bp >= body_end) return std::unexpected(ParseError::kInvalidFormat);

        std::string_view val(val_start, static_cast<size_t>(bp - val_start));
        ++bp; // skip SOH

        if (!view.push(field_tag, val)) {
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
/// @param data     Pointer to a buffer of concatenated FIX messages
/// @param len      Number of available bytes
/// @param callback Called with (const MessageView&) for each parsed message.
///                 Return true to continue, false to stop early.
/// @return Number of bytes successfully consumed (sum of parsed message lengths)
template <typename Fn>
    requires std::invocable<Fn, const MessageView&>
size_t parse_all(const uint8_t* data, size_t len, Fn&& callback) noexcept(
    noexcept(callback(std::declval<const MessageView&>()))) {
    size_t offset = 0;
    while (offset < len) {
        auto result = parse(data + offset, len - offset);
        if (!result) break;

        if constexpr (std::is_same_v<std::invoke_result_t<Fn, const MessageView&>, bool>) {
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
class Parser {
public:
    /// Parse a FIX message from raw bytes.
    [[nodiscard]] std::expected<MessageView, ParseError>
    operator()(const uint8_t* data, size_t len) const noexcept {
        return parse(data, len);
    }
};

} // namespace eph::fix

/// std::formatter specialization for fix::ParseError.
template <>
struct std::formatter<eph::fix::ParseError> : std::formatter<std::string_view> {
    auto format(eph::fix::ParseError e, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::fix::parse_error_name(e), ctx);
    }
};

/// std::formatter specialization for fix::MessageView.
///
/// Formats a parsed FIX message as "tag=value|tag=value|..." with pipe delimiters.
/// Tag numbers are emitted as-is (use tag::tag_name() separately for human-readable names).
/// Example output: "35=D|49=SENDER|56=TARGET|55=AAPL|54=1|44=150.50"
template <>
struct std::formatter<eph::fix::MessageView> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    auto format(const eph::fix::MessageView& msg, auto& ctx) const {
        auto out = ctx.out();
        for (size_t i = 0; i < msg.field_count(); ++i) {
            if (i > 0) *out++ = '|';
            out = std::format_to(out, "{}={}", msg.fields_[i].tag, msg.fields_[i].value);
        }
        return out;
    }
};
