#pragma once

/// @file tls_encryptor.hpp
/// TLS record encryption — AES-GCM via aws-lc AEAD API (write direction).
///
/// Thread safety: encrypt() is NOT thread-safe. Exactly one thread
/// (the TX thread or app thread in direct mode) must own this object.

#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <type_traits>

#include <spdlog/spdlog.h>

#include <openssl/aead.h>
#include <openssl/err.h>
#include <openssl/mem.h>

#include "eph/core/detail/logger.hpp"
#include "eph/net/detail/tls_constants.hpp"

namespace eph::net {

namespace detail {
/// Lazily-initialized logger for TLS encryption operations.
/// @return Pointer to the "transport.tls_enc" spdlog logger.
inline spdlog::logger* tls_enc_logger() {
    static auto* l = ::eph::core::detail::make_logger("transport.tls_enc");
    return l;
}
} // namespace detail

/// AES-GCM record-level encryption (write direction only).
///
/// Owns the encryption AEAD context, write IV, and write sequence number.
/// Created from TlsHotState after TLS 1.3 handshake key export.
class TlsEncryptor {
public:
    /// Create a TlsEncryptor from the write-direction key material in a TlsHotState.
    ///
    /// Initializes the AEAD context with the appropriate AES-GCM algorithm
    /// (128 or 256 bit) and copies the write IV and sequence number.
    ///
    /// @param state    TLS hot state containing write key, IV, and sequence
    /// @param key_len  AES key length: 16 (AES-128) or 32 (AES-256, default)
    /// @return Initialized TlsEncryptor, or error string on failure
    [[nodiscard]] static std::expected<TlsEncryptor, std::string>
    create(const TlsHotState& state, size_t key_len = tls_const::kAes256KeyLen) {
        TlsEncryptor enc;

        const EVP_AEAD* aead;
        if (key_len == 16) {
            aead = EVP_aead_aes_128_gcm();
        } else if (key_len == 32) {
            aead = EVP_aead_aes_256_gcm();
        } else {
            return std::unexpected(std::format(
                "Unsupported AES key length: {}", key_len));
        }

        if (!EVP_AEAD_CTX_init(&enc.ctx_, aead,
                               state.write.ki.key, key_len,
                               tls_record::kAuthTagLen, nullptr)) {
            return std::unexpected("EVP_AEAD_CTX_init failed for encryption");
        }
        enc.init_ = true;

        std::memcpy(enc.iv_, state.write.ki.iv, tls_const::kTls13NonceLen);
        enc.seq_ = state.write.seq;

        return enc;
    }

    ~TlsEncryptor() {
        if (init_) EVP_AEAD_CTX_cleanup(&ctx_);
        OPENSSL_cleanse(iv_, sizeof(iv_));
    }

    TlsEncryptor(const TlsEncryptor&) = delete;
    TlsEncryptor& operator=(const TlsEncryptor&) = delete;

    // Safety: EVP_AEAD_CTX in BoringSSL/aws-lc (AES-GCM) is a flat struct
    // containing the aead pointer + inline key schedule arrays, with no
    // internal heap pointers. Bitwise copy is safe for move semantics.
    // If aws-lc changes this invariant, replace with EVP_AEAD_CTX_init
    // from saved key material instead of direct struct copy.
    static_assert(std::is_trivially_copyable_v<EVP_AEAD_CTX>,
        "EVP_AEAD_CTX is no longer trivially copyable — bitwise move is unsafe");
    TlsEncryptor(TlsEncryptor&& other) noexcept
        : ctx_(other.ctx_), init_(other.init_), seq_(other.seq_) {
        std::memcpy(iv_, other.iv_, tls_const::kTls13NonceLen);
        other.init_ = false;
        OPENSSL_cleanse(other.iv_, sizeof(other.iv_));
    }

    TlsEncryptor& operator=(TlsEncryptor&& other) noexcept {
        if (this != &other) {
            if (init_) EVP_AEAD_CTX_cleanup(&ctx_);
            ctx_ = other.ctx_;
            init_ = other.init_;
            seq_ = other.seq_;
            std::memcpy(iv_, other.iv_, tls_const::kTls13NonceLen);
            OPENSSL_cleanse(&other.ctx_, sizeof(other.ctx_));
            other.init_ = false;
            OPENSSL_cleanse(other.iv_, sizeof(other.iv_));
        }
        return *this;
    }

