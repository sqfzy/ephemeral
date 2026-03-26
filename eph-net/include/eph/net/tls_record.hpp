#pragma once

/// @file tls_record.hpp
/// TLS record layer — AES-256-GCM encryption/decryption via aws-lc AEAD API.
///
/// Uses EVP_AEAD_CTX_seal / EVP_AEAD_CTX_open for single-call AEAD,
/// replacing the multi-step EVP_Encrypt Init/Update/Final pattern.
/// This eliminates ~150ns of per-record API overhead.
///
/// TLS 1.3 record format:
///   [ContentType(1)] [Legacy version(2)] [Length(2)] [Encrypted data] [Auth tag(16)]

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/aead.h>

#include "eph/net/tls_session.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::shared_ptr<spdlog::logger> tls_record_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.tls_record");
        if (!lg) lg = spdlog::stdout_color_mt("net.tls_record");
        // Inherit level from spdlog global default
        return lg;
    }();
    return l;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// TLS record constants
// ─────────────────────────────────────────────────────────────────────────────

namespace tls_record {

inline constexpr uint8_t  kContentTypeAppData = 0x17;
inline constexpr uint16_t kLegacyVersion      = 0x0303; // TLS 1.2 for compat
inline constexpr uint16_t kRecordHeaderLen     = 5;
inline constexpr uint16_t kAuthTagLen          = 16;

/// Maximum TLS 1.3 record sequence number (conservative: 2^24).
/// RFC 8446 §5.5 recommends key update at ~2^24.5 records.
/// Beyond this limit, nonce reuse risk makes continued encryption unsafe.
inline constexpr uint64_t kMaxSequenceNumber = (1ULL << 24);

/// Threshold at which to emit a WARN log about approaching sequence exhaustion.
/// Set to 90% of kMaxSequenceNumber to give applications time to reconnect.
inline constexpr uint64_t kSequenceWarnThreshold = kMaxSequenceNumber * 9 / 10;

/// Threshold at which Transport triggers a preemptive reconnect for key refresh.
/// Set to 95% of kMaxSequenceNumber — above the warning but below the hard limit.
inline constexpr uint64_t kSequenceReconnectThreshold = kMaxSequenceNumber * 95 / 100;

/// Build the per-record nonce for TLS 1.3 AES-GCM.
/// nonce = write_iv XOR (sequence_number padded to 12 bytes)
/// Optimized: uses uint64_t XOR on the last 8 bytes instead of byte loop.
inline void build_nonce(uint8_t out[tls_const::kTls13NonceLen],
                        const uint8_t iv[tls_const::kTls13NonceLen],
                        uint64_t seq) noexcept {
    // Copy first 4 bytes of IV unchanged (seq only affects last 8 bytes)
    std::memcpy(out, iv, 4);

    // XOR last 8 bytes of IV with big-endian sequence number in one op
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

/// Write a TLS record header.
inline void write_record_header(uint8_t* dst, uint8_t content_type,
                                 uint16_t payload_len) noexcept {
    dst[0] = content_type;
    dst[1] = static_cast<uint8_t>(kLegacyVersion >> 8);
    dst[2] = static_cast<uint8_t>(kLegacyVersion & 0xFF);
    dst[3] = static_cast<uint8_t>(payload_len >> 8);
    dst[4] = static_cast<uint8_t>(payload_len & 0xFF);
}

/// Parse a TLS record header.
inline bool parse_record_header(const uint8_t* src,
                                 uint8_t& content_type,
                                 uint16_t& payload_len) noexcept {
    content_type = src[0];
    payload_len = static_cast<uint16_t>((src[3] << 8) | src[4]);
    // TLS 1.3: only application_data records pass through AEAD decryption
    return content_type == kContentTypeAppData &&
           payload_len <= tls_const::kMaxRecordPayload + kAuthTagLen + 1;
}

} // namespace tls_record

// ─────────────────────────────────────────────────────────────────────────────
// TLS Record Crypto — aws-lc EVP_AEAD API (single-call seal/open)
// ─────────────────────────────────────────────────────────────────────────────

/// AES-256-GCM record-level encryption/decryption via aws-lc AEAD API.
///
/// Thread safety: encrypt() and decrypt() use separate AEAD contexts.
/// It is safe to call encrypt() from one thread and decrypt() from another,
/// but each individual method must not be called concurrently with itself.
///
/// Typical ownership: TX thread owns encrypt(), RX thread owns decrypt().
class TlsRecordCrypto {
public:
    /// Initialize with extracted TLS session keys.
    /// @param key_len  AES key length: 16 (AES-128) or 32 (AES-256).
    ///                 Determined by the negotiated cipher suite.
    static std::expected<TlsRecordCrypto, std::string>
    create(const TlsHotState& state, size_t key_len = tls_const::kAes256KeyLen) {
        TlsRecordCrypto crypto;

        // Select AEAD algorithm based on negotiated key length
        const EVP_AEAD* aead;
        if (key_len == 16) {
            aead = EVP_aead_aes_128_gcm();
        } else if (key_len == 32) {
            aead = EVP_aead_aes_256_gcm();
        } else {
            return std::unexpected(std::format(
                "Unsupported AES key length: {}", key_len));
        }

        // Encryption context (write direction)
        if (!EVP_AEAD_CTX_init(&crypto.enc_ctx_, aead,
                               state.write.ki.key, key_len,
                               tls_record::kAuthTagLen, nullptr)) {
            return std::unexpected("EVP_AEAD_CTX_init failed for encryption");
        }
        crypto.enc_init_ = true;

        // Decryption context (read direction)
        if (!EVP_AEAD_CTX_init(&crypto.dec_ctx_, aead,
                               state.read.ki.key, key_len,
                               tls_record::kAuthTagLen, nullptr)) {
            EVP_AEAD_CTX_cleanup(&crypto.enc_ctx_);
            crypto.enc_init_ = false;
            return std::unexpected("EVP_AEAD_CTX_init failed for decryption");
        }
        crypto.dec_init_ = true;

        std::memcpy(crypto.write_iv_, state.write.ki.iv, tls_const::kTls13NonceLen);
        std::memcpy(crypto.read_iv_,  state.read.ki.iv,  tls_const::kTls13NonceLen);
        crypto.write_seq_ = state.write.seq;
        crypto.read_seq_  = state.read.seq;

        return crypto;
    }

