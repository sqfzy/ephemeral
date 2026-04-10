#pragma once

/// @file tls_decryptor.hpp
/// TLS record decryption — AES-GCM via aws-lc AEAD API (read direction).
///
/// Thread safety: decrypt() is NOT thread-safe. Exactly one thread
/// (the RX thread or app thread in direct mode) must own this object.

#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <type_traits>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/aead.h>
#include <openssl/err.h>
#include <openssl/mem.h>

#include "eph/transport/detail/tls_constants.hpp"

namespace eph::net {

namespace detail {
/// Lazily-initialized logger for TLS decryption operations.
/// @return Pointer to the "transport.tls_dec" spdlog logger.
inline spdlog::logger* tls_dec_logger() {
    static auto l = [] {
        auto lg = spdlog::get("transport.tls_dec");
        if (!lg) lg = spdlog::stdout_color_mt("transport.tls_dec");
        return lg;
    }();
    return l.get();
}
} // namespace detail

/// AES-GCM record-level decryption (read direction only).
///
/// Owns the decryption AEAD context, read IV, and read sequence number.
/// Created from TlsHotState after TLS 1.3 handshake key export.
class TlsDecryptor {
public:
    /// Create a TlsDecryptor from the read-direction key material in a TlsHotState.
    ///
    /// Initializes the AEAD context with the appropriate AES-GCM algorithm
    /// (128 or 256 bit) and copies the read IV and sequence number.
    ///
    /// @param state    TLS hot state containing read key, IV, and sequence
    /// @param key_len  AES key length: 16 (AES-128) or 32 (AES-256, default)
    /// @return Initialized TlsDecryptor, or error string on failure
    [[nodiscard]] static std::expected<TlsDecryptor, std::string>
    create(const TlsHotState& state, size_t key_len = tls_const::kAes256KeyLen) {
        TlsDecryptor dec;

        const EVP_AEAD* aead;
        if (key_len == 16) {
            aead = EVP_aead_aes_128_gcm();
        } else if (key_len == 32) {
            aead = EVP_aead_aes_256_gcm();
        } else {
            return std::unexpected(std::format(
                "Unsupported AES key length: {}", key_len));
        }

        if (!EVP_AEAD_CTX_init(&dec.ctx_, aead,
                               state.read.ki.key, key_len,
                               tls_record::kAuthTagLen, nullptr)) {
            return std::unexpected("EVP_AEAD_CTX_init failed for decryption");
        }
        dec.init_ = true;

        std::memcpy(dec.iv_, state.read.ki.iv, tls_const::kTls13NonceLen);
        dec.seq_ = state.read.seq;

        return dec;
    }

    ~TlsDecryptor() {
        if (init_) EVP_AEAD_CTX_cleanup(&ctx_);
        OPENSSL_cleanse(iv_, sizeof(iv_));
    }

    TlsDecryptor(const TlsDecryptor&) = delete;
    TlsDecryptor& operator=(const TlsDecryptor&) = delete;

    // Safety: EVP_AEAD_CTX in BoringSSL/aws-lc (AES-GCM) is a flat struct
    // containing the aead pointer + inline key schedule arrays, with no
    // internal heap pointers. Bitwise copy is safe for move semantics.
    // If aws-lc changes this invariant, replace with EVP_AEAD_CTX_init
    // from saved key material instead of direct struct copy.
    static_assert(std::is_trivially_copyable_v<EVP_AEAD_CTX>,
        "EVP_AEAD_CTX is no longer trivially copyable — bitwise move is unsafe");
    TlsDecryptor(TlsDecryptor&& other) noexcept
        : ctx_(other.ctx_), init_(other.init_), seq_(other.seq_) {
        std::memcpy(iv_, other.iv_, tls_const::kTls13NonceLen);
        other.init_ = false;
        OPENSSL_cleanse(other.iv_, sizeof(other.iv_));
    }

    TlsDecryptor& operator=(TlsDecryptor&& other) noexcept {
        if (this != &other) {
            if (init_) EVP_AEAD_CTX_cleanup(&ctx_);
            ctx_ = other.ctx_;
            init_ = other.init_;
            seq_ = other.seq_;
            std::memcpy(iv_, other.iv_, tls_const::kTls13NonceLen);
            std::memset(&other.ctx_, 0, sizeof(other.ctx_));
            other.init_ = false;
            OPENSSL_cleanse(other.iv_, sizeof(other.iv_));
        }
        return *this;
    }

