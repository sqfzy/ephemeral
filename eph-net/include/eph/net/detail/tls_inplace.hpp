#pragma once

/// @file tls_inplace.hpp
/// In-place AEAD decrypt helper — the zero-copy primitive behind the DPDK
/// `PacketView` design. Handles TLS 1.3, TLS 1.2 AES-GCM, and TLS 1.2
/// ChaCha20-Poly1305 records uniformly.
///
/// ## Why this exists
///
/// The headline performance feature is that a DPDK PacketView (`MbufView`)
/// hands the codec a writable pointer directly into a received mbuf. For
/// TLS-wrapped connections we want the decrypted plaintext to land **in
/// the same mbuf bytes** the NIC DMA'd into — no intermediate plaintext
/// buffer, no memcpy.
///
/// aws-lc / BoringSSL's AEAD (`EVP_AEAD_CTX_open`) supports this mode
/// for both AES-GCM and ChaCha20-Poly1305: the `in` and `out` pointers
/// may be identical. The auth tag is verified before the plaintext
/// overwrites the ciphertext, so a tag mismatch leaves `out` unspecified
/// but authentically-distinct from the ciphertext (the caller MUST check
/// the return value and must not read `out` on failure).
///
/// This helper lives in `eph-net` so both `eph-net-kernel` and
/// `eph-net-dpdk` can share one in-place AEAD wrapper without duplicating
/// the (subtle) per-version nonce / AAD construction and sequence-number
/// bookkeeping.
///
/// ## Wire layouts handled
///
///   TLS 1.3                 [hdr(5)] [ciphertext+inner_type] [tag(16)]
///   TLS 1.2 AES-GCM         [hdr(5)] [explicit_nonce(8)] [ciphertext] [tag(16)]
///   TLS 1.2 CHACHA20        [hdr(5)] [ciphertext] [tag(16)]
///
/// After successful decrypt:
///   - TLS 1.3 / 1.2 CHACHA20: `*plaintext_len` plaintext bytes start at `buf + 5`
///   - TLS 1.2 AES-GCM:        `*plaintext_len` plaintext bytes start at `buf + 13`
///                             (header + explicit_nonce; both are kept untouched)
///
/// On failure, `buf` is left in an unspecified state and must be discarded.
///
/// This header is deliberately lightweight: it does NOT manage keys,
/// contexts, or sequence numbers. Those live in the Stream's `TlsState`.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <type_traits>

#include <openssl/aead.h>
#include <openssl/err.h>     // ERR_get_error  (queue-drain on AEAD failure)
#include <openssl/mem.h>     // OPENSSL_cleanse

#include "eph/core/error.hpp"
#include "eph/net/detail/tls_constants.hpp"

