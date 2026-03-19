#pragma once

/// @file tls_record.hpp
/// TLS record layer — AES-256-GCM encryption/decryption for hot path.
///
/// Phase 1: Wraps EVP_AEAD_CTX_seal / EVP_AEAD_CTX_open from aws-lc.
/// Operates directly on mbufs for zero-copy encryption.
///
/// TLS 1.3 record format:
///   [ContentType(1)] [Legacy version(2)] [Length(2)] [Encrypted data] [Auth tag(16)]
///
/// For application data, ContentType = 0x17 (application_data).

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include <openssl/evp.h>

#include "eph/dpdk/tls_session.hpp"

namespace eph::dpdk {

// ─────────────────────────────────────────────────────────────────────────────
// TLS record constants
// ─────────────────────────────────────────────────────────────────────────────

namespace tls_record {

inline constexpr uint8_t  kContentTypeAppData = 0x17;
inline constexpr uint16_t kLegacyVersion      = 0x0303; // TLS 1.2 for compat
inline constexpr uint16_t kRecordHeaderLen     = 5;
inline constexpr uint16_t kAuthTagLen          = 16;

/// Build the per-record nonce for TLS 1.3 AES-GCM.
/// nonce = write_iv XOR (sequence_number padded to 12 bytes)
inline void build_nonce(uint8_t out[tls_const::kTls13NonceLen],
                        const uint8_t iv[tls_const::kTls13NonceLen],
                        uint64_t seq) noexcept {
    // Start with IV
    std::memcpy(out, iv, tls_const::kTls13NonceLen);
    // XOR sequence number into the last 8 bytes
    for (int i = 0; i < 8; ++i) {
        out[tls_const::kTls13NonceLen - 1 - i] ^=
            static_cast<uint8_t>((seq >> (i * 8)) & 0xFF);
    }
}

/// Write a TLS record header.
/// @param dst   Output buffer (must have at least 5 bytes)
/// @param content_type   Content type (0x17 for application data)
/// @param payload_len    Length of encrypted payload + auth tag
inline void write_record_header(uint8_t* dst, uint8_t content_type,
                                 uint16_t payload_len) noexcept {
    dst[0] = content_type;
    dst[1] = static_cast<uint8_t>(kLegacyVersion >> 8);
    dst[2] = static_cast<uint8_t>(kLegacyVersion & 0xFF);
    dst[3] = static_cast<uint8_t>(payload_len >> 8);
    dst[4] = static_cast<uint8_t>(payload_len & 0xFF);
}

/// Parse a TLS record header.
/// @param src   Input buffer (must have at least 5 bytes)
/// @param[out] content_type  Parsed content type
/// @param[out] payload_len   Parsed payload length (including auth tag)
/// @return true if valid, false if malformed
inline bool parse_record_header(const uint8_t* src,
                                 uint8_t& content_type,
                                 uint16_t& payload_len) noexcept {
    content_type = src[0];
    payload_len = static_cast<uint16_t>((src[3] << 8) | src[4]);
    // Basic validation
    return payload_len <= tls_const::kMaxRecordPayload + kAuthTagLen + 1;
}

} // namespace tls_record

// ─────────────────────────────────────────────────────────────────────────────
// TLS Record Encryptor — hot-path AES-256-GCM encryption
// ─────────────────────────────────────────────────────────────────────────────

/// Encrypts application data into TLS records using AES-256-GCM.
/// Designed for hot-path use — no allocations, no syscalls, no locks.
///
/// Usage:
///   1. Initialize with TlsHotState (extracted after handshake)
///   2. Call encrypt() to produce TLS records in-place on a buffer
///   3. Call decrypt() to decrypt received TLS records
class TlsRecordCrypto {
public:
    /// Initialize the encryptor with extracted session keys.
    /// @return Error if cipher initialization fails
    static std::expected<TlsRecordCrypto, std::string>
    create(const TlsHotState& state) {
        TlsRecordCrypto crypto;

        // Initialize encryption context (AES-256-GCM)
        crypto.enc_ctx_ = EVP_CIPHER_CTX_new();
        if (!crypto.enc_ctx_) {
            return std::unexpected("EVP_CIPHER_CTX_new failed for encryption");
        }

        if (EVP_EncryptInit_ex(crypto.enc_ctx_, EVP_aes_256_gcm(),
                               nullptr, nullptr, nullptr) != 1) {
            EVP_CIPHER_CTX_free(crypto.enc_ctx_);
            return std::unexpected("EVP_EncryptInit_ex failed");
        }

        // Initialize decryption context
        crypto.dec_ctx_ = EVP_CIPHER_CTX_new();
        if (!crypto.dec_ctx_) {
            EVP_CIPHER_CTX_free(crypto.enc_ctx_);
            return std::unexpected("EVP_CIPHER_CTX_new failed for decryption");
        }

        if (EVP_DecryptInit_ex(crypto.dec_ctx_, EVP_aes_256_gcm(),
                               nullptr, nullptr, nullptr) != 1) {
            EVP_CIPHER_CTX_free(crypto.enc_ctx_);
            EVP_CIPHER_CTX_free(crypto.dec_ctx_);
            return std::unexpected("EVP_DecryptInit_ex failed");
        }

        // Store keys and IVs
        std::memcpy(crypto.write_key_, state.write_key, tls_const::kAes256KeyLen);
        std::memcpy(crypto.write_iv_,  state.write_iv,  tls_const::kTls13NonceLen);
        std::memcpy(crypto.read_key_,  state.read_key,  tls_const::kAes256KeyLen);
        std::memcpy(crypto.read_iv_,   state.read_iv,   tls_const::kTls13NonceLen);
        crypto.write_seq_ = state.write_seq;
        crypto.read_seq_  = state.read_seq;

        return crypto;
    }

