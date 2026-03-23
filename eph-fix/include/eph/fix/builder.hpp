#pragma once

/// @file builder.hpp
/// Zero-copy FIX message builder.
///
/// Writes tag=value\x01 fields directly into a user-provided buffer.
/// On finish(), prepends BeginString + BodyLength and appends CheckSum.
/// No heap allocations.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/fix/tags.hpp"

namespace eph::fix {

namespace detail {
inline std::shared_ptr<spdlog::logger> fix_builder_logger() {
    static auto l = [] {
        auto lg = spdlog::get("fix.builder");
        if (!lg) lg = spdlog::stdout_color_mt("fix.builder");
        return lg;
    }();
    return l;
}
} // namespace detail

/// Builds a FIX message into a caller-provided buffer.
///
/// Usage:
///   uint8_t buf[1024];
///   MessageBuilder b(buf, sizeof(buf));
///   b.set(tag::MsgType, "D");
///   b.set(tag::SenderCompID, "SENDER");
///   b.set_int(tag::MsgSeqNum, 1);
///   b.set(tag::Symbol, "AAPL");
///   b.set_int(tag::Side, 1);
///   b.set_int(tag::OrderQty, 100);
///   b.set_int(tag::OrdType, 2);
///   b.set_double(tag::Price, 150.50);
///   size_t len = b.finish();  // len == 0 on buffer overflow
class MessageBuilder {
public:
    /// @param buf      Output buffer (caller-owned, must outlive builder)
    /// @param capacity Total buffer size in bytes
    explicit MessageBuilder(uint8_t* buf, size_t capacity) noexcept
        : buf_(buf), capacity_(capacity) {
        // Guard: null buffer or insufficient capacity → immediate overflow state.
        // kHeaderReserve (32 bytes) is needed for BeginString + BodyLength prefix,
        // plus at least ~20 bytes for a minimal field + checksum trailer.
        if (buf == nullptr || capacity < kHeaderReserve) [[unlikely]] {
            log_invalid_construction(buf, capacity);
            overflow_ = true;
            return;
        }
        // Reserve space at the front for "8=FIX.4.4\x019=NNNNN\x01" (up to ~24 bytes).
        // We use a generous 32-byte reservation so body length up to 99999 fits.
        body_start_ = kHeaderReserve;
        pos_        = kHeaderReserve;
    }

    /// Append a string-valued field: tag=value\x01.
    /// The value must NOT contain SOH (0x01) bytes — SOH is the FIX field
    /// delimiter and embedding it would corrupt the message. Sets overflow
    /// flag if SOH is found.
    MessageBuilder& set(uint32_t t, std::string_view value) noexcept {
        if (overflow_) [[unlikely]] return *this;

        // Reject values containing SOH — they would corrupt field boundaries
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\x01') [[unlikely]] {
                log_soh_found(t, i);
                overflow_ = true;
                return *this;
            }
        }