namespace eph::net::detail {

/// In-place AEAD decryptor handling TLS 1.3, 1.2 AES-GCM, 1.2 ChaCha20-Poly1305.
///
/// Holds an `EVP_AEAD_CTX`, the per-direction static IV (12B for 1.3 /
/// CHACHA20, or 4B implicit IV in `iv_[0..3]` for 1.2 AES-GCM), the read
/// sequence number, and the negotiated record format. Decrypts a full
/// TLS record in place: the header (and, for AES-GCM-1.2, the 8B explicit
/// nonce) is kept untouched; the ciphertext+tag region is rewritten with
/// plaintext starting at the format-specific offset.
///
/// Thread safety: NOT thread-safe. Exactly one thread must own this
/// object — eph Stream implementations are single-threaded from the
/// RX lcore / epoll thread, so this matches.
class TlsInPlaceDecryptor {
public:
    /// Construct from raw key material + record format.
    ///
    /// @param key      AES-128 (16B) / AES-256 (32B) / CHACHA20 (32B) key
    /// @param key_len  Length of `key`
    /// @param iv       Per-direction IV. 12B for `Tls13` and `Tls12Chacha20`;
    ///                 the first 4 bytes are the implicit_IV for `Tls12AesGcm`
    ///                 (the upper 8 bytes of the buffer are unused but copied).
    /// @param iv_len   Must be 12 (the buffer always carries 12B; the format
    ///                 dictates how many of those bytes are meaningful).
    /// @param seq      Initial read sequence number.
    /// @param fmt      Negotiated record format (drives the open_in_place
    ///                 branch).
    [[nodiscard]] static std::expected<TlsInPlaceDecryptor, ::eph::core::ErrorInfo>
    create(const uint8_t* key, std::size_t key_len,
           const uint8_t* iv,  std::size_t iv_len,
           uint64_t seq,
           ::eph::net::TlsRecordFormat fmt =
               ::eph::net::TlsRecordFormat::Tls13) noexcept {
        if (iv_len != 12) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "TlsInPlaceDecryptor::create: iv_len must be 12"});
        }
        const EVP_AEAD* aead = nullptr;
        if (fmt == ::eph::net::TlsRecordFormat::Tls12Chacha20) {
            if (key_len != 32) {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::InvalidConfig,
                    "TlsInPlaceDecryptor::create: CHACHA20 requires 32B key"});
            }
            aead = EVP_aead_chacha20_poly1305();
        } else {
            if (key_len == 16) {
                aead = EVP_aead_aes_128_gcm();
            } else if (key_len == 32) {
                aead = EVP_aead_aes_256_gcm();
            } else {
                return std::unexpected(::eph::core::ErrorInfo{
                    ::eph::core::Error::InvalidConfig,
                    "TlsInPlaceDecryptor::create: key_len must be 16 or 32"});
            }
        }

        TlsInPlaceDecryptor dec;
        constexpr std::size_t kAuthTagLen = 16;
        if (!EVP_AEAD_CTX_init(&dec.ctx_, aead, key, key_len,
                                kAuthTagLen, nullptr)) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsInPlaceDecryptor::create: EVP_AEAD_CTX_init failed"});
        }
        dec.init_          = true;
        std::memcpy(dec.iv_, iv, 12);
        dec.seq_           = seq;
        dec.record_format_ = fmt;
        return dec;
    }

    ~TlsInPlaceDecryptor() {
        if (init_) EVP_AEAD_CTX_cleanup(&ctx_);
        OPENSSL_cleanse(iv_, sizeof(iv_));
    }

    TlsInPlaceDecryptor(const TlsInPlaceDecryptor&)            = delete;
    TlsInPlaceDecryptor& operator=(const TlsInPlaceDecryptor&) = delete;

    TlsInPlaceDecryptor(TlsInPlaceDecryptor&& other) noexcept
        : ctx_(other.ctx_), init_(other.init_), seq_(other.seq_),
          record_format_(other.record_format_) {
        std::memcpy(iv_, other.iv_, 12);
        other.init_ = false;
        // Cleanse moved-from secret state. Mirrors the move-assign below.
        OPENSSL_cleanse(&other.ctx_, sizeof(other.ctx_));
        OPENSSL_cleanse(other.iv_, sizeof(other.iv_));
    }
    TlsInPlaceDecryptor& operator=(TlsInPlaceDecryptor&& other) noexcept {
        if (this != &other) {
            if (init_) EVP_AEAD_CTX_cleanup(&ctx_);
            ctx_           = other.ctx_;
            init_          = other.init_;
            seq_           = other.seq_;
            record_format_ = other.record_format_;
            std::memcpy(iv_, other.iv_, 12);
            OPENSSL_cleanse(&other.ctx_, sizeof(other.ctx_));
            other.init_ = false;
            OPENSSL_cleanse(other.iv_, sizeof(other.iv_));
        }
        return *this;
    }

    TlsInPlaceDecryptor() = default;

    /// Decrypt one full TLS record in place.
    ///
    /// @param buf       Pointer to the start of the TLS record.
    /// @param record_len  Full record length (header + body + tag).
    /// @param[out] plaintext_len  Plaintext length on success.
    /// @param[out] inner_ct  TLS 1.3 inner content type (last byte of
    ///                  decrypted plaintext) — 23 = app_data, 22 = handshake
    ///                  (NewSessionTicket), 21 = alert. For TLS 1.2 the
    ///                  record header carries the real content type and
    ///                  this is set to that header byte for symmetry.
    ///                  May be nullptr.
    /// @return true on success; false on auth tag mismatch or malformed input.
    [[nodiscard]] bool
    open_in_place(uint8_t* buf, std::size_t record_len,
                   std::size_t* plaintext_len,
                   uint8_t* inner_ct = nullptr) noexcept {
        if (!buf || !plaintext_len) return false;
        constexpr std::size_t kHdr = 5;
        constexpr std::size_t kTag = 16;

        // Sequence cap — same rationale as TlsDecryptor: under AES-GCM,
        // nonce reuse after seq overflow is catastrophic. Fail closed.
        constexpr uint64_t kMaxSeq = 1ULL << 32;
        if (seq_ >= kMaxSeq) [[unlikely]] return false;

        const uint8_t content_type = buf[0];

        switch (record_format_) {
            case ::eph::net::TlsRecordFormat::Tls13:
                return open_tls13_(buf, record_len, kHdr, kTag,
                                   plaintext_len, inner_ct);
            case ::eph::net::TlsRecordFormat::Tls12AesGcm:
                return open_tls12_aes_gcm_(buf, record_len, kHdr, kTag,
                                           content_type,
                                           plaintext_len, inner_ct);
            case ::eph::net::TlsRecordFormat::Tls12Chacha20:
                return open_tls12_chacha20_(buf, record_len, kHdr, kTag,
                                             content_type,
                                             plaintext_len, inner_ct);
        }
        return false;
    }

    /// Accessor — current read sequence number.
    [[nodiscard]] uint64_t read_seq() const noexcept { return seq_; }
    /// Accessor — negotiated record format.
    [[nodiscard]] ::eph::net::TlsRecordFormat record_format() const noexcept {
        return record_format_;
    }

