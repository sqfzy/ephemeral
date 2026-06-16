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

#include "eph/core/log.hpp"

#include <openssl/aead.h>
#include <openssl/err.h>
#include <openssl/mem.h>

#include "eph/net/detail/tls_constants.hpp"

namespace eph::net {

namespace detail {
/// Lazily-initialized logger for TLS decryption operations.
/// @return Pointer to the "transport.tls_dec" spdlog logger.
inline spdlog::logger* tls_dec_logger() {
    static spdlog::logger* l = ::eph::log::get("transport.tls_dec");
    return l;
}
} // namespace detail

/// AEAD record-level decryption (read direction only).
///
/// Owns the decryption AEAD context, read IV, sequence number, and
/// record format. Created from TlsHotState after handshake key export.
/// Branches on `record_format_` once per record to pick the right
/// nonce/AAD/wire-layout — the branch is fully predictable per session.
class TlsDecryptor {
public:
    /// Create a TlsDecryptor from the read-direction key material in a TlsHotState.
    ///
    /// AEAD selection mirrors `TlsEncryptor::create`:
    ///   - `state.record_format == Tls12Chacha20` → CHACHA20-POLY1305 (key_len must be 32)
    ///   - else → AES-128/256-GCM by `key_len`
    [[nodiscard]] static std::expected<TlsDecryptor, std::string>
    create(const TlsHotState& state, size_t key_len = tls_const::kAes256KeyLen) {
        TlsDecryptor dec;

        const EVP_AEAD* aead = nullptr;
        if (state.record_format == TlsRecordFormat::Tls12Chacha20) {
            if (key_len != 32) {
                return std::unexpected(std::format(
                    "CHACHA20-POLY1305 requires 32-byte key, got {}", key_len));
            }
            aead = EVP_aead_chacha20_poly1305();
        } else {
            if (key_len == 16) {
                aead = EVP_aead_aes_128_gcm();
            } else if (key_len == 32) {
                aead = EVP_aead_aes_256_gcm();
            } else {
                return std::unexpected(std::format(
                    "Unsupported AES key length: {}", key_len));
            }
        }

        if (!EVP_AEAD_CTX_init(&dec.ctx_, aead,
                               state.read.ki.key, key_len,
                               tls_record::kAuthTagLen, nullptr)) {
            return std::unexpected("EVP_AEAD_CTX_init failed for decryption");
        }
        dec.init_ = true;

        std::memcpy(dec.iv_, state.read.ki.iv, tls_const::kTls13NonceLen);
        dec.seq_           = state.read.seq;
        dec.record_format_ = state.record_format;

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

    TlsDecryptor& operator=(TlsDecryptor&& other) noexcept {
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

    /// Decrypt a TLS record.
    /// @param record       Input TLS record (header + encrypted data + tag)
    /// @param record_len   Total record length
    /// @param out          Output buffer for decrypted plaintext
    /// @param[out] out_len Actual decrypted plaintext length (excluding inner type)
    /// @param[out] inner_ct  TLS 1.3 inner content type byte (out param). For
    ///                       TLS 1.2 the inner content type is not present in
    ///                       plaintext — the record header carries the real
    ///                       type, which this method writes through here for
    ///                       symmetry with the 1.3 path. May be nullptr.
    /// @return true on success, false on decryption/authentication failure
    [[nodiscard]] bool decrypt(const uint8_t* record, uint16_t record_len,
                 uint8_t* out, uint16_t& out_len,
                 uint8_t* inner_ct = nullptr) noexcept {
        if (!record || !out) [[unlikely]] {
            EPH_LOG_ERROR(detail::tls_dec_logger(),
                "decrypt: null pointer — record={}, out={}",
                static_cast<const void*>(record), static_cast<void*>(out));
            return false;
        }
        if (record_len < tls_record::kRecordHeaderLen + tls_record::kAuthTagLen) {
            EPH_LOG_DEBUG(detail::tls_dec_logger(),
                "decrypt: record too short: {} < {}",
                record_len, tls_record::kRecordHeaderLen + tls_record::kAuthTagLen);
            return false;
        }

        if (seq_ >= tls_record::kMaxSequenceNumber) {
            EPH_LOG_ERROR(detail::tls_dec_logger(),
                "TLS read sequence limit reached ({}): must reconnect", seq_);
            return false;
        }

        uint8_t content_type;
        uint16_t payload_len;
        if (!tls_record::parse_record_header(record, content_type, payload_len)) {
            EPH_LOG_WARN(detail::tls_dec_logger(),
                "decrypt: invalid record header — content_type=0x{:02X}, "
                "payload_len={}, record_len={}",
                content_type, payload_len, record_len);
            return false;
        }

        if (tls_record::kRecordHeaderLen + payload_len > record_len) {
            EPH_LOG_DEBUG(detail::tls_dec_logger(),
                "decrypt: record truncated: header+payload={} > record_len={}",
                tls_record::kRecordHeaderLen + payload_len, record_len);
            return false;
        }

        bool ok = false;
        switch (record_format_) {
            case TlsRecordFormat::Tls13:
                ok = decrypt_tls13_(record, payload_len, out, out_len, inner_ct);
                break;
            case TlsRecordFormat::Tls12AesGcm:
                ok = decrypt_tls12_aes_gcm_(record, payload_len, out, out_len,
                                             content_type, inner_ct);
                break;
            case TlsRecordFormat::Tls12Chacha20:
                ok = decrypt_tls12_chacha20_(record, payload_len, out, out_len,
                                              content_type, inner_ct);
                break;
        }
        if (!ok) return false;

        seq_++;
        if (seq_ >= tls_record::kMaxSequenceNumber) [[unlikely]] {
            EPH_LOG_ERROR(detail::tls_dec_logger(),
                "TLS read sequence exhausted ({}/{}): must reconnect immediately",
                seq_, tls_record::kMaxSequenceNumber);
        } else if (seq_ == tls_record::kSequenceWarnThreshold) [[unlikely]] {
            EPH_LOG_WARN(detail::tls_dec_logger(),
                "TLS read sequence approaching limit ({}/{})",
                seq_, tls_record::kMaxSequenceNumber);
        }
        return true;
    }

    /// Current read sequence number (monotonically increasing).
    /// @return Number of records decrypted so far
    [[nodiscard]] uint64_t read_seq() const noexcept { return seq_; }

    /// Negotiated record format (read-only after construction).
    [[nodiscard]] TlsRecordFormat record_format() const noexcept {
        return record_format_;
    }

private:
    TlsDecryptor() = default;

    /// TLS 1.3 record decrypt. AAD = 5B header, nonce = iv XOR seq, plaintext
    /// trails an inner content-type byte (RFC 8446 §5.2).
    [[nodiscard]] bool decrypt_tls13_(const uint8_t* record, uint16_t payload_len,
                                      uint8_t* out, uint16_t& out_len,
                                      uint8_t* inner_ct) noexcept {
        if (payload_len < tls_record::kAuthTagLen + 1) {
            EPH_LOG_DEBUG(detail::tls_dec_logger(),
                "decrypt: TLS 1.3 record too short: payload_len={} < min {}",
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
            log_open_failure_("tls13", payload_len);
            return false;
        }

        // TLS 1.3 spec violation if AEAD output is empty — the trailing
        // inner content-type byte MUST be present. Reject defensively.
        if (plaintext_len == 0) [[unlikely]] {
            EPH_LOG_ERROR(detail::tls_dec_logger(),
                "decrypt: TLS 1.3 AEAD reported zero-length plaintext "
                "(seq={})", seq_);
            return false;
        }
        if (inner_ct) *inner_ct = out[plaintext_len - 1];
        out_len = static_cast<uint16_t>(plaintext_len - 1);
        return true;
    }

    /// TLS 1.2 AES-GCM record decrypt (RFC 5288). Read 8B explicit nonce
    /// from the record body, build the 12B nonce, AAD = 13B with
    /// plaintext_len, decrypt the remaining ciphertext+tag.
    [[nodiscard]] bool decrypt_tls12_aes_gcm_(const uint8_t* record,
                                               uint16_t payload_len,
                                               uint8_t* out, uint16_t& out_len,
                                               uint8_t content_type,
                                               uint8_t* inner_ct) noexcept {
        // payload = explicit_nonce(8B) + ciphertext + tag(16B)
        if (payload_len < tls_record_v12::kExplicitNonceLen + tls_record::kAuthTagLen) {
            EPH_LOG_DEBUG(detail::tls_dec_logger(),
                "decrypt: TLS 1.2 AES-GCM record too short: payload_len={} < min {}",
                payload_len,
                tls_record_v12::kExplicitNonceLen + tls_record::kAuthTagLen);
            return false;
        }

        const uint8_t* explicit_nonce = record + tls_record::kRecordHeaderLen;
        const uint8_t* ciphertext     = explicit_nonce + tls_record_v12::kExplicitNonceLen;
        const uint16_t ct_and_tag_len = payload_len - tls_record_v12::kExplicitNonceLen;
        const uint16_t pt_len         = ct_and_tag_len - tls_record::kAuthTagLen;

        // Construct the 12B nonce from our 4B implicit IV (first 4 bytes of iv_)
        // and the 8B explicit nonce on the wire. We deliberately do NOT use the
        // `seq_`-derived implicit nonce on the read path: a strict reader
        // honours whatever the peer sends, since some TLS 1.2 stacks use a
        // counter rather than seq for their explicit nonce. The AAD's
        // length field below uses `pt_len`, which is the only place seq
        // affects the read math.
        uint8_t nonce[tls_const::kTls13NonceLen];
        std::memcpy(nonce, iv_, tls_record_v12::kImplicitIvLen);
        std::memcpy(nonce + tls_record_v12::kImplicitIvLen, explicit_nonce,
                    tls_record_v12::kExplicitNonceLen);

        uint8_t aad[tls_record_v12::kAadLen];
        tls_record_v12::build_aad(aad, seq_, content_type, pt_len);

        size_t plaintext_len = 0;
        if (!EVP_AEAD_CTX_open(&ctx_, out, &plaintext_len, pt_len,
                                nonce, tls_const::kTls13NonceLen,
                                ciphertext, ct_and_tag_len,
                                aad, tls_record_v12::kAadLen)) {
            log_open_failure_("tls12-aes-gcm", payload_len);
            return false;
        }
        out_len = static_cast<uint16_t>(plaintext_len);
        // TLS 1.2 carries the real content type in the record header itself.
        // Surface it through `inner_ct` for callers that want a uniform
        // shape across versions.
        if (inner_ct) *inner_ct = content_type;
        return true;
    }

    /// TLS 1.2 ChaCha20-Poly1305 record decrypt (RFC 7905). Same nonce
    /// construction as TLS 1.3 (XOR), 13B AAD, no explicit nonce on wire.
    [[nodiscard]] bool decrypt_tls12_chacha20_(const uint8_t* record,
                                                uint16_t payload_len,
                                                uint8_t* out, uint16_t& out_len,
                                                uint8_t content_type,
                                                uint8_t* inner_ct) noexcept {
        if (payload_len < tls_record::kAuthTagLen) {
            EPH_LOG_DEBUG(detail::tls_dec_logger(),
                "decrypt: TLS 1.2 CHACHA20 record too short: payload_len={} < min {}",
                payload_len, tls_record::kAuthTagLen);
            return false;
        }
        const uint16_t pt_len = payload_len - tls_record::kAuthTagLen;

        const uint8_t* ciphertext = record + tls_record::kRecordHeaderLen;

        uint8_t nonce[tls_const::kTls13NonceLen];
        tls_record_v12::build_nonce_chacha20(nonce, iv_, seq_);

        uint8_t aad[tls_record_v12::kAadLen];
        tls_record_v12::build_aad(aad, seq_, content_type, pt_len);

        size_t plaintext_len = 0;
        if (!EVP_AEAD_CTX_open(&ctx_, out, &plaintext_len, pt_len,
                                nonce, tls_const::kTls13NonceLen,
                                ciphertext, payload_len,
                                aad, tls_record_v12::kAadLen)) {
            log_open_failure_("tls12-chacha20", payload_len);
            return false;
        }
        out_len = static_cast<uint16_t>(plaintext_len);
        if (inner_ct) *inner_ct = content_type;
        return true;
    }

    void log_open_failure_(const char* fmt_label, uint16_t payload_len) const noexcept {
        char err_buf[256] = {0};
        const unsigned long err_code = ERR_get_error();
        if (err_code != 0) {
            ERR_error_string_n(err_code, err_buf, sizeof(err_buf));
        }
        EPH_LOG_ERROR(detail::tls_dec_logger(),
            "EVP_AEAD_CTX_open failed: format={} payload_len={} seq={} "
            "openssl_err=0x{:08x} ({})",
            fmt_label, payload_len, seq_, err_code,
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
