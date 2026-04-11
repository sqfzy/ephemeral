/// @file core/json_scan.hpp
/// Minimal JSON numeric-field scanner for bench mock payloads (Phase 10).
///
/// Rationale (plan D-5): lat_ex_market / lat_ex_order measure end-to-end
/// one-way latency by extracting a server-stamped `"T":<ns>` field from
/// the mock WS/UDP payload. Pulling in `eph-json` for this would couple
/// Phase 10 to the eph-json rewrite and drag in the full JSON codec
/// machinery for a single field lookup. Instead: hand-written ~20-line
/// scanner that matches the mock's known output shape and nothing more.
///
/// NOT A GENERAL JSON PARSER. Constraints on mock payloads:
///   - field names are ASCII, no backslash-escapes
///   - field values are decimal unsigned integers (0–UINT64_MAX)
///   - the field delimiter is exactly `":"` (no whitespace inside)
///   - whitespace between `":"` and the first digit is tolerated
///     (space and tab only)
///
/// These match what `json.dumps(...)` in the Python mocks produces for
/// numeric fields; cross-checked in sub-phase 10.2.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace bench {

/// Search `data[0..len)` for an integer field whose key is `field_name`
/// and return its parsed value, or `std::nullopt` if the field is absent
/// or unparseable.
///
/// The scan is `O(len * field_name.size())` but `field_name.size()` is
/// typically < 8 chars and bench mock payloads are < 1 KB, so this is
/// not a hot-path concern — it runs once per measured message.
///
/// Caller contract: `data` may be `nullptr` only if `len == 0`. The
/// function is `noexcept` and never reads past `data + len`.
///
/// ```cpp
/// const uint8_t buf[] = R"({"s":"BTC","T":1712345678901234567})";
/// auto t = bench::scan_json_uint_field(buf, sizeof(buf) - 1, "T");
/// // *t == 1712345678901234567
/// ```
[[nodiscard]] inline std::optional<uint64_t>
scan_json_uint_field(const uint8_t* data,
                     std::size_t len,
                     std::string_view field_name) noexcept {
    // Cheapest possible pre-check: we need at least `"K":D` which is
    // field_name.size() + 4 bytes (two quotes, colon, one digit).
    if (data == nullptr || field_name.empty() ||
        len < field_name.size() + 4) {
        return std::nullopt;
    }

    // Build a small on-stack needle: `"<field>":`
    // 256 is generous for bench keys (typical is "T" or "ts").
    char needle[256];
    const std::size_t needle_len = field_name.size() + 3;
    if (needle_len > sizeof(needle)) {
        return std::nullopt; // field name too long for our stack buffer
    }
    needle[0] = '"';
    std::memcpy(needle + 1, field_name.data(), field_name.size());
    needle[1 + field_name.size()] = '"';
    needle[2 + field_name.size()] = ':';

    // Linear byte scan — std::memmem would be faster but is glibc-only
    // and the payload sizes here do not justify the portability cost.
    for (std::size_t i = 0; i + needle_len <= len; ++i) {
        if (std::memcmp(data + i, needle, needle_len) != 0) continue;

        // Found `"field":`. Parse the following digits with optional
        // leading space/tab.
        std::size_t pos = i + needle_len;
        while (pos < len && (data[pos] == ' ' || data[pos] == '\t')) {
            ++pos;
        }
        // Must have at least one decimal digit immediately after.
        if (pos >= len || data[pos] < '0' || data[pos] > '9') {
            return std::nullopt;
        }

        uint64_t value = 0;
        while (pos < len && data[pos] >= '0' && data[pos] <= '9') {
            const uint64_t digit = static_cast<uint64_t>(data[pos] - '0');
            // Overflow check: prevent wraparound on very long digit
            // runs (malformed payload).
            if (value > (UINT64_MAX - digit) / 10) {
                return std::nullopt;
            }
            value = value * 10 + digit;
            ++pos;
        }
        return value;
    }

    return std::nullopt;
}

} // namespace bench