private:
    [[nodiscard]] bool open_tls13_(uint8_t* buf, std::size_t record_len,
                                   std::size_t kHdr, std::size_t kTag,
                                   std::size_t* plaintext_len,
                                   uint8_t* inner_ct) noexcept {
        if (record_len < kHdr + kTag + 1) return false;
        const std::size_t payload_len = record_len - kHdr;

        // Per-record nonce: iv XOR seq_be (RFC 8446 §5.3).
        uint8_t nonce[12];
        std::memcpy(nonce, iv_, 4);
        uint64_t iv_tail;
        std::memcpy(&iv_tail, iv_ + 4, 8);
        const uint64_t seq_be = std::byteswap(seq_);
        const uint64_t nonce_tail = iv_tail ^ seq_be;
        std::memcpy(nonce + 4, &nonce_tail, 8);

        std::size_t out_len = 0;
        if (!EVP_AEAD_CTX_open(&ctx_, buf + kHdr, &out_len,
                                payload_len - kTag,
                                nonce, 12,
                                buf + kHdr, payload_len,
                                buf, kHdr)) {
            while (ERR_get_error() != 0) { /* drain */ }
            return false;
        }
        // TLS 1.3 inner content-type byte trails the plaintext.
        if (out_len == 0) return false;
        const uint8_t ct = buf[kHdr + out_len - 1];
        out_len--;

        *plaintext_len = out_len;
        if (inner_ct) *inner_ct = ct;
        ++seq_;
        return true;
    }

    [[nodiscard]] bool open_tls12_aes_gcm_(uint8_t* buf, std::size_t record_len,
                                           std::size_t kHdr, std::size_t kTag,
                                           uint8_t content_type,
                                           std::size_t* plaintext_len,
                                           uint8_t* inner_ct) noexcept {
        // payload = explicit_nonce(8) + ciphertext + tag(16). Need at least
        // 8 + 16 = 24B of payload to be well-formed.
        const std::size_t kExpNonce = ::eph::net::tls_record_v12::kExplicitNonceLen;
        const std::size_t kImpIv    = ::eph::net::tls_record_v12::kImplicitIvLen;
        if (record_len < kHdr + kExpNonce + kTag) return false;

        const std::size_t payload_len   = record_len - kHdr;
        const std::size_t ct_and_tag_len = payload_len - kExpNonce;
        const std::size_t pt_len         = ct_and_tag_len - kTag;

        // 12B nonce = 4B implicit_IV (iv_[0..3]) ‖ 8B explicit_nonce on wire.
        uint8_t nonce[12];
        std::memcpy(nonce, iv_, kImpIv);
        std::memcpy(nonce + kImpIv, buf + kHdr, kExpNonce);

        // 13B AAD = seq‖type‖version‖plaintext_len (RFC 5246 §6.2.3.3).
        uint8_t aad[::eph::net::tls_record_v12::kAadLen];
        ::eph::net::tls_record_v12::build_aad(aad, seq_, content_type,
                                                static_cast<uint16_t>(pt_len));

        // Ciphertext+tag region begins after header+explicit_nonce; plaintext
        // overwrites the ciphertext bytes, which start at buf + 13.
        uint8_t* ct_region = buf + kHdr + kExpNonce;
        std::size_t out_len = 0;
        if (!EVP_AEAD_CTX_open(&ctx_, ct_region, &out_len, pt_len,
                                nonce, 12,
                                ct_region, ct_and_tag_len,
                                aad, sizeof(aad))) {
            while (ERR_get_error() != 0) { /* drain */ }
            return false;
        }
        *plaintext_len = out_len;
        // TLS 1.2 carries the true content type in the record header.
        if (inner_ct) *inner_ct = content_type;
        ++seq_;
        return true;
    }

    [[nodiscard]] bool open_tls12_chacha20_(uint8_t* buf, std::size_t record_len,
                                             std::size_t kHdr, std::size_t kTag,
                                             uint8_t content_type,
                                             std::size_t* plaintext_len,
                                             uint8_t* inner_ct) noexcept {
        if (record_len < kHdr + kTag) return false;
        const std::size_t payload_len = record_len - kHdr;
        const std::size_t pt_len      = payload_len - kTag;

        // 12B nonce: same construction as TLS 1.3 (RFC 7905 §2 reused the
        // shape). build_nonce_chacha20 = static_iv XOR seq_padded.
        uint8_t nonce[12];
        ::eph::net::tls_record_v12::build_nonce_chacha20(nonce, iv_, seq_);

        uint8_t aad[::eph::net::tls_record_v12::kAadLen];
        ::eph::net::tls_record_v12::build_aad(aad, seq_, content_type,
                                                static_cast<uint16_t>(pt_len));

        std::size_t out_len = 0;
        if (!EVP_AEAD_CTX_open(&ctx_, buf + kHdr, &out_len, pt_len,
                                nonce, 12,
                                buf + kHdr, payload_len,
                                aad, sizeof(aad))) {
            while (ERR_get_error() != 0) { /* drain */ }
            return false;
        }
        *plaintext_len = out_len;
        if (inner_ct) *inner_ct = content_type;
        ++seq_;
        return true;
    }

    EVP_AEAD_CTX                ctx_{};
    bool                        init_ = false;
    uint8_t                     iv_[12]{};
    uint64_t                    seq_ = 0;
    ::eph::net::TlsRecordFormat record_format_ =
        ::eph::net::TlsRecordFormat::Tls13;
};

// aws-lc's EVP_AEAD_CTX is a flat POD for AES-GCM and CHACHA20-POLY1305.
// If the invariant ever breaks, the move constructor's bitwise copy
// would leak state.
static_assert(std::is_trivially_copyable_v<EVP_AEAD_CTX>,
    "EVP_AEAD_CTX is no longer trivially copyable — bitwise move is unsafe");

} // namespace eph::net::detail
