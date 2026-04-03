#pragma once

/// @file base64.hpp
/// Minimal base64 encoding for internal use (WebSocket keys, proxy auth).
///
/// Standalone (no OpenSSL dependency) so lightweight modules can use it
/// without pulling in TLS headers. For production base64 with decode
/// support, prefer EVP_EncodeBlock/EVP_DecodeBlock from aws-lc.

#include <cstdint>
#include <string>

namespace eph::core::detail {

/// @brief Encode raw bytes as base64 (RFC 4648).
///
/// Uses a pure-C++ implementation to avoid OpenSSL header dependencies.
/// Suitable for small payloads (WebSocket keys, proxy auth tokens).
///
/// @param data  Input bytes to encode.
/// @param len   Number of bytes to encode.
/// @return Base64-encoded string with '=' padding.
[[nodiscard]] inline std::string base64_encode(const uint8_t* data, size_t len) {
    static constexpr char kChars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) triple |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) triple |= static_cast<uint32_t>(data[i + 2]);

        result += kChars[(triple >> 18) & 0x3F];
        result += kChars[(triple >> 12) & 0x3F];
        result += (i + 1 < len) ? kChars[(triple >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? kChars[triple & 0x3F] : '=';
    }
    return result;
}

/// @brief Encode a string as base64 (convenience overload).
[[nodiscard]] inline std::string base64_encode(const std::string& input) {
    return base64_encode(reinterpret_cast<const uint8_t*>(input.data()), input.size());
}

} // namespace eph::core::detail