    ~TlsRecordCrypto() {
        if (enc_init_) EVP_AEAD_CTX_cleanup(&enc_ctx_);
        if (dec_init_) EVP_AEAD_CTX_cleanup(&dec_ctx_);
    }

    TlsRecordCrypto(const TlsRecordCrypto&) = delete;
    TlsRecordCrypto& operator=(const TlsRecordCrypto&) = delete;

    // Safety: EVP_AEAD_CTX in BoringSSL/aws-lc is a flat struct (aead pointer +
    // key schedule arrays) with no internal self-referencing pointers, so bitwise
    // copy is safe. If aws-lc changes this invariant, these move operations must
    // be updated to use EVP_AEAD_CTX_init + cleanup instead of direct copy.
    TlsRecordCrypto(TlsRecordCrypto&& other) noexcept
        : enc_ctx_(other.enc_ctx_)
        , dec_ctx_(other.dec_ctx_)
        , enc_init_(other.enc_init_)
        , dec_init_(other.dec_init_)
        , write_seq_(other.write_seq_)
        , read_seq_(other.read_seq_) {
        std::memcpy(write_iv_, other.write_iv_, tls_const::kTls13NonceLen);
        std::memcpy(read_iv_,  other.read_iv_,  tls_const::kTls13NonceLen);
        other.enc_init_ = false;
        other.dec_init_ = false;
    }

