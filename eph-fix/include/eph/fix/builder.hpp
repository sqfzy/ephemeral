#pragma once

/// @file builder.hpp
/// Zero-copy FIX message builder.
///
/// Writes tag=value\x01 fields directly into a user-provided buffer.
/// On finish(), prepends BeginString + BodyLength and appends CheckSum.
/// No heap allocations.

#include <cstdint>
#include <cstring>
#include <string_view>

#include "eph/fix/tags.hpp"

namespace eph::fix {

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
        // Reserve space at the front for "8=FIX.4.4\x019=NNNNN\x01" (up to ~24 bytes).
        // We use a generous 32-byte reservation so body length up to 99999 fits.
        body_start_ = kHeaderReserve;
        pos_        = kHeaderReserve;
    }

    /// Append a string-valued field: tag=value\x01.
    MessageBuilder& set(uint32_t t, std::string_view value) noexcept {
        if (overflow_) return *this;

        // Write "tag=value\x01"
        size_t tag_len = write_uint(t, pos_);
        size_t needed = tag_len + 1 + value.size() + 1; // tag + '=' + value + SOH
        if (pos_ + needed > capacity_) { overflow_ = true; return *this; }

        pos_ += tag_len;
        buf_[pos_++] = '=';
        std::memcpy(buf_ + pos_, value.data(), value.size());
        pos_ += value.size();
        buf_[pos_++] = '\x01';
        return *this;
    }

    /// Append an integer-valued field.
    MessageBuilder& set_int(uint32_t t, int64_t value) noexcept {
        char tmp[24];
        size_t n = format_int(value, tmp);
        return set(t, std::string_view(tmp, n));
    }

    /// Append a raw binary field: tag=<raw bytes>\x01.
    /// Useful for FIX Data/RawData fields (tags 95/96) where the value may
    /// contain SOH or other non-printable bytes.
    MessageBuilder& set_raw(uint32_t t, const uint8_t* data, size_t len) noexcept {
        return set(t, std::string_view(reinterpret_cast<const char*>(data), len));
    }

    /// Append a double-valued field with fixed-point precision.
    MessageBuilder& set_double(uint32_t t, double value, int precision = 2) noexcept {
        char tmp[32];
        size_t n = format_double(value, tmp, precision);
        return set(t, std::string_view(tmp, n));
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

        if (hpos > body_start_) {
            // Header doesn't fit in reserved space
            overflow_ = true;
            return 0;
        }

        // Place header right before body
        size_t header_offset = body_start_ - hpos;
        std::memcpy(buf_ + header_offset, header, hpos);

        // Compute checksum over header + body
        size_t msg_before_cs = pos_ - header_offset;
        uint32_t sum = 0;
        for (size_t i = header_offset; i < pos_; ++i) {
            sum += buf_[i];
        }
        uint8_t cs = static_cast<uint8_t>(sum & 0xFF);

        // Append "10=XXX\x01" (exactly 7 bytes)
        if (pos_ + 7 > capacity_) { overflow_ = true; return 0; }

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
        overflow_   = false;
    }

    /// Pointer to the finalized message data (valid only after finish()).
    [[nodiscard]] const uint8_t* data() const noexcept { return buf_; }

    /// Total message length (valid only after finish()).
    [[nodiscard]] size_t size() const noexcept { return total_len_; }

private:
    static constexpr size_t kHeaderReserve = 32;

    uint8_t* buf_;
    size_t   capacity_;
    size_t   body_start_ = 0;
    size_t   pos_        = 0;
    size_t   total_len_  = 0;
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
        if (val < 0) {
            out[off++] = '-';
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

            // Write with leading zeros
            char frac_buf[20];
            size_t fn = format_uint(frac_int, frac_buf);
            // Pad with leading zeros if needed
            for (int i = 0; i < precision - static_cast<int>(fn); ++i) {
                out[off++] = '0';
            }
            std::memcpy(out + off, frac_buf, fn);
            off += fn;
        }

        return off;
    }
};

} // namespace eph::fix
