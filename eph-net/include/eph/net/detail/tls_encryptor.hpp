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

#include "eph/core/log.hpp"

#include <openssl/aead.h>
#include <openssl/err.h>
#include <openssl/mem.h>

#include "eph/net/detail/tls_constants.hpp"

namespace eph::net {

namespace detail {
/// Lazily-initialized logger for TLS encryption operations.
/// @return Pointer to the "transport.tls_enc" spdlog logger.
inline spdlog::logger* tls_enc_logger() {
    static spdlog::logger* l = ::eph::log::get("transport.tls_enc");
    return l;
}
} // namespace detail

/// AEAD record-level encryption (write direction only).
///
/// Owns the encryption AEAD context, write IV, sequence number, and the
/// record format (TLS 1.3 vs 1.2 GCM vs 1.2 CHACHA20). Created from
/// TlsHotState after handshake key extraction.
///
/// The record format is fixed for the lifetime of a session — it's set
/// once at `create()` and read by `encrypt()` on each record. Branch
/// prediction is therefore essentially perfect (100% predictable per
/// session), so the cost of supporting three formats here is in the
/// nanosecond range.
class TlsEncryptor {
public:
    /// Create a TlsEncryptor from the write-direction key material in a TlsHotState.
    ///
    /// Initializes the AEAD context with the appropriate algorithm —
    /// AES-128/256-GCM or CHACHA20-POLY1305 — based on `state.record_format`
    /// and `key_len`, and copies the write IV and sequence number.
    ///
    /// @param state    TLS hot state (key, IV, seq, version, record_format)
    /// @param key_len  Key length:
    ///                   - 16 for AES-128-GCM (Tls13 / Tls12AesGcm)
    ///                   - 32 for AES-256-GCM (Tls13 / Tls12AesGcm)
    ///                   - 32 for CHACHA20-POLY1305 (Tls12Chacha20)
    /// @return Initialized TlsEncryptor, or error string on failure
    [[nodiscard]] static std::expected<TlsEncryptor, std::string>
    create(const TlsHotState& state, size_t key_len = tls_const::kAes256KeyLen) {
        TlsEncryptor enc;

        const EVP_AEAD* aead = nullptr;
        if (state.record_format == TlsRecordFormat::Tls12Chacha20) {
            if (key_len != 32) {
                return std::unexpected(std::format(
                    "CHACHA20-POLY1305 requires 32-byte key, got {}", key_len));
            }
            aead = EVP_aead_chacha20_poly1305();
        } else {
            // Tls13 or Tls12AesGcm — AES-GCM family
            if (key_len == 16) {
                aead = EVP_aead_aes_128_gcm();
            } else if (key_len == 32) {
                aead = EVP_aead_aes_256_gcm();
            } else {
                return std::unexpected(std::format(
                    "Unsupported AES key length: {}", key_len));
            }
        }

        if (!EVP_AEAD_CTX_init(&enc.ctx_, aead,
                               state.write.ki.key, key_len,
                               tls_record::kAuthTagLen, nullptr)) {
            return std::unexpected("EVP_AEAD_CTX_init failed for encryption");
        }
        enc.init_ = true;

        std::memcpy(enc.iv_, state.write.ki.iv, tls_const::kTls13NonceLen);
        enc.seq_           = state.write.seq;
        enc.record_format_ = state.record_format;

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
        : ctx_(other.ctx_), init_(other.init_), seq_(other.seq_),
          record_format_(other.record_format_) {
        std::memcpy(iv_, other.iv_, tls_const::kTls13NonceLen);
        other.init_ = false;
        // Cleanse both halves of the secret state so the moved-from
        // instance does not retain expanded AES round keys (held inside
        // EVP_AEAD_CTX for aws-lc AES-GCM) or the static IV. Mirrors the
        // move-assignment operator below; the move-ctor was previously
        // asymmetric and left ctx_ intact (matches the same fix already
        // applied in tls_inplace.hpp).
        OPENSSL_cleanse(&other.ctx_, sizeof(other.ctx_));
        OPENSSL_cleanse(other.iv_, sizeof(other.iv_));
    }

    TlsEncryptor& operator=(TlsEncryptor&& other) noexcept {
        if (this != &other) {
            if (init_) EVP_AEAD_CTX_cleanup(&ctx_);
            ctx_           = other.ctx_;
            init_          = other.init_;
            seq_           = other.seq_;
            record_format_ = other.record_format_;
            std::memcpy(iv_, other.iv_, tls_const::kTls13NonceLen);
            OPENSSL_cleanse(&other.ctx_, sizeof(other.ctx_));
            other.init_ = false;
            OPENSSL_cleanse(other.iv_, sizeof(other.iv_));
        }
        return *this;
    }

    /// Encrypt plaintext into a TLS record.
    ///
    /// Branches once on `record_format_` and dispatches to one of three
    /// inline private helpers. The branch is fully predictable per session
    /// (one record format for the connection's lifetime), so the runtime
    /// cost over the original 1.3-only path is essentially zero.
    ///
    /// @param plaintext     Input data
    /// @param plaintext_len Input length (must be <= kMaxRecordPayload)
    /// @param out           Output buffer (must have at least
    ///                      `encrypted_size(plaintext_len)` bytes)
    /// @return Total bytes written to out, or 0 on error
    [[nodiscard]] uint16_t encrypt(const uint8_t* plaintext, uint16_t plaintext_len,
                     uint8_t* out) noexcept {
        if (plaintext_len > tls_const::kMaxRecordPayload) {
            EPH_LOG_WARN(detail::tls_enc_logger(),
                "encrypt: plaintext_len={} exceeds kMaxRecordPayload={}",
                plaintext_len, tls_const::kMaxRecordPayload);
            return 0;
        }

        if (seq_ >= tls_record::kMaxSequenceNumber) {
            EPH_LOG_ERROR(detail::tls_enc_logger(),
                "TLS write sequence limit reached ({}): must reconnect", seq_);
            return 0;
        }

        if (plaintext == nullptr && plaintext_len > 0) [[unlikely]] {
            EPH_LOG_ERROR(detail::tls_enc_logger(),
                "encrypt: plaintext is null but plaintext_len={}", plaintext_len);
            return 0;
        }

        uint16_t written = 0;
        switch (record_format_) {
            case TlsRecordFormat::Tls13:
                written = encrypt_tls13_(plaintext, plaintext_len, out);
                break;
            case TlsRecordFormat::Tls12AesGcm:
                written = encrypt_tls12_aes_gcm_(plaintext, plaintext_len, out);
                break;
            case TlsRecordFormat::Tls12Chacha20:
                written = encrypt_tls12_chacha20_(plaintext, plaintext_len, out);
                break;
        }
        if (written == 0) return 0;

        seq_++;

        if (seq_ >= tls_record::kMaxSequenceNumber) [[unlikely]] {
            EPH_LOG_ERROR(detail::tls_enc_logger(),
                "TLS write sequence exhausted ({}/{}): must reconnect immediately",
                seq_, tls_record::kMaxSequenceNumber);
        } else if (seq_ == tls_record::kSequenceWarnThreshold) [[unlikely]] {
            EPH_LOG_WARN(detail::tls_enc_logger(),
                "TLS write sequence approaching limit ({}/{})",
                seq_, tls_record::kMaxSequenceNumber);
        }

        return written;
    }

    /// Compute the wire-record size for the given plaintext length under
    /// this encryptor's negotiated record format.
    [[nodiscard]] uint16_t encrypted_size(uint16_t plaintext_len) const noexcept {
        return ::eph::net::encrypted_size_for(record_format_, plaintext_len);
    }

    /// Format-explicit static helper. Used by tests / static_asserts /
    /// callers without a `TlsEncryptor` instance.
    [[nodiscard]] static constexpr uint16_t encrypted_size_for(
            TlsRecordFormat fmt, uint16_t plaintext_len) noexcept {
        return ::eph::net::encrypted_size_for(fmt, plaintext_len);
    }

    /// Current write sequence number (monotonically increasing).
    /// @return Number of records encrypted so far
    [[nodiscard]] uint64_t write_seq() const noexcept { return seq_; }

    /// Negotiated record format (read-only after construction).
    [[nodiscard]] TlsRecordFormat record_format() const noexcept {
        return record_format_;
    }

private:
    TlsEncryptor() = default;

    /// TLS 1.3 record (RFC 8446 §5.2). Plaintext is extended with the
    /// inner content-type byte before encryption; AAD is the 5-byte
    /// record header. Wire = header‖ciphertext+tag.
    [[nodiscard]] uint16_t encrypt_tls13_(const uint8_t* plaintext,
                                          uint16_t plaintext_len,
                                          uint8_t* out) noexcept {
        const uint16_t inner_len     = plaintext_len + 1;
        const uint16_t encrypted_len = inner_len + tls_record::kAuthTagLen;

        tls_record::write_record_header(out, tls_record::kContentTypeAppData,
                                         encrypted_len);

        uint8_t nonce[tls_const::kTls13NonceLen];
        tls_record::build_nonce(nonce, iv_, seq_);

        // Build inner plaintext (payload + content type byte) directly in
        // the output buffer region that will hold ciphertext. aws-lc
        // EVP_AEAD_CTX_seal supports in-place (out == in), so we avoid a
        // separate 16 KB stack buffer.
        uint8_t* cipher_region = out + tls_record::kRecordHeaderLen;
        if (plaintext_len > 0) {
            std::memcpy(cipher_region, plaintext, plaintext_len);
        }
        cipher_region[plaintext_len] = tls_record::kContentTypeAppData;

        size_t ciphertext_len = 0;
        const bool ok = EVP_AEAD_CTX_seal(
            &ctx_, cipher_region, &ciphertext_len,
            inner_len + tls_record::kAuthTagLen,
            nonce, tls_const::kTls13NonceLen,
            cipher_region, inner_len,
            out, tls_record::kRecordHeaderLen);
        if (!ok) {
            log_seal_failure_("tls13", plaintext_len);
            return 0;
        }
        return tls_record::kRecordHeaderLen + static_cast<uint16_t>(ciphertext_len);
    }

    /// TLS 1.2 AES-GCM record (RFC 5288). 8B explicit nonce on the wire
    /// (= seq big-endian); 13B AAD = seq‖type‖version‖plaintext_len; no
    /// inner content type. Wire = header‖explicit_nonce‖ciphertext+tag.
    [[nodiscard]] uint16_t encrypt_tls12_aes_gcm_(const uint8_t* plaintext,
                                                   uint16_t plaintext_len,
                                                   uint8_t* out) noexcept {
        // length-on-wire = explicit_nonce + plaintext + tag (AAD's length
        // field carries the plaintext-only length, see build_aad below).
        const uint16_t wire_payload_len =
            tls_record_v12::kExplicitNonceLen + plaintext_len + tls_record::kAuthTagLen;
        tls_record::write_record_header(out, tls_record::kContentTypeAppData,
                                         wire_payload_len);

        // explicit_nonce on wire is the 8-byte big-endian sequence number.
        uint64_t seq_be;
        if constexpr (std::endian::native == std::endian::little) {
            seq_be = std::byteswap(seq_);
        } else {
            seq_be = seq_;
        }
        uint8_t* explicit_nonce_dst = out + tls_record::kRecordHeaderLen;
        std::memcpy(explicit_nonce_dst, &seq_be, 8);

        uint8_t nonce[tls_const::kTls13NonceLen];
        tls_record_v12::build_nonce_aes_gcm(nonce, iv_, seq_);

        uint8_t aad[tls_record_v12::kAadLen];
        tls_record_v12::build_aad(aad, seq_,
                                   tls_record::kContentTypeAppData,
                                   plaintext_len);

        // Ciphertext (with 16B tag) goes after the explicit nonce.
        // In-place write: copy plaintext to its destination first, then seal.
        uint8_t* cipher_region = explicit_nonce_dst + tls_record_v12::kExplicitNonceLen;
        if (plaintext_len > 0) {
            std::memcpy(cipher_region, plaintext, plaintext_len);
        }

        size_t ciphertext_len = 0;
        const bool ok = EVP_AEAD_CTX_seal(
            &ctx_, cipher_region, &ciphertext_len,
            plaintext_len + tls_record::kAuthTagLen,
            nonce, tls_const::kTls13NonceLen,
            cipher_region, plaintext_len,
            aad, tls_record_v12::kAadLen);
        if (!ok) {
            log_seal_failure_("tls12-aes-gcm", plaintext_len);
            return 0;
        }
        return tls_record::kRecordHeaderLen +
               tls_record_v12::kExplicitNonceLen +
               static_cast<uint16_t>(ciphertext_len);
    }

    /// TLS 1.2 ChaCha20-Poly1305 record (RFC 7905). No explicit nonce on
    /// wire (nonce derived via TLS 1.3-style XOR of static IV with seq);
    /// 13B AAD = seq‖type‖version‖plaintext_len; no inner content type.
    /// Wire = header‖ciphertext+tag.
    [[nodiscard]] uint16_t encrypt_tls12_chacha20_(const uint8_t* plaintext,
                                                    uint16_t plaintext_len,
                                                    uint8_t* out) noexcept {
        const uint16_t wire_payload_len = plaintext_len + tls_record::kAuthTagLen;
        tls_record::write_record_header(out, tls_record::kContentTypeAppData,
                                         wire_payload_len);

        uint8_t nonce[tls_const::kTls13NonceLen];
        tls_record_v12::build_nonce_chacha20(nonce, iv_, seq_);

        uint8_t aad[tls_record_v12::kAadLen];
        tls_record_v12::build_aad(aad, seq_,
                                   tls_record::kContentTypeAppData,
                                   plaintext_len);

        uint8_t* cipher_region = out + tls_record::kRecordHeaderLen;
        if (plaintext_len > 0) {
            std::memcpy(cipher_region, plaintext, plaintext_len);
        }

        size_t ciphertext_len = 0;
        const bool ok = EVP_AEAD_CTX_seal(
            &ctx_, cipher_region, &ciphertext_len,
            plaintext_len + tls_record::kAuthTagLen,
            nonce, tls_const::kTls13NonceLen,
            cipher_region, plaintext_len,
            aad, tls_record_v12::kAadLen);
        if (!ok) {
            log_seal_failure_("tls12-chacha20", plaintext_len);
            return 0;
        }
        return tls_record::kRecordHeaderLen +
               static_cast<uint16_t>(ciphertext_len);
    }

    /// Drain the OpenSSL error queue and emit an actionable log line.
    void log_seal_failure_(const char* fmt_label, uint16_t plaintext_len) const noexcept {
        char err_buf[256] = {0};
        const unsigned long err_code = ERR_get_error();
        if (err_code != 0) {
            ERR_error_string_n(err_code, err_buf, sizeof(err_buf));
        }
        EPH_LOG_ERROR(detail::tls_enc_logger(),
            "EVP_AEAD_CTX_seal failed: format={} plaintext_len={} seq={} "
            "openssl_err=0x{:08x} ({})",
            fmt_label, plaintext_len, seq_, err_code,
            err_buf[0] ? err_buf : "no error in queue");
        while (ERR_get_error() != 0) { /* drain */ }
    }

    EVP_AEAD_CTX     ctx_{};
    bool             init_ = false;
    uint8_t          iv_[tls_const::kTls13NonceLen]{};
    uint64_t         seq_ = 0;
    TlsRecordFormat  record_format_ = TlsRecordFormat::Tls13;
};

} // namespace eph::net