    TlsRecordCrypto& operator=(TlsRecordCrypto&& other) noexcept {
        if (this != &other) {
            if (enc_init_) EVP_AEAD_CTX_cleanup(&enc_ctx_);
            if (dec_init_) EVP_AEAD_CTX_cleanup(&dec_ctx_);
            enc_ctx_ = other.enc_ctx_;
            dec_ctx_ = other.dec_ctx_;
            enc_init_ = other.enc_init_;
            dec_init_ = other.dec_init_;
            write_seq_ = other.write_seq_;
            read_seq_ = other.read_seq_;
            std::memcpy(write_iv_, other.write_iv_, tls_const::kTls13NonceLen);
            std::memcpy(read_iv_,  other.read_iv_,  tls_const::kTls13NonceLen);
            other.enc_init_ = false;
            other.dec_init_ = false;
        }
        return *this;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Encryption (hot path — single EVP_AEAD_CTX_seal call)
    // ─────────────────────────────────────────────────────────────────────────

    /// Encrypt plaintext into a TLS record.
    ///
    /// Output layout: [record_header(5)] [ciphertext(plaintext_len + 1)] [tag(16)]
    /// The "+1" is for the TLS 1.3 inner content type byte.
    ///
    /// @param plaintext     Input data — caller MUST ensure at least 1 byte of
    ///                      writable space past plaintext[plaintext_len] (used to
    ///                      temporarily append the TLS 1.3 inner content type byte,
    ///                      restored after seal). This eliminates a full-payload memcpy.
    /// @param plaintext_len Input length (must be <= kMaxRecordPayload)
    /// @param out           Output buffer (must have at least encrypted_size() bytes,
    ///                      must NOT overlap with plaintext)
    /// @return Total bytes written to out, or 0 on error
    uint16_t encrypt(uint8_t* plaintext, uint16_t plaintext_len,
                     uint8_t* out) noexcept {
        if (plaintext_len > tls_const::kMaxRecordPayload) return 0;

        if (write_seq_ >= tls_record::kMaxSequenceNumber) {
            SPDLOG_LOGGER_ERROR(detail::tls_record_logger(),
                "TLS write sequence limit reached ({}): nonce reuse risk, "
                "must reconnect", write_seq_);
            return 0;
        }

        uint16_t inner_len = plaintext_len + 1;
        uint16_t encrypted_len = inner_len + tls_record::kAuthTagLen;

        // Write TLS record header (also serves as AAD for AEAD)
        tls_record::write_record_header(out, tls_record::kContentTypeAppData,
                                         encrypted_len);

        // Build per-record nonce
        uint8_t nonce[tls_const::kTls13NonceLen];
        tls_record::build_nonce(nonce, write_iv_, write_seq_);

        // Temporarily append TLS 1.3 inner content type byte after payload.
        // Caller guarantees 1 byte of writable space past plaintext_len.
        // Guard: plaintext may be nullptr when plaintext_len == 0 (valid TLS padding).
        assert((plaintext != nullptr || plaintext_len == 0) &&
               "encrypt: plaintext must be non-null when plaintext_len > 0");
        uint8_t saved = 0;
        if (plaintext) {
            saved = plaintext[plaintext_len];
            plaintext[plaintext_len] = tls_record::kContentTypeAppData;
        }

        // For plaintext_len == 0 with nullptr, use a local buffer for the content type byte
        uint8_t fallback_inner = tls_record::kContentTypeAppData;
        const uint8_t* seal_input = plaintext ? plaintext : &fallback_inner;

        uint8_t* ciphertext = out + tls_record::kRecordHeaderLen;
        size_t ciphertext_len = 0;

        bool ok = EVP_AEAD_CTX_seal(&enc_ctx_, ciphertext, &ciphertext_len,
                                     inner_len + tls_record::kAuthTagLen,
                                     nonce, tls_const::kTls13NonceLen,
                                     seal_input, inner_len,
                                     out, tls_record::kRecordHeaderLen);

        // Restore the byte we temporarily overwrote
        if (plaintext) plaintext[plaintext_len] = saved;

        if (!ok) {
            SPDLOG_LOGGER_ERROR(detail::tls_record_logger(),
                "EVP_AEAD_CTX_seal failed: plaintext_len={}, write_seq={}",
                plaintext_len, write_seq_);
            return 0;
        }

        write_seq_++;

        // Advance warning: approaching sequence exhaustion
        if (write_seq_ == tls_record::kSequenceWarnThreshold) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::tls_record_logger(),
                "TLS write sequence at {}% of limit ({}/{}), "
                "consider reconnecting soon to avoid forced disconnect",
                write_seq_ * 100 / tls_record::kMaxSequenceNumber,
                write_seq_, tls_record::kMaxSequenceNumber);
        }

        return tls_record::kRecordHeaderLen + static_cast<uint16_t>(ciphertext_len);
    }