    /// Encrypt plaintext into a TLS record.
    /// @param plaintext     Input data
    /// @param plaintext_len Input length (must be <= kMaxRecordPayload)
    /// @param out           Output buffer (must have at least encrypted_size() bytes)
    /// @return Total bytes written to out, or 0 on error
    [[nodiscard]] uint16_t encrypt(const uint8_t* plaintext, uint16_t plaintext_len,
                     uint8_t* out) noexcept {
        if (plaintext_len > tls_const::kMaxRecordPayload) {
            SPDLOG_LOGGER_WARN(detail::tls_enc_logger(),
                "encrypt: plaintext_len={} exceeds kMaxRecordPayload={}",
                plaintext_len, tls_const::kMaxRecordPayload);
            return 0;
        }

        if (seq_ >= tls_record::kMaxSequenceNumber) {
            SPDLOG_LOGGER_ERROR(detail::tls_enc_logger(),
                "TLS write sequence limit reached ({}): must reconnect", seq_);
            return 0;
        }

        if (plaintext == nullptr && plaintext_len > 0) [[unlikely]] {
            SPDLOG_LOGGER_ERROR(detail::tls_enc_logger(),
                "encrypt: plaintext is null but plaintext_len={}", plaintext_len);
            return 0;
        }

        uint16_t inner_len = plaintext_len + 1;
        uint16_t encrypted_len = inner_len + tls_record::kAuthTagLen;

        tls_record::write_record_header(out, tls_record::kContentTypeAppData,
                                         encrypted_len);

        uint8_t nonce[tls_const::kTls13NonceLen];
        tls_record::build_nonce(nonce, iv_, seq_);

        // Build inner plaintext (payload + content type byte) directly in
        // the output buffer region that will hold ciphertext. BoringSSL/
        // aws-lc EVP_AEAD_CTX_seal supports in-place encryption (out == in),
        // so we avoid a separate 16 KB stack buffer that was previously
        // allocated on every encrypt() call.
        uint8_t* cipher_region = out + tls_record::kRecordHeaderLen;
        if (plaintext_len > 0) {
            std::memcpy(cipher_region, plaintext, plaintext_len);
        }
        cipher_region[plaintext_len] = tls_record::kContentTypeAppData;

        size_t ciphertext_len = 0;

        bool ok = EVP_AEAD_CTX_seal(&ctx_, cipher_region, &ciphertext_len,
                                     inner_len + tls_record::kAuthTagLen,
                                     nonce, tls_const::kTls13NonceLen,
                                     cipher_region, inner_len,
                                     out, tls_record::kRecordHeaderLen);

        if (!ok) {
            // Pull the OpenSSL/AWS-LC error queue for actionable diagnosis.
            char err_buf[256] = {0};
            unsigned long err_code = ERR_get_error();
            if (err_code != 0) {
                ERR_error_string_n(err_code, err_buf, sizeof(err_buf));
            }
            SPDLOG_LOGGER_ERROR(detail::tls_enc_logger(),
                "EVP_AEAD_CTX_seal failed: plaintext_len={}, seq={}, "
                "openssl_err=0x{:08x} ({})",
                plaintext_len, seq_, err_code,
                err_buf[0] ? err_buf : "no error in queue");
            while (ERR_get_error() != 0) { /* drain */ }
            return 0;
        }

        seq_++;

        if (seq_ >= tls_record::kMaxSequenceNumber) [[unlikely]] {
            SPDLOG_LOGGER_ERROR(detail::tls_enc_logger(),
                "TLS write sequence exhausted ({}/{}): must reconnect immediately",
                seq_, tls_record::kMaxSequenceNumber);
        } else if (seq_ == tls_record::kSequenceWarnThreshold) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::tls_enc_logger(),
                "TLS write sequence approaching limit ({}/{})",
                seq_, tls_record::kMaxSequenceNumber);
        }

        return tls_record::kRecordHeaderLen + static_cast<uint16_t>(ciphertext_len);
    }

    /// Compute output size for encrypting a given plaintext length.
    static constexpr uint16_t encrypted_size(uint16_t plaintext_len) noexcept {
        return tls_record::kRecordHeaderLen + plaintext_len + 1 +
               tls_record::kAuthTagLen;
    }

    /// Current write sequence number (monotonically increasing).
    /// @return Number of records encrypted so far
    [[nodiscard]] uint64_t write_seq() const noexcept { return seq_; }

private:
    TlsEncryptor() = default;

    EVP_AEAD_CTX ctx_{};
    bool         init_ = false;
    uint8_t      iv_[tls_const::kTls13NonceLen]{};
    uint64_t     seq_ = 0;
};

} // namespace eph::net