        return set_trusted(t, value);
    }

    /// Append an integer-valued field.
    MessageBuilder& set_int(uint32_t t, int64_t value) noexcept {
        char tmp[24];
        size_t n = format_int(value, tmp);
        return set_trusted(t, std::string_view(tmp, n));
    }

    /// Append a field from a raw byte pointer + length.
    /// The data must NOT contain SOH (0x01) bytes, as the parser uses SOH
    /// as a field delimiter. Sets overflow flag if SOH is found (validated
    /// by set()).
    /// For true binary FIX fields (Data/RawData), a length-aware parser
    /// would be required.
    MessageBuilder& set_raw(uint32_t t, const uint8_t* data, size_t len) noexcept {
        if (data == nullptr || len == 0)
            return set_trusted(t, std::string_view{});
        // SOH validation: set_raw() accepts arbitrary bytes, must validate
        for (size_t i = 0; i < len; ++i) {
            if (data[i] == 0x01) [[unlikely]] {
                log_soh_found(t, i);
                overflow_ = true;
                return *this;
            }
        }
        return set_trusted(t, std::string_view(reinterpret_cast<const char*>(data), len));
    }

    /// Append a FIX boolean field (Y/N).
    /// Useful for PossDupFlag, PossResend, ResetSeqNumFlag, GapFillFlag, etc.
    MessageBuilder& set_bool(uint32_t t, bool value) noexcept {
        return set_trusted(t, value ? std::string_view("Y", 1) : std::string_view("N", 1));
    }

    /// Timestamp sub-second precision levels.
    enum class TimestampPrecision : uint8_t {
        kSeconds = 0,       ///< "YYYYMMDD-HH:MM:SS" (17 chars)
        kMilliseconds = 3,  ///< "YYYYMMDD-HH:MM:SS.sss" (21 chars)
        kMicroseconds = 6,  ///< "YYYYMMDD-HH:MM:SS.ssssss" (24 chars)
        kNanoseconds = 9,   ///< "YYYYMMDD-HH:MM:SS.sssssssss" (27 chars)
    };

    /// Append a UTCTimestamp field with configurable sub-second precision.
    ///
    /// FIX 4.4+ UTCTimestamp format. Default is millisecond precision for
    /// backward compatibility. Use kMicroseconds or kNanoseconds for modern
    /// FIX venues (CME, ICE, etc.) that require higher precision.
    ///
    /// @param t        Tag number (e.g. tag::SendingTime, tag::TransactTime)
    /// @param epoch_ns Nanoseconds since Unix epoch (1970-01-01 00:00:00 UTC)
    /// @param prec     Sub-second precision (default: milliseconds)
    MessageBuilder& set_timestamp(uint32_t t, uint64_t epoch_ns,
                                  TimestampPrecision prec = TimestampPrecision::kMilliseconds) noexcept {
        // Convert nanoseconds to components via integer arithmetic only.
        // No gmtime, no floating point — keeps the zero-allocation guarantee.
        uint64_t epoch_sec = epoch_ns / 1'000'000'000ULL;

        // Note: the maximum representable date in uint64_t nanoseconds is ~2554-07-21,
        // well within the YYYYMMDD range (year 9999), so no overflow guard is needed.

        // Days since epoch and time-of-day
        uint32_t day_sec   = static_cast<uint32_t>(epoch_sec % 86400);
        uint32_t hour      = day_sec / 3600;
        uint32_t minute    = (day_sec % 3600) / 60;
        uint32_t second    = day_sec % 60;

        // Civil date from day count (algorithm from Howard Hinnant)
        int64_t  z  = static_cast<int64_t>(epoch_sec / 86400) + 719468;
        int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
        uint64_t doe = static_cast<uint64_t>(z - era * 146097);
        uint64_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
        int64_t  y   = static_cast<int64_t>(yoe) + era * 400;
        uint64_t doy = doe - (365*yoe + yoe/4 - yoe/100);
        uint64_t mp  = (5*doy + 2) / 153;
        uint32_t day = static_cast<uint32_t>(doy - (153*mp + 2)/5 + 1);
        uint32_t mon = static_cast<uint32_t>(mp < 10 ? mp + 3 : mp - 9);
        if (mon <= 2) ++y;
        uint32_t year = static_cast<uint32_t>(y);

        // Format base: "YYYYMMDD-HH:MM:SS" (17 chars)
        char tmp[32];
        tmp[0]  = static_cast<char>('0' + (year / 1000) % 10);
        tmp[1]  = static_cast<char>('0' + (year / 100) % 10);
        tmp[2]  = static_cast<char>('0' + (year / 10) % 10);
        tmp[3]  = static_cast<char>('0' + year % 10);
        tmp[4]  = static_cast<char>('0' + mon / 10);
        tmp[5]  = static_cast<char>('0' + mon % 10);
        tmp[6]  = static_cast<char>('0' + day / 10);
        tmp[7]  = static_cast<char>('0' + day % 10);
        tmp[8]  = '-';
        tmp[9]  = static_cast<char>('0' + hour / 10);
        tmp[10] = static_cast<char>('0' + hour % 10);
        tmp[11] = ':';
        tmp[12] = static_cast<char>('0' + minute / 10);
        tmp[13] = static_cast<char>('0' + minute % 10);
        tmp[14] = ':';
        tmp[15] = static_cast<char>('0' + second / 10);
        tmp[16] = static_cast<char>('0' + second % 10);

        size_t total_len = 17;
        int frac_digits = static_cast<int>(prec);

        if (frac_digits > 0) {
            tmp[17] = '.';
            // Extract the sub-second fraction at the requested precision
            uint64_t sub_ns = epoch_ns % 1'000'000'000ULL;
            uint64_t frac_val;
            if (frac_digits == 3) frac_val = sub_ns / 1'000'000ULL;      // ms
            else if (frac_digits == 6) frac_val = sub_ns / 1'000ULL;     // us
            else frac_val = sub_ns;                                        // ns

            // Write fractional digits right-to-left with leading zeros
            for (int i = frac_digits - 1; i >= 0; --i) {
                tmp[18 + i] = static_cast<char>('0' + frac_val % 10);
                frac_val /= 10;
            }
            total_len = 18 + static_cast<size_t>(frac_digits);
        }

        return set_trusted(t, std::string_view(tmp, total_len));
    }

    /// Append a double-valued field with fixed-point precision.
    /// Sets overflow flag if value is NaN or Infinity (not representable in FIX).
    /// Precision is clamped to [0, 15] to prevent buffer overrun in format_double().
    MessageBuilder& set_double(uint32_t t, double value, int precision = 2) noexcept {
        if (!std::isfinite(value)) [[unlikely]] {
            log_non_finite(t);
            overflow_ = true;
            return *this;
        }
        if (precision < 0) precision = 0;
        if (precision > 15) precision = 15;
        char tmp[32];
        size_t n = format_double(value, tmp, precision);
        return set_trusted(t, std::string_view(tmp, n));
    }

    /// Finalize the message: prepend BeginString + BodyLength, append CheckSum.
    ///
    /// @param begin_string  FIX version string (default "FIX.4.4")
    /// @return Total message length in bytes, or 0 if buffer was too small
    size_t finish(std::string_view begin_string = "FIX.4.4") noexcept {
        if (overflow_) return 0;

        // Body is buf_[body_start_ .. pos_), body_length = pos_ - body_start_
        size_t body_length = pos_ - body_start_;

        // Format the header: "8=FIX.4.4\x019=NNNNN\x01"
        // Build it in a temp buffer, then check if it fits in the reserved space.
        char header[48];
        size_t hpos = 0;

        // "8="
        header[hpos++] = '8';
        header[hpos++] = '=';
        std::memcpy(header + hpos, begin_string.data(), begin_string.size());
        hpos += begin_string.size();
        header[hpos++] = '\x01';

        // "9=NNN\x01"
        header[hpos++] = '9';
        header[hpos++] = '=';
        hpos += format_uint(body_length, header + hpos);
        header[hpos++] = '\x01';

        if (hpos > body_start_) [[unlikely]] {
            log_header_overflow(hpos, body_start_);
            overflow_ = true;
            return 0;
        }

        // Place header right before body
        size_t header_offset = body_start_ - hpos;
        std::memcpy(buf_ + header_offset, header, hpos);

        // Compute checksum over header + body
        uint32_t sum = 0;
        for (size_t i = header_offset; i < pos_; ++i) {
            sum += buf_[i];
        }
        uint8_t cs = static_cast<uint8_t>(sum & 0xFF);

        // Append "10=XXX\x01" (exactly 7 bytes)
        if (pos_ + kTrailerLen > capacity_) [[unlikely]] {
            log_trailer_overflow(pos_, capacity_);
            overflow_ = true;
            return 0;
        }

        buf_[pos_++] = '1';
        buf_[pos_++] = '0';
        buf_[pos_++] = '=';
        buf_[pos_++] = static_cast<uint8_t>('0' + (cs / 100) % 10);
        buf_[pos_++] = static_cast<uint8_t>('0' + (cs / 10) % 10);
        buf_[pos_++] = static_cast<uint8_t>('0' + cs % 10);
        buf_[pos_++] = '\x01';

        // Shift everything so the message starts at buf_[0]
        size_t total = pos_ - header_offset;
        if (header_offset > 0) {
            std::memmove(buf_, buf_ + header_offset, total);
        }

        total_len_ = total;
        return total;
    }

    /// Reset the builder for reuse with the same buffer.
    /// Avoids re-constructing when building many messages into the same buffer.
    void reset() noexcept {
        body_start_ = kHeaderReserve;
        pos_        = kHeaderReserve;
        total_len_  = 0;
        field_count_ = 0;
        overflow_   = false;
    }

    /// Append a single-character enum field (Side, OrdType, ExecType, etc.).
    ///
    /// Convenience over set(tag, string_view(&c, 1)) — avoids constructing
    /// a string_view from a char variable.
    MessageBuilder& set_char(uint32_t t, char value) noexcept {
        return set(t, std::string_view(&value, 1));
    }

    // -----------------------------------------------------------------------
    // Unique-tag setters — reject duplicate tags (FIX spec compliance)
    // -----------------------------------------------------------------------

    /// Like set(), but rejects the field if the tag already exists.
    /// Sets the overflow flag and logs a warning on duplicate.
    /// Use for non-repeating-group fields where duplicates violate the FIX spec.
    MessageBuilder& set_unique(uint32_t t, std::string_view value) noexcept {
        if (has_tag(t)) [[unlikely]] {
            log_duplicate_tag(t);
            overflow_ = true;
            return *this;
        }
        return set(t, value);
    }

    /// Like set_int(), but rejects duplicates.
    MessageBuilder& set_int_unique(uint32_t t, int64_t value) noexcept {
        if (has_tag(t)) [[unlikely]] {
            log_duplicate_tag(t);
            overflow_ = true;
            return *this;
        }
        return set_int(t, value);
    }

    /// Like set_double(), but rejects duplicates.
    MessageBuilder& set_double_unique(uint32_t t, double value, int precision = 2) noexcept {
        if (has_tag(t)) [[unlikely]] {
            log_duplicate_tag(t);
            overflow_ = true;
            return *this;
        }
        return set_double(t, value, precision);
    }

    /// Check whether the builder has overflowed the buffer.
    /// Useful for detecting overflow mid-build without waiting for finish().
    [[nodiscard]] bool has_overflow() const noexcept { return overflow_; }

    /// Check whether a tag has already been written to the message body.
    /// Scans the written fields by parsing "tag=value\x01" boundaries.
    /// Useful for preventing duplicate tags, which violate the FIX spec
    /// (outside of repeating groups). O(field_count) scan.
    [[nodiscard]] bool has_tag(uint32_t t) const noexcept {
        if (overflow_ || pos_ <= body_start_) return false;

        // Tag as ASCII digits for comparison
        char tag_str[12];
        size_t tag_len = format_uint(t, tag_str);

        // Scan body: each field is "tag=value\x01"
        size_t i = body_start_;
        while (i < pos_) {
            // Find '=' to delimit the tag number
            size_t eq = i;
            while (eq < pos_ && buf_[eq] != '=') ++eq;
            if (eq >= pos_) break;

            // Compare tag
            size_t this_tag_len = eq - i;
            if (this_tag_len == tag_len &&
                std::memcmp(buf_ + i, tag_str, tag_len) == 0) {
                return true;
            }

            // Skip past value and SOH
            size_t soh = eq + 1;
            while (soh < pos_ && buf_[soh] != '\x01') ++soh;
            i = soh + 1;
        }
        return false;
    }

    /// Number of body fields appended so far (excludes BeginString, BodyLength, CheckSum).
    [[nodiscard]] size_t field_count() const noexcept { return field_count_; }

    /// Number of body bytes written so far (excluding header reservation).
    /// Valid during building, before finish(). After finish(), use size().
    [[nodiscard]] size_t bytes_used() const noexcept {
        return pos_ - body_start_;
    }

    /// Approximate remaining buffer capacity for body fields.
    /// Accounts for the 7-byte CheckSum trailer that finish() will append.
    /// Returns 0 if already overflowed or no space remains.
    [[nodiscard]] size_t remaining_capacity() const noexcept {
        if (overflow_) return 0;
        if (pos_ + kTrailerLen >= capacity_) return 0;
        return capacity_ - pos_ - kTrailerLen;
    }

    /// Pointer to the finalized message data (valid only after finish()).
    [[nodiscard]] const uint8_t* data() const noexcept { return buf_; }

    /// Total message length (valid only after finish()).
    [[nodiscard]] size_t size() const noexcept { return total_len_; }

    /// View of the finalized message as a span (valid only after finish()).
    [[nodiscard]] std::span<const uint8_t> as_span() const noexcept {
        return {buf_, total_len_};
    }

    /// View of the finalized message as a string_view (valid only after finish()).
    /// Useful for logging or text-based transport.
    [[nodiscard]] std::string_view as_string_view() const noexcept {
        return {reinterpret_cast<const char*>(buf_), total_len_};
    }