    /// Compute the output size for encrypting a given plaintext length.
    static constexpr uint16_t encrypted_size(uint16_t plaintext_len) noexcept {
        // header(5) + plaintext + content_type(1) + tag(16)
        return tls_record::kRecordHeaderLen + plaintext_len + 1 +
               tls_record::kAuthTagLen;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Decryption (hot path — single EVP_AEAD_CTX_open call)
    // ─────────────────────────────────────────────────────────────────────────

    /// Decrypt a TLS record.
    ///
    /// @param record       Input TLS record (header + encrypted data + tag)
    /// @param record_len   Total record length
    /// @param out          Output buffer for decrypted plaintext
    /// @param[out] out_len Actual decrypted plaintext length (excluding content type)
    /// @return true on success, false on decryption/authentication failure
    bool decrypt(const uint8_t* record, uint16_t record_len,
                 uint8_t* out, uint16_t& out_len) noexcept {
        if (record_len < tls_record::kRecordHeaderLen + tls_record::kAuthTagLen) {
            SPDLOG_LOGGER_DEBUG(detail::tls_record_logger(),
                "TLS decrypt: record too short for header+tag: {} < {}",
                record_len,
                tls_record::kRecordHeaderLen + tls_record::kAuthTagLen);
            return false;
        }

        if (read_seq_ >= tls_record::kMaxSequenceNumber) {
            SPDLOG_LOGGER_ERROR(detail::tls_record_logger(),
                "TLS read sequence limit reached ({}): nonce reuse risk, "
                "must reconnect", read_seq_);
            return false;
        }

        uint8_t content_type;
        uint16_t payload_len;
        if (!tls_record::parse_record_header(record, content_type, payload_len)) {
            return false;
        }

        if (tls_record::kRecordHeaderLen + payload_len > record_len) {
            SPDLOG_LOGGER_DEBUG(detail::tls_record_logger(),
                "TLS decrypt: record truncated: header+payload={} > record_len={}",
                tls_record::kRecordHeaderLen + payload_len, record_len);
            return false;
        }

        // Payload must contain at least the auth tag (16 bytes) + inner content type (1 byte)
        if (payload_len < tls_record::kAuthTagLen + 1) {
            SPDLOG_LOGGER_DEBUG(detail::tls_record_logger(),
                "TLS record too short: payload_len={} < min {}",
                payload_len, tls_record::kAuthTagLen + 1);
            return false;
        }

        const uint8_t* ciphertext = record + tls_record::kRecordHeaderLen;

        // Build nonce
        uint8_t nonce[tls_const::kTls13NonceLen];
        tls_record::build_nonce(nonce, read_iv_, read_seq_);

        // Single-call AEAD open: nonce + AAD (record header) + ciphertext+tag -> plaintext
        size_t plaintext_len = 0;

        if (!EVP_AEAD_CTX_open(&dec_ctx_, out, &plaintext_len,
                                payload_len - tls_record::kAuthTagLen, // max_out_len: plaintext capacity (excludes tag)
                                nonce, tls_const::kTls13NonceLen,
                                ciphertext, payload_len, // in_len: ciphertext + tag
                                record, tls_record::kRecordHeaderLen)) {
            SPDLOG_LOGGER_ERROR(detail::tls_record_logger(),
                "EVP_AEAD_CTX_open failed: record_len={}, read_seq={}",
                record_len, read_seq_);
            return false;
        }

        // TLS 1.3: last byte of decrypted data is the inner content type — strip it
        if (plaintext_len > 0) {
            plaintext_len--;
        }

        out_len = static_cast<uint16_t>(plaintext_len);
        read_seq_++;

        // Advance warning: approaching sequence exhaustion
        if (read_seq_ == tls_record::kSequenceWarnThreshold) [[unlikely]] {
            SPDLOG_LOGGER_WARN(detail::tls_record_logger(),
                "TLS read sequence at {}% of limit ({}/{}), "
                "consider reconnecting soon to avoid forced disconnect",
                read_seq_ * 100 / tls_record::kMaxSequenceNumber,
                read_seq_, tls_record::kMaxSequenceNumber);
        }

        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Queries
    // ─────────────────────────────────────────────────────────────────────────

    [[nodiscard]] uint64_t write_seq() const noexcept { return write_seq_; }
    [[nodiscard]] uint64_t read_seq()  const noexcept { return read_seq_; }

    /// Skip N TLS records without decrypting — advances the read sequence
    /// counter to stay in sync with the server.  Used by Transport when
    /// RxQueue is EvictingQueue (latest-value semantics) to skip stale
    /// records and only decrypt the most recent one.
    ///
    /// SAFETY: skipped records may contain TLS KeyUpdate messages.  If so,
    /// the next real decrypt() will fail and Transport will reconnect.
    void advance_read_seq(uint64_t count) noexcept { read_seq_ += count; }

private:
    TlsRecordCrypto() = default;

    EVP_AEAD_CTX enc_ctx_{};
    EVP_AEAD_CTX dec_ctx_{};
    bool         enc_init_ = false;
    bool         dec_init_ = false;

    uint8_t  write_iv_[tls_const::kTls13NonceLen]{};
    uint8_t  read_iv_[tls_const::kTls13NonceLen]{};
    uint64_t write_seq_ = 0;
    uint64_t read_seq_  = 0;
};

} // namespace eph::net
