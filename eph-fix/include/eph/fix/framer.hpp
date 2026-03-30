#pragma once

/// @file framer.hpp
/// FIX message framer satisfying the eph::net::MessageFramer concept.
///
/// FIX messages are self-delimiting: they start with "8=FIX" and end with
/// "10=XXX\x01" (CheckSum tag). The framer scans for the CheckSum field
/// to find message boundaries in a byte stream.

#include <cstdint>
#include <cstring>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/fix/parser.hpp"
#include "eph/core/framer_concept.hpp"

namespace eph::fix {

namespace detail {
inline std::shared_ptr<spdlog::logger> fix_framer_logger() {
    static auto l = [] {
        auto lg = spdlog::get("fix.framer");
        if (!lg) lg = spdlog::stdout_color_mt("fix.framer");
        return lg;
    }();
    return l;
}
} // namespace detail

/// FIX protocol framer -- finds message boundaries by scanning for the
/// CheckSum tag ("10=XXX\x01") in the byte stream.
///
/// @tparam MaxBodyLength  Maximum allowed BodyLength value (default 1 MB).
///                        Override for venues with larger messages.
///
/// encode() is a pass-through: FIX messages produced by MessageBuilder
/// are already fully framed (BeginString + BodyLength + CheckSum).
///
/// decode() scans for a complete message by:
/// 1. Verifying the buffer starts with "8=" (BeginString)
/// 2. Parsing BodyLength from the "9=NNN\x01" field
/// 3. Computing the total message length and checking availability
template <size_t MaxBodyLength = kDefaultMaxBodyLength>
class BasicFixFramer {
public:
    static constexpr size_t max_overhead() noexcept { return 0; }

    /// Pass-through encode: FIX messages are already self-framed.
    size_t encode(uint8_t* out, const uint8_t* data, size_t len,
                  uint8_t /*msg_type*/) noexcept {
        std::memcpy(out, data, len);
        return len;
    }

    /// Decode: find a complete FIX message in the buffer.
    ///
    /// Scans for "8=...\x019=NNN\x01" at the start, computes total message
    /// length from BodyLength, and verifies that "10=XXX\x01" is present.
    /// The msg_type byte in DecodedFrame is set to the FIX MsgType char
    /// if tag 35 is found in the body, otherwise 0.
    [[nodiscard]] std::expected<eph::net::DecodedFrame, eph::net::FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        if (len < 2) return std::unexpected(eph::net::FrameError::kIncomplete);

        const char* msg = reinterpret_cast<const char*>(data);
        const char* end = msg + len;

        // Must start with "8="
        if (msg[0] != '8' || msg[1] != '=') {
            SPDLOG_LOGGER_WARN(detail::fix_framer_logger(),
                "FIX frame missing BeginString: first bytes=0x{:02x}{:02x}, len={}",
                static_cast<uint8_t>(msg[0]), static_cast<uint8_t>(msg[1]), len);
            return std::unexpected(eph::net::FrameError::kInvalidFormat);
        }

        // Find end of BeginString field (SOH after "8=...")
        const char* p = msg + 2;
        while (p < end && *p != '\x01') ++p;
        if (p >= end) return std::unexpected(eph::net::FrameError::kIncomplete);
        ++p; // skip SOH

        // Must be "9=" (BodyLength)
        if (p + 2 > end) return std::unexpected(eph::net::FrameError::kIncomplete);
        if (p[0] != '9' || p[1] != '=') {
            SPDLOG_LOGGER_WARN(detail::fix_framer_logger(),
                "FIX frame missing BodyLength tag: expected '9=' at offset {}, len={}",
                static_cast<size_t>(p - msg), len);
            return std::unexpected(eph::net::FrameError::kInvalidFormat);
        }
        p += 2;

