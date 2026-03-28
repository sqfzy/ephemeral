#pragma once

/// @file hmac.hpp
/// HMAC-SHA256 signing utility for authenticated crypto exchange REST APIs.
///
/// Provides HMAC-SHA256 computation with hex and base64 output encoding.
/// Covers Binance/Bybit (hex signatures) and OKX (base64 signatures).
/// Uses aws-lc (OpenSSL-compatible) as the cryptographic backend.

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline spdlog::logger* hmac_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.hmac");
        if (!lg) lg = spdlog::stdout_color_mt("net.hmac");
        return lg;
    }();
    return l.get();
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Hex encoding
// ─────────────────────────────────────────────────────────────────────────────

/// Encode bytes as lowercase hex string.
[[nodiscard]] inline std::string to_hex(std::span<const uint8_t> bytes) noexcept {
    static constexpr char kHexChars[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        result += kHexChars[b >> 4];
        result += kHexChars[b & 0x0F];
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Base64 encoding
// ─────────────────────────────────────────────────────────────────────────────

/// Encode bytes as base64 string (for OKX API which uses base64 signatures).
/// Uses EVP_EncodeBlock from aws-lc for correctness.
[[nodiscard]] inline std::expected<std::string, std::string>
to_base64(std::span<const uint8_t> bytes) noexcept {
    if (bytes.empty()) {
        return std::string{};
    }

    // EVP_EncodeBlock output size: 4 * ceil(n/3) + 1 (for null terminator)
    const size_t out_len = ((bytes.size() + 2) / 3) * 4;
    std::string result(out_len, '\0');

    const int written = EVP_EncodeBlock(
        reinterpret_cast<uint8_t*>(result.data()),
        bytes.data(),
        static_cast<int>(bytes.size()));

    if (written < 0) {
        SPDLOG_LOGGER_ERROR(detail::hmac_logger(),
                            "EVP_EncodeBlock failed for {} bytes", bytes.size());
        return std::unexpected("EVP_EncodeBlock failed");
    }

    result.resize(static_cast<size_t>(written));
    SPDLOG_LOGGER_TRACE(detail::hmac_logger(),
                        "base64 encoded {} bytes -> {} chars", bytes.size(), written);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// HMAC-SHA256
// ─────────────────────────────────────────────────────────────────────────────

/// Compute HMAC-SHA256 of a message using a secret key.
/// Returns the raw 32-byte digest.
[[nodiscard]] inline std::expected<std::array<uint8_t, 32>, std::string>
hmac_sha256(std::string_view key, std::string_view message) noexcept {
    std::array<uint8_t, 32> digest{};
    unsigned int digest_len = 0;

    SPDLOG_LOGGER_DEBUG(detail::hmac_logger(),
                        "computing HMAC-SHA256: key_len={}, msg_len={}",
                        key.size(), message.size());

    const uint8_t* result = HMAC(
        EVP_sha256(),
        key.data(), static_cast<int>(key.size()),
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size(),
        digest.data(),
        &digest_len);

    if (result == nullptr) {
        SPDLOG_LOGGER_ERROR(detail::hmac_logger(),
                            "HMAC() returned null: key_len={}, msg_len={}",
                            key.size(), message.size());
        return std::unexpected("HMAC computation failed");
    }

    if (digest_len != 32) {
        // Should never happen with SHA-256, but defensive check.
        SPDLOG_LOGGER_ERROR(detail::hmac_logger(),
                            "HMAC produced unexpected digest length: {} (expected 32)",
                            digest_len);
        return std::unexpected("HMAC produced unexpected digest length");
    }

    SPDLOG_LOGGER_TRACE(detail::hmac_logger(),
                        "HMAC-SHA256 computed successfully: digest_len={}", digest_len);
    return digest;
}

/// Compute HMAC-SHA256 and return as lowercase hex string.
/// This is the format used by Binance and Bybit for request signatures.
[[nodiscard]] inline std::expected<std::string, std::string>
hmac_sha256_hex(std::string_view key, std::string_view message) noexcept {
    auto digest = hmac_sha256(key, message);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    return to_hex(*digest);
}

} // namespace eph::net
