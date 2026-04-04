#pragma once

/// @file tls_constants.hpp
/// TLS record layer constants — shared by TlsEncryptor, TlsDecryptor, and
/// TlsRecordCrypto. Extracted to avoid circular include dependencies.

#include <bit>
#include <cstdint>
#include <cstring>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/transport/detail/tls_session.hpp"

namespace eph::net {

namespace detail {
/// Lazily-initialized logger for TLS record-layer operations.
/// @return Pointer to the "net.tls_record" spdlog logger.
inline spdlog::logger* tls_record_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.tls_record");
        if (!lg) lg = spdlog::stdout_color_mt("net.tls_record");
        return lg;
    }();
    return l.get();
}
} // namespace detail

/// TLS record-layer constants and utility functions.
///
/// Implements the wire format for TLS 1.3 application data records:
/// header construction, header parsing, and nonce derivation for AES-GCM.
namespace tls_record {

inline constexpr uint8_t  kContentTypeAppData = 0x17; ///< TLS 1.3 application data content type
inline constexpr uint16_t kLegacyVersion      = 0x0303; ///< Legacy TLS version bytes (0x0303 = TLS 1.2, per TLS 1.3 spec)
inline constexpr uint16_t kRecordHeaderLen     = 5;   ///< TLS record header: content_type(1) + version(2) + length(2)
inline constexpr uint16_t kAuthTagLen          = 16;  ///< AES-GCM authentication tag length in bytes

/// Maximum TLS 1.3 record sequence number before forced reconnection.
///
/// Set to 2^24 (~16M records) per NIST SP 800-38D §8.3 recommendation
/// for AES-GCM with random nonces: the birthday bound at 2^32 gives a
/// ~2^-32 collision probability, and 2^24 provides a ~2^8 safety margin.
///
/// At HFT rates (~100K messages/sec), this triggers reconnection every
/// ~167 seconds. This is intentional: periodic key refresh limits the
/// blast radius of a compromised session key. If longer sessions are
/// needed, implement TLS 1.3 KeyUpdate (RFC 8446 §4.6.3) instead of
/// raising this limit.
inline constexpr uint64_t kMaxSequenceNumber = (1ULL << 24);
/// Threshold at which a log warning is emitted (90% of max).
inline constexpr uint64_t kSequenceWarnThreshold = kMaxSequenceNumber * 9 / 10;
/// Threshold at which a preemptive reconnect is triggered (95% of max).
inline constexpr uint64_t kSequenceReconnectThreshold = kMaxSequenceNumber * 95 / 100;

/// Build a per-record nonce by XOR-ing the IV with the big-endian sequence number.
///
/// Implements the TLS 1.3 nonce construction from RFC 8446 section 5.3:
/// the 64-bit sequence number is zero-padded to the nonce length and
/// XOR-ed with the static IV derived during key schedule.
///
/// @param[out] out  Output nonce buffer (must be tls_const::kTls13NonceLen bytes)
/// @param      iv   Static per-connection IV (tls_const::kTls13NonceLen bytes)
/// @param      seq  Record sequence number (monotonically increasing)
inline void build_nonce(uint8_t out[tls_const::kTls13NonceLen],
                        const uint8_t iv[tls_const::kTls13NonceLen],
                        uint64_t seq) noexcept {
    std::memcpy(out, iv, 4);
    uint64_t iv_tail;
    std::memcpy(&iv_tail, iv + 4, 8);
    uint64_t seq_be;
    if constexpr (std::endian::native == std::endian::little) {
        seq_be = std::byteswap(seq);
    } else {
        seq_be = seq;
    }
    uint64_t result = iv_tail ^ seq_be;
    std::memcpy(out + 4, &result, 8);
}

/// Write a 5-byte TLS record header to the output buffer.
///
/// @param[out] dst          Output buffer (must have at least kRecordHeaderLen bytes)
/// @param      content_type TLS content type byte (typically kContentTypeAppData)
/// @param      payload_len  Length of the encrypted payload (ciphertext + tag)
inline void write_record_header(uint8_t* dst, uint8_t content_type,
                                 uint16_t payload_len) noexcept {
    dst[0] = content_type;
    dst[1] = static_cast<uint8_t>(kLegacyVersion >> 8);
    dst[2] = static_cast<uint8_t>(kLegacyVersion & 0xFF);
    dst[3] = static_cast<uint8_t>(payload_len >> 8);
    dst[4] = static_cast<uint8_t>(payload_len & 0xFF);
}

/// Parse a TLS record header and validate content type and payload bounds.
///
/// @param      src          Input buffer (must have at least kRecordHeaderLen bytes)
/// @param[out] content_type Parsed content type byte
/// @param[out] payload_len  Parsed payload length
/// @return true if the record header describes a valid application data record
///         within the maximum TLS record size; false otherwise.
[[nodiscard]] inline bool parse_record_header(const uint8_t* src,
                                 uint8_t& content_type,
                                 uint16_t& payload_len) noexcept {
    content_type = src[0];
    payload_len = static_cast<uint16_t>((src[3] << 8) | src[4]);

    // RFC 8446 §5.1: legacy_record_version must be 0x0303 for TLS 1.3
    // records.  Non-conforming values indicate protocol violations or
    // middlebox interference — warn instead of silently accepting.
    if (src[1] != 0x03 || src[2] != 0x03) {
        SPDLOG_LOGGER_WARN(detail::tls_record_logger(),
            "TLS record header: unexpected version bytes 0x{:02X}{:02X} "
            "(expected 0x0303 per RFC 8446)",
            src[1], src[2]);
    }

    return content_type == kContentTypeAppData &&
           payload_len <= tls_const::kMaxRecordPayload + kAuthTagLen + 1;
}

} // namespace tls_record

} // namespace eph::net
