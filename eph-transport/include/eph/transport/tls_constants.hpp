#pragma once

/// @file tls_constants.hpp
/// TLS record layer constants — shared by TlsEncryptor, TlsDecryptor, and
/// TlsRecordCrypto. Extracted to avoid circular include dependencies.

#include <bit>
#include <cstdint>
#include <cstring>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/transport/tls_session.hpp"

namespace eph::net {

namespace detail {
inline const std::shared_ptr<spdlog::logger>& tls_record_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.tls_record");
        if (!lg) lg = spdlog::stdout_color_mt("net.tls_record");
        return lg;
    }();
    return l;
}
} // namespace detail

namespace tls_record {

inline constexpr uint8_t  kContentTypeAppData = 0x17;
inline constexpr uint16_t kLegacyVersion      = 0x0303;
inline constexpr uint16_t kRecordHeaderLen     = 5;
inline constexpr uint16_t kAuthTagLen          = 16;

inline constexpr uint64_t kMaxSequenceNumber = (1ULL << 24);
inline constexpr uint64_t kSequenceWarnThreshold = kMaxSequenceNumber * 9 / 10;
inline constexpr uint64_t kSequenceReconnectThreshold = kMaxSequenceNumber * 95 / 100;

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

inline void write_record_header(uint8_t* dst, uint8_t content_type,
                                 uint16_t payload_len) noexcept {
    dst[0] = content_type;
    dst[1] = static_cast<uint8_t>(kLegacyVersion >> 8);
    dst[2] = static_cast<uint8_t>(kLegacyVersion & 0xFF);
    dst[3] = static_cast<uint8_t>(payload_len >> 8);
    dst[4] = static_cast<uint8_t>(payload_len & 0xFF);
}

inline bool parse_record_header(const uint8_t* src,
                                 uint8_t& content_type,
                                 uint16_t& payload_len) noexcept {
    content_type = src[0];
    payload_len = static_cast<uint16_t>((src[3] << 8) | src[4]);

    if (src[1] != 0x03 || src[2] != 0x03) {
        SPDLOG_LOGGER_DEBUG(detail::tls_record_logger(),
            "TLS record header: unexpected version bytes 0x{:02X}{:02X}",
            src[1], src[2]);
    }

    return content_type == kContentTypeAppData &&
           payload_len <= tls_const::kMaxRecordPayload + kAuthTagLen + 1;
}

} // namespace tls_record

} // namespace eph::net