private:
    /// Write a tag=value\x01 field without SOH validation.
    /// Used by internal formatting methods (set_int, set_bool, set_double,
    /// set_timestamp) that produce known-safe ASCII output.
    MessageBuilder& set_trusted(uint32_t t, std::string_view value) noexcept {
        if (overflow_) [[unlikely]] return *this;

        size_t tag_len = write_uint(t, pos_);
        size_t needed = tag_len + 1 + value.size() + 1;
        if (pos_ + needed > capacity_) [[unlikely]] {
            log_overflow(t, needed, capacity_ - pos_);
            overflow_ = true;
            return *this;
        }

        pos_ += tag_len;
        buf_[pos_++] = '=';
        std::memcpy(buf_ + pos_, value.data(), value.size());
        pos_ += value.size();
        buf_[pos_++] = '\x01';
        ++field_count_;
        return *this;
    }

    static constexpr size_t kHeaderReserve = 32;
    static constexpr size_t kTrailerLen = 7;  // "10=XXX\x01"
    // "8=" + version (≤15) + SOH + "9=" + body_len (≤7 digits) + SOH = ≤28
    static_assert(kHeaderReserve >= 28,
                  "kHeaderReserve must fit BeginString + BodyLength header");

    uint8_t* buf_;
    size_t   capacity_;
    size_t   body_start_ = 0;
    size_t   pos_        = 0;
    size_t   total_len_  = 0;
    size_t   field_count_ = 0;
    bool     overflow_   = false;

    /// Write an unsigned integer as ASCII digits into buf_ at offset, returning digit count.
    /// Does NOT advance pos_.
    size_t write_uint(uint64_t val, size_t offset) const noexcept {
        char tmp[20];
        size_t n = format_uint(val, tmp);
        if (offset + n <= capacity_) {
            std::memcpy(buf_ + offset, tmp, n);
        }
        return n;
    }

    /// Format unsigned integer to ASCII. Returns number of digits written.
    static size_t format_uint(uint64_t val, char* out) noexcept {
        if (val == 0) { out[0] = '0'; return 1; }
        char tmp[20];
        int i = 0;
        while (val > 0) {
            tmp[i++] = static_cast<char>('0' + val % 10);
            val /= 10;
        }
        for (int j = 0; j < i; ++j) {
            out[j] = tmp[i - 1 - j];
        }
        return static_cast<size_t>(i);
    }

    /// Format signed integer to ASCII. Returns number of chars written.
    /// Handles INT64_MIN correctly by casting to uint64_t before negation.
    static size_t format_int(int64_t val, char* out) noexcept {
        size_t off = 0;
        uint64_t uval;
        if (val < 0) {
            out[off++] = '-';
            // Cast to unsigned BEFORE negation to avoid UB on INT64_MIN.
            // -static_cast<uint64_t>(val) is well-defined modular arithmetic.
            uval = -static_cast<uint64_t>(val);
        } else {
            uval = static_cast<uint64_t>(val);
        }
        off += format_uint(uval, out + off);
        return off;
    }

    /// Format double with fixed precision. Returns number of chars written.
    static size_t format_double(double val, char* out, int precision) noexcept {
        size_t off = 0;
        size_t sign_len = 0;
        if (val < 0) {
            out[off++] = '-';
            sign_len = 1;
            val = -val;
        }

        // Integer part
        auto int_part = static_cast<uint64_t>(val);
        off += format_uint(int_part, out + off);

        if (precision > 0) {
            out[off++] = '.';

            // Fractional part: multiply by 10^precision and round
            double frac = val - static_cast<double>(int_part);
            double multiplier = 1.0;
            for (int i = 0; i < precision; ++i) multiplier *= 10.0;
            auto frac_int = static_cast<uint64_t>(frac * multiplier + 0.5);

            // Handle rounding carry: e.g. 0.995 at precision=2 →
            // frac_int=100 which has 3 digits but only 2 slots.
            // Carry into integer part and reset fractional to zero.
            uint64_t frac_limit = static_cast<uint64_t>(multiplier);
            if (frac_int >= frac_limit) {
                // Rewrite: skip sign, reformat integer+1, re-add '.'
                off = sign_len;
                off += format_uint(int_part + 1, out + off);
                out[off++] = '.';
                frac_int = 0;
            }

            // Write with leading zeros
            char frac_buf[20];
            size_t fn = format_uint(frac_int, frac_buf);
            // Pad with leading zeros if needed
            for (size_t i = fn; i < static_cast<size_t>(precision); ++i) {
                out[off++] = '0';
            }
            std::memcpy(out + off, frac_buf, fn);
            off += fn;
        }

        return off;
    }

    // -- Cold logging helpers (noinline to keep hot paths small) ---------------

    [[gnu::noinline, gnu::cold]]
    static void log_invalid_construction(const uint8_t* buf, size_t capacity) noexcept {
        SPDLOG_LOGGER_WARN(detail::fix_builder_logger(),
            "FIX builder: invalid construction, buf={}, capacity={} (min {})",
            fmt::ptr(buf), capacity, kHeaderReserve);
    }

    [[gnu::noinline, gnu::cold]]
    static void log_overflow(uint32_t tag, size_t needed, size_t remaining) noexcept {
        SPDLOG_LOGGER_WARN(detail::fix_builder_logger(),
            "FIX builder: buffer overflow at tag={}, needed={} bytes, remaining={}",
            tag, needed, remaining);
    }

    [[gnu::noinline, gnu::cold]]
    static void log_soh_found(uint32_t tag, size_t offset) noexcept {
        SPDLOG_LOGGER_WARN(detail::fix_builder_logger(),
            "FIX builder: SOH byte found in value at offset {}, tag={} — "
            "SOH is the FIX field delimiter and would corrupt the message",
            offset, tag);
    }

    [[gnu::noinline, gnu::cold]]
    static void log_non_finite(uint32_t tag) noexcept {
        SPDLOG_LOGGER_WARN(detail::fix_builder_logger(),
            "FIX builder: non-finite double for tag={}", tag);
    }

    [[gnu::noinline, gnu::cold]]
    static void log_header_overflow(size_t hpos, size_t body_start) noexcept {
        SPDLOG_LOGGER_WARN(detail::fix_builder_logger(),
            "FIX builder: header too large ({} bytes) for reserved space ({})",
            hpos, body_start);
    }

    [[gnu::noinline, gnu::cold]]
    static void log_trailer_overflow(size_t pos, size_t capacity) noexcept {
        SPDLOG_LOGGER_WARN(detail::fix_builder_logger(),
            "FIX builder: no room for checksum trailer, pos={}, capacity={}",
            pos, capacity);
    }

    [[gnu::noinline, gnu::cold]]
    static void log_duplicate_tag(uint32_t tag) noexcept {
        SPDLOG_LOGGER_WARN(detail::fix_builder_logger(),
            "FIX builder: duplicate tag={} rejected by set_unique()", tag);
    }
};

} // namespace eph::fix