        // Parse body length digits with overflow protection.
        // Validate against kMaxBodyLength inside the loop to prevent
        // integer overflow on maliciously large digit strings.
        size_t body_length = 0;
        while (p < end && *p != '\x01') {
            char c = *p++;
            if (c < '0' || c > '9') {
                SPDLOG_LOGGER_WARN(detail::fix_framer_logger(),
                    "FIX frame BodyLength contains non-digit char=0x{:02x}", static_cast<uint8_t>(c));
                return std::unexpected(eph::net::FrameError::kInvalidFormat);
            }
            if (body_length > MaxBodyLength / 10) {
                SPDLOG_LOGGER_WARN(detail::fix_framer_logger(),
                    "FIX frame BodyLength overflow before multiply, body_length={}", body_length);
                return std::unexpected(eph::net::FrameError::kInvalidFormat);
            }
            body_length = body_length * 10 + static_cast<size_t>(c - '0');
            if (body_length > MaxBodyLength) {
                SPDLOG_LOGGER_WARN(detail::fix_framer_logger(),
                    "FIX frame BodyLength exceeds maximum allowed {} bytes",
                    MaxBodyLength);
                return std::unexpected(eph::net::FrameError::kInvalidFormat);
            }
        }
        if (p >= end) return std::unexpected(eph::net::FrameError::kIncomplete);
        ++p; // skip SOH after body length value

        // Body starts here. Total = header + body + checksum ("10=XXX\x01" = 7 bytes)
        size_t header_len = static_cast<size_t>(p - msg);

        // Overflow-safe bounds check: verify len >= header_len + body_length + 7
        // without computing a sum that could wrap.
        if (body_length > len || header_len > len - body_length || header_len + body_length > len - 7)
            return std::unexpected(eph::net::FrameError::kIncomplete);

        size_t total = header_len + body_length + 7;

        // Verify CheckSum field presence and value
        const char* cs = msg + header_len + body_length;
        if (cs[0] != '1' || cs[1] != '0' || cs[2] != '=') {
            SPDLOG_LOGGER_WARN(detail::fix_framer_logger(),
                "FIX frame CheckSum field malformed: expected '10=' at offset {}, body_length={}",
                header_len + body_length, body_length);
            return std::unexpected(eph::net::FrameError::kInvalidFormat);
        }
        if (cs[6] != '\x01') {
            SPDLOG_LOGGER_WARN(detail::fix_framer_logger(),
                "FIX frame CheckSum field missing trailing SOH, body_length={}", body_length);
            return std::unexpected(eph::net::FrameError::kInvalidFormat);
        }

        // Parse declared checksum (3 ASCII digits)
        uint32_t declared_cs = 0;
        for (size_t i = 3; i < 6; ++i) {
            char c = cs[i];
            if (c < '0' || c > '9') {
                SPDLOG_LOGGER_WARN(detail::fix_framer_logger(),
                    "FIX frame CheckSum value contains non-digit char=0x{:02x}", static_cast<uint8_t>(c));
                return std::unexpected(eph::net::FrameError::kInvalidFormat);
            }
            declared_cs = declared_cs * 10 + static_cast<uint32_t>(c - '0');
        }

        // Validate checksum (reuse compute_checksum from parser.hpp)
        size_t cs_body_len = header_len + body_length;
        uint8_t computed = compute_checksum(data, cs_body_len);
        if (computed != static_cast<uint8_t>(declared_cs)) {
            SPDLOG_LOGGER_WARN(detail::fix_framer_logger(),
                "FIX frame checksum mismatch: declared={}, computed={}, body_length={}",
                declared_cs, computed, body_length);
            return std::unexpected(eph::net::FrameError::kInvalidFormat);
        }

        // Try to extract MsgType (tag 35) from the body for the msg_type hint.
        // Scan body fields for "35=X\x01" -- typically the first body field.
        uint8_t fix_msg_type = 0;
        const char* bp = p;
        const char* body_end = p + body_length;
        while (bp < body_end) {
            // Parse tag number
            uint32_t tag = 0;
            while (bp < body_end && *bp != '=') {
                char c = *bp++;
                if (c >= '0' && c <= '9') tag = tag * 10 + static_cast<uint32_t>(c - '0');
            }
            if (bp >= body_end) break;
            ++bp; // skip '='

            const char* val_start = bp;
            while (bp < body_end && *bp != '\x01') ++bp;
            if (bp >= body_end) break;

            if (tag == 35 && bp > val_start) {
                fix_msg_type = static_cast<uint8_t>(*val_start);
                break;
            }
            ++bp; // skip SOH
        }

        return eph::net::DecodedFrame{
            .payload     = data,
            .payload_len = total,
            .msg_type    = fix_msg_type,
            .is_control  = false,
            .total_len   = total,
        };
    }
};

/// Default FixFramer with 1 MB max body length (backward compatible).
using FixFramer = BasicFixFramer<>;

static_assert(eph::net::MessageFramer<FixFramer>,
    "FixFramer must satisfy MessageFramer concept");

} // namespace eph::fix
