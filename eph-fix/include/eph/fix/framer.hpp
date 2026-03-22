#pragma once

/// @file framer.hpp
/// FIX message framer satisfying the eph::net::MessageFramer concept.
///
/// FIX messages are self-delimiting: they start with "8=FIX" and end with
/// "10=XXX\x01" (CheckSum tag). The framer scans for the CheckSum field
/// to find message boundaries in a byte stream.

#include <cstdint>
#include <cstring>

#include "eph/net/framer_concept.hpp"

namespace eph::fix {

/// FIX protocol framer -- finds message boundaries by scanning for the
/// CheckSum tag ("10=XXX\x01") in the byte stream.
///
/// encode() is a pass-through: FIX messages produced by MessageBuilder
/// are already fully framed (BeginString + BodyLength + CheckSum).
///
/// decode() scans for a complete message by:
/// 1. Verifying the buffer starts with "8=" (BeginString)
/// 2. Parsing BodyLength from the "9=NNN\x01" field
/// 3. Computing the total message length and checking availability
class FixFramer {
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
    static std::expected<eph::net::DecodedFrame, eph::net::FrameError>
    decode(const uint8_t* data, size_t len) noexcept {
        if (len < 2) return std::unexpected(eph::net::FrameError::kIncomplete);

        const char* msg = reinterpret_cast<const char*>(data);
        const char* end = msg + len;

        // Must start with "8="
        if (msg[0] != '8' || msg[1] != '=') {
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
            return std::unexpected(eph::net::FrameError::kInvalidFormat);
        }
        p += 2;

        // Parse body length digits
        size_t body_length = 0;
        while (p < end && *p != '\x01') {
            char c = *p++;
            if (c < '0' || c > '9') {
                return std::unexpected(eph::net::FrameError::kInvalidFormat);
            }
            body_length = body_length * 10 + static_cast<size_t>(c - '0');
        }
        if (p >= end) return std::unexpected(eph::net::FrameError::kIncomplete);
        ++p; // skip SOH after body length value

        // Body starts here. Total = header + body + checksum ("10=XXX\x01" = 7 bytes)
        size_t header_len = static_cast<size_t>(p - msg);
        size_t total = header_len + body_length + 7;

        if (len < total) return std::unexpected(eph::net::FrameError::kIncomplete);

        // Verify CheckSum field presence
        const char* cs = msg + header_len + body_length;
        if (cs[0] != '1' || cs[1] != '0' || cs[2] != '=') {
            return std::unexpected(eph::net::FrameError::kInvalidFormat);
        }
        if (cs[6] != '\x01') {
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

static_assert(eph::net::MessageFramer<FixFramer>,
    "FixFramer must satisfy MessageFramer concept");

} // namespace eph::fix