    /// Decrypt a TLS record.
    /// @param record       Input TLS record (header + encrypted data + tag)
    /// @param record_len   Total record length
    /// @param out          Output buffer for decrypted plaintext
    /// @param[out] out_len Actual decrypted plaintext length (excluding content type)
    /// @return true on success, false on decryption/authentication failure
    [[nodiscard]] bool decrypt(const uint8_t* record, uint16_t record_len,
                 uint8_t* out, uint16_t& out_len) noexcept {
        if (!record || !out) [[unlikely]] {
            SPDLOG_LOGGER_ERROR(detail::tls_dec_logger(),
                "decrypt: null pointer — record={}, out={}",
                static_cast<const void*>(record), static_cast<void*>(out));
            return false;
        }
        if (record_len < tls_record::kRecordHeaderLen + tls_record::kAuthTagLen) {
            SPDLOG_LOGGER_DEBUG(detail::tls_dec_logger(),
                "decrypt: record too short: {} < {}",
                record_len, tls_record::kRecordHeaderLen + tls_record::kAuthTagLen);
            return false;
        }

        if (seq_ >= tls_record::kMaxSequenceNumber) {
            SPDLOG_LOGGER_ERROR(detail::tls_dec_logger(),
                "TLS read sequence limit reached ({}): must reconnect", seq_);
            return false;
        }

        uint8_t content_type;
        uint16_t payload_len;
        if (!tls_record::parse_record_header(record, content_type, payload_len)) {
            SPDLOG_LOGGER_WARN(detail::tls_dec_logger(),
                "decrypt: invalid record header — content_type=0x{:02X}, "
                "payload_len={}, record_len={}",
                content_type, payload_len, record_len);
            return false;
        }

        if (tls_record::kRecordHeaderLen + payload_len > record_len) {
            SPDLOG_LOGGER_DEBUG(detail::tls_dec_logger(),
                "decrypt: record truncated: header+payload={} > record_len={}",
                tls_record::kRecordHeaderLen + payload_len, record_len);
            return false;
        }

        if (payload_len < tls_record::kAuthTagLen + 1) {
            SPDLOG_LOGGER_DEBUG(detail::tls_dec_logger(),
                "TLS record too short: payload_len={} < min {}",
                payload_len, tls_record::kAuthTagLen + 1);
            return false;
        }

        const uint8_t* ciphertext = record + tls_record::kRecordHeaderLen;

        uint8_t nonce[tls_const::kTls13NonceLen];
        tls_record::build_nonce(nonce, iv_, seq_);

        size_t plaintext_len = 0;

        if (!EVP_AEAD_CTX_open(&ctx_, out, &plaintext_len,
                                payload_len - tls_record::kAuthTagLen,
                                nonce, tls_const::kTls13NonceLen,
                                ciphertext, payload_len,
                                record, tls_record::kRecordHeaderLen)) {
            // Pull the OpenSSL/AWS-LC error queue for actionable diagnosis
            // (auth tag mismatch vs. invalid nonce vs. context corruption etc.).
            char err_buf[256] = {0};
            unsigned long err_code = ERR_get_error();
            if (err_code != 0) {
                ERR_error_string_n(err_code, err_buf, sizeof(err_buf));
            }
            SPDLOG_LOGGER_ERROR(detail::tls_dec_logger(),
                "EVP_AEAD_CTX_open failed: record_len={}, seq={}, "
                "openssl_err=0x{:08x} ({})",
                record_len, seq_, err_code,
                err_buf[0] ? err_buf : "no error in queue");
            // Drain any remaining queued errors so we don't leak them
            // into a future caller's diagnosis.
            while (ERR_get_error() != 0) { /* drain */ }
            return false;
        }

        // TLS 1.3: last byte of decrypted data is the inner content type — strip it
        if (plaintext_len > 0) {
            plaintext_len--;
        }

        out_len = static_cast<uint16_t>(plaintext_len);
        seq_++;

        if (seq_ >= tls_record::kMaxSequenceNumber) [[unlikely]] {
            SPDLOG_LOGGER_ERROR(detail::tls_dec_logger(),
                "TLS read sequence exhausted ({}/{}): must reconnect immediately",
                seq_, tls_record::kMaxSequenceNumber);
        } else if (seq_ == tls_record::kSequenceWarnThreshold) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::tls_dec_logger(),
                "TLS read sequence approaching limit ({}/{})",
                seq_, tls_record::kMaxSequenceNumber);
        }

        return true;
    }

    /// Current read sequence number (monotonically increasing).
    /// @return Number of records decrypted so far
    [[nodiscard]] uint64_t read_seq() const noexcept { return seq_; }

private:
    TlsDecryptor() = default;

    EVP_AEAD_CTX ctx_{};
    bool         init_ = false;
    uint8_t      iv_[tls_const::kTls13NonceLen]{};
    uint64_t     seq_ = 0;
};

} // namespace eph::net