    ~TlsRecordCrypto() {
        if (enc_ctx_) EVP_CIPHER_CTX_free(enc_ctx_);
        if (dec_ctx_) EVP_CIPHER_CTX_free(dec_ctx_);
    }

    TlsRecordCrypto(const TlsRecordCrypto&) = delete;
    TlsRecordCrypto& operator=(const TlsRecordCrypto&) = delete;

    TlsRecordCrypto(TlsRecordCrypto&& other) noexcept
        : enc_ctx_(other.enc_ctx_)
        , dec_ctx_(other.dec_ctx_)
        , write_seq_(other.write_seq_)
        , read_seq_(other.read_seq_) {
        std::memcpy(write_key_, other.write_key_, tls_const::kAes256KeyLen);
        std::memcpy(write_iv_,  other.write_iv_,  tls_const::kTls13NonceLen);
        std::memcpy(read_key_,  other.read_key_,  tls_const::kAes256KeyLen);
        std::memcpy(read_iv_,   other.read_iv_,   tls_const::kTls13NonceLen);
        other.enc_ctx_ = nullptr;
        other.dec_ctx_ = nullptr;
    }

    TlsRecordCrypto& operator=(TlsRecordCrypto&& other) noexcept {
        if (this != &other) {
            if (enc_ctx_) EVP_CIPHER_CTX_free(enc_ctx_);
            if (dec_ctx_) EVP_CIPHER_CTX_free(dec_ctx_);
            enc_ctx_ = other.enc_ctx_;
            dec_ctx_ = other.dec_ctx_;
            write_seq_ = other.write_seq_;
            read_seq_ = other.read_seq_;
            std::memcpy(write_key_, other.write_key_, tls_const::kAes256KeyLen);
            std::memcpy(write_iv_,  other.write_iv_,  tls_const::kTls13NonceLen);
            std::memcpy(read_key_,  other.read_key_,  tls_const::kAes256KeyLen);
            std::memcpy(read_iv_,   other.read_iv_,   tls_const::kTls13NonceLen);
            other.enc_ctx_ = nullptr;
            other.dec_ctx_ = nullptr;
        }
        return *this;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Encryption (hot path)
    // ─────────────────────────────────────────────────────────────────────────

    /// Encrypt plaintext into a TLS record.
    ///
    /// Output layout: [record_header(5)] [ciphertext(plaintext_len + 1)] [tag(16)]
    /// The "+1" is for the TLS 1.3 inner content type byte appended to plaintext.
    ///
    /// @param plaintext     Input data
    /// @param plaintext_len Input length (must be <= kMaxRecordPayload)
    /// @param out           Output buffer (must have at least
    ///                      5 + plaintext_len + 1 + 16 bytes)
    /// @return Total bytes written to out, or 0 on error
    uint16_t encrypt(const uint8_t* plaintext, uint16_t plaintext_len,
                     uint8_t* out) noexcept {
        if (plaintext_len > tls_const::kMaxRecordPayload) return 0;

        // TLS 1.3: plaintext + content type byte (0x17 for app data)
        // Total inner plaintext = plaintext_len + 1
        uint16_t inner_len = plaintext_len + 1;
        uint16_t encrypted_len = inner_len + tls_record::kAuthTagLen;

        // Write TLS record header
        tls_record::write_record_header(out, tls_record::kContentTypeAppData,
                                         encrypted_len);

        // Build per-record nonce
        uint8_t nonce[tls_const::kTls13NonceLen];
        tls_record::build_nonce(nonce, write_iv_, write_seq_);

        // Set key and nonce for this record
        if (EVP_EncryptInit_ex(enc_ctx_, nullptr, nullptr,
                               write_key_, nonce) != 1) {
            return 0;
        }

        // AAD = TLS record header (5 bytes)
        int aad_len;
        if (EVP_EncryptUpdate(enc_ctx_, nullptr, &aad_len,
                              out, tls_record::kRecordHeaderLen) != 1) {
            return 0;
        }

        uint8_t* ciphertext = out + tls_record::kRecordHeaderLen;

        // Encrypt plaintext
        int out_len;
        if (EVP_EncryptUpdate(enc_ctx_, ciphertext, &out_len,
                              plaintext, plaintext_len) != 1) {
            return 0;
        }

        // Append inner content type and encrypt
        uint8_t content_type = tls_record::kContentTypeAppData;
        int ct_len;
        if (EVP_EncryptUpdate(enc_ctx_, ciphertext + out_len, &ct_len,
                              &content_type, 1) != 1) {
            return 0;
        }
        out_len += ct_len;

        // Finalize
        int final_len;
        if (EVP_EncryptFinal_ex(enc_ctx_, ciphertext + out_len,
                                &final_len) != 1) {
            return 0;
        }
        out_len += final_len;

        // Get auth tag
        if (EVP_CIPHER_CTX_ctrl(enc_ctx_, EVP_CTRL_GCM_GET_TAG,
                                tls_record::kAuthTagLen,
                                ciphertext + out_len) != 1) {
            return 0;
        }

        write_seq_++;
        return tls_record::kRecordHeaderLen + encrypted_len;
    }

    /// Compute the output size for encrypting a given plaintext length.
    static constexpr uint16_t encrypted_size(uint16_t plaintext_len) noexcept {
        // header(5) + plaintext + content_type(1) + tag(16)
        return tls_record::kRecordHeaderLen + plaintext_len + 1 +
               tls_record::kAuthTagLen;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Decryption (hot path)
    // ─────────────────────────────────────────────────────────────────────────

    /// Decrypt a TLS record.
    ///
    /// @param record       Input TLS record (header + encrypted data + tag)
    /// @param record_len   Total record length
    /// @param out          Output buffer for decrypted plaintext
    /// @param[out] out_len Actual decrypted plaintext length (excluding content type)
    /// @return true on success, false on decryption failure
    bool decrypt(const uint8_t* record, uint16_t record_len,
                 uint8_t* out, uint16_t& out_len) noexcept {
        if (record_len < tls_record::kRecordHeaderLen + tls_record::kAuthTagLen) {
            return false;
        }

        // Parse record header
        uint8_t content_type;
        uint16_t payload_len;
        if (!tls_record::parse_record_header(record, content_type, payload_len)) {
            return false;
        }

        if (tls_record::kRecordHeaderLen + payload_len > record_len) {
            return false;
        }

        const uint8_t* ciphertext = record + tls_record::kRecordHeaderLen;
        uint16_t ciphertext_len = payload_len - tls_record::kAuthTagLen;
        const uint8_t* tag = ciphertext + ciphertext_len;

        // Build nonce
        uint8_t nonce[tls_const::kTls13NonceLen];
        tls_record::build_nonce(nonce, read_iv_, read_seq_);

        // Set key and nonce
        if (EVP_DecryptInit_ex(dec_ctx_, nullptr, nullptr,
                               read_key_, nonce) != 1) {
            return false;
        }

        // AAD = record header
        int aad_len;
        if (EVP_DecryptUpdate(dec_ctx_, nullptr, &aad_len,
                              record, tls_record::kRecordHeaderLen) != 1) {
            return false;
        }

        // Decrypt
        int decrypted;
        if (EVP_DecryptUpdate(dec_ctx_, out, &decrypted,
                              ciphertext, ciphertext_len) != 1) {
            return false;
        }

        // Set expected tag
        if (EVP_CIPHER_CTX_ctrl(dec_ctx_, EVP_CTRL_GCM_SET_TAG,
                                tls_record::kAuthTagLen,
                                const_cast<uint8_t*>(tag)) != 1) {
            return false;
        }

        // Verify tag
        int final_len;
        if (EVP_DecryptFinal_ex(dec_ctx_, out + decrypted, &final_len) != 1) {
            return false; // Authentication failure
        }
        decrypted += final_len;

        // TLS 1.3: last byte of decrypted data is the inner content type
        if (decrypted > 0) {
            decrypted--; // Strip inner content type
        }

        out_len = static_cast<uint16_t>(decrypted);
        read_seq_++;
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Queries
    // ─────────────────────────────────────────────────────────────────────────

    [[nodiscard]] uint64_t write_seq() const noexcept { return write_seq_; }
    [[nodiscard]] uint64_t read_seq()  const noexcept { return read_seq_; }

private:
    TlsRecordCrypto() = default;

    EVP_CIPHER_CTX* enc_ctx_ = nullptr;
    EVP_CIPHER_CTX* dec_ctx_ = nullptr;

    uint8_t  write_key_[tls_const::kAes256KeyLen]{};
    uint8_t  write_iv_[tls_const::kTls13NonceLen]{};
    uint8_t  read_key_[tls_const::kAes256KeyLen]{};
    uint8_t  read_iv_[tls_const::kTls13NonceLen]{};
    uint64_t write_seq_ = 0;
    uint64_t read_seq_  = 0;
};

} // namespace eph::dpdk
