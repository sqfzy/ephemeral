#pragma once

/// @file tls_record.hpp
/// TLS record layer — AEAD encryption/decryption via aws-lc.
///
/// This file provides:
///   - `tls_record` / `tls_record_v12` namespaces: constants, nonce builders,
///     header parsing, AAD construction (via `tls_constants.hpp`)
///   - `TlsEncryptor`: write-direction AEAD (see `tls_encryptor.hpp`)
///   - `TlsDecryptor`: read-direction AEAD (see `tls_decryptor.hpp`)
///   - `TlsRecordCrypto`: backward-compatible composition of both
///
/// Three wire formats are supported, dispatched by `TlsRecordFormat`:
///
/// TLS 1.3 (`Tls13`, RFC 8446):
///   [ContentType(1)] [LegacyVersion=0x0303(2)] [Length(2)]
///   [Ciphertext+InnerType] [AuthTag(16)]
///
/// TLS 1.2 AES-GCM (`Tls12AesGcm`, RFC 5288):
///   [ContentType(1)] [Version=0x0303(2)] [Length(2)]
///   [ExplicitNonce(8)] [Ciphertext] [AuthTag(16)]
///
/// TLS 1.2 ChaCha20-Poly1305 (`Tls12Chacha20`, RFC 7905):
///   [ContentType(1)] [Version=0x0303(2)] [Length(2)]
///   [Ciphertext] [AuthTag(16)]

#include "eph/net/detail/tls_constants.hpp"
#include "eph/net/detail/tls_encryptor.hpp"
#include "eph/net/detail/tls_decryptor.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// TlsRecordCrypto — backward-compatible composition
// ─────────────────────────────────────────────────────────────────────────────

/// Backward-compatible composition of TlsEncryptor + TlsDecryptor.
///
/// Thread safety: enc.encrypt() and dec.decrypt() use separate AEAD contexts.
/// It is safe to call enc.encrypt() from one thread and dec.decrypt() from
/// another, but each individual method must not be called concurrently.
///
/// For new code preferring split ownership, use TlsEncryptor and TlsDecryptor
/// directly (e.g., kDirectTx mode gives encryptor to app thread).
struct TlsRecordCrypto {
    TlsEncryptor enc;
    TlsDecryptor dec;

    /// Create a TlsRecordCrypto by initializing both encryptor and decryptor.
    ///
    /// @param state    TLS hot state containing key material for both directions
    /// @param key_len  AES key length: 16 (AES-128) or 32 (AES-256, default)
    /// @return Initialized TlsRecordCrypto, or error string on failure
    [[nodiscard]] static std::expected<TlsRecordCrypto, std::string>
    create(const TlsHotState& state, size_t key_len = tls_const::kAes256KeyLen) {
        auto enc_result = TlsEncryptor::create(state, key_len);
        if (!enc_result) return std::unexpected(enc_result.error());

        auto dec_result = TlsDecryptor::create(state, key_len);
        if (!dec_result) return std::unexpected(dec_result.error());

        return TlsRecordCrypto{
            .enc = std::move(*enc_result),
            .dec = std::move(*dec_result),
        };
    }

    /// Encrypt plaintext into a TLS record (delegates to enc).
    /// @param plaintext     Input data
    /// @param plaintext_len Input length (must be <= kMaxRecordPayload)
    /// @param out           Output buffer (must have at least encrypted_size() bytes)
    /// @return Total bytes written to out, or 0 on error
    [[nodiscard]] uint16_t encrypt(const uint8_t* plaintext, uint16_t plaintext_len,
                     uint8_t* out) noexcept {
        return enc.encrypt(plaintext, plaintext_len, out);
    }

    /// Decrypt a TLS record (delegates to dec).
    /// @param record       Input TLS record (header + ciphertext + tag)
    /// @param record_len   Total record length
    /// @param out          Output buffer for decrypted plaintext
    /// @param[out] out_len Actual decrypted plaintext length
    /// @param[out] inner_ct  TLS 1.3 inner content type byte. May be nullptr.
    /// @return true on success, false on decryption/authentication failure
    [[nodiscard]] bool decrypt(const uint8_t* record, uint16_t record_len,
                 uint8_t* out, uint16_t& out_len,
                 uint8_t* inner_ct = nullptr) noexcept {
        return dec.decrypt(record, record_len, out, out_len, inner_ct);
    }

    /// Compute the wire-record size for the given plaintext length under
    /// the negotiated record format. Format-aware — delegates to the
    /// internal encryptor.
    [[nodiscard]] uint16_t encrypted_size(uint16_t plaintext_len) const noexcept {
        return enc.encrypted_size(plaintext_len);
    }

    /// Format-explicit static helper. Useful when there's no
    /// `TlsRecordCrypto` instance handy (e.g. unit-test buffer sizing
    /// or `static_assert`s on layout).
    [[nodiscard]] static constexpr uint16_t encrypted_size_for(
            TlsRecordFormat fmt, uint16_t plaintext_len) noexcept {
        return ::eph::net::encrypted_size_for(fmt, plaintext_len);
    }

    /// Current write-direction sequence number.
    [[nodiscard]] uint64_t write_seq() const noexcept { return enc.write_seq(); }
    /// Current read-direction sequence number.
    [[nodiscard]] uint64_t read_seq()  const noexcept { return dec.read_seq(); }
    /// Negotiated record format.
    [[nodiscard]] TlsRecordFormat record_format() const noexcept {
        return enc.record_format();
    }
};

} // namespace eph::net
