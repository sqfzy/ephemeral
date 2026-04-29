#pragma once

/// @file tls_inplace.hpp
/// In-place AES-GCM AEAD decrypt helper — the zero-copy primitive behind
/// the DPDK `PacketView` design.
///
/// ## Why this exists
///
/// The headline performance feature is that a DPDK PacketView
/// (`MbufView`) hands the codec a writable pointer directly into a received
/// mbuf. For TLS-wrapped connections we want the decrypted plaintext to
/// land **in the same mbuf bytes** the NIC DMA'd into — no intermediate
/// plaintext buffer, no memcpy.
///
/// aws-lc / BoringSSL's AES-GCM AEAD (`EVP_AEAD_CTX_open`) supports this
/// mode: the `in` and `out` pointers may be identical. The auth tag is
/// verified before the plaintext overwrites the ciphertext, so a tag
/// mismatch leaves `out` unspecified but authentically-distinct from the
/// ciphertext (the caller MUST check the return value and must not read
/// `out` on failure).
///
/// This helper lives in eph-core so both `eph-net-kernel` and
/// `eph-net-dpdk` can share one in-place AEAD wrapper without duplicating
/// the (subtle) TLS 1.3 nonce construction and sequence-number bookkeeping.
///
/// ## Contract
///
/// - Caller has already stripped / validated the 5-byte TLS record header
///   (the caller usually keeps it, because AES-GCM uses it as AAD).
/// - `buf` points at the full record: `[header(5)] [ciphertext+tag]`.
/// - After successful decrypt, `*plaintext_len` bytes of plaintext start at
///   `buf + 5` (the header bytes are untouched).
/// - On failure, `buf` is left in an unspecified state and must be
///   discarded.
///
/// This header is deliberately lightweight: it does NOT manage keys,
/// contexts, or sequence numbers. Those live in the Stream's `TlsState`.
/// It's a thin RAII holder plus `open_in_place()` call.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <type_traits>

#include <openssl/aead.h>
#include <openssl/mem.h>     // OPENSSL_cleanse

#include "eph/core/error.hpp"

namespace eph::net::detail {

/// @brief TLS 1.3 AES-GCM in-place decryptor.
///
/// Holds an `EVP_AEAD_CTX` plus the 12-byte static IV and monotonic read
/// sequence. Decrypts a full TLS record in place: the 5-byte header is
/// kept as-is (it's the AEAD AAD), and the ciphertext+tag is rewritten
/// with plaintext starting at `buf + 5`.
///
/// Thread safety: NOT thread-safe. Exactly one thread must own this
/// object. All eph Stream implementations are single-threaded from the
/// RX lcore / epoll thread, so this matches.
class TlsInPlaceDecryptor {
public:
    /// @brief Construct from raw key material (key, iv) + AES key length.
    ///
    /// The caller supplies 16 or 32 byte key (AES-128 vs AES-256), 12 byte
    /// IV, and the initial read sequence number. After construction the
    /// object is owned — move-only, non-copyable.
    ///
    /// Return: populated decryptor, or ErrorInfo on aws-lc init failure.
    [[nodiscard]] static std::expected<TlsInPlaceDecryptor, ::eph::core::ErrorInfo>
    create(const uint8_t* key, std::size_t key_len,
           const uint8_t* iv,  std::size_t iv_len,
           uint64_t seq) noexcept {
        if (iv_len != 12) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "TlsInPlaceDecryptor::create: iv_len must be 12 (AES-GCM)"});
        }
        const EVP_AEAD* aead = nullptr;
        if (key_len == 16) {
            aead = EVP_aead_aes_128_gcm();
        } else if (key_len == 32) {
            aead = EVP_aead_aes_256_gcm();
        } else {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "TlsInPlaceDecryptor::create: key_len must be 16 or 32"});
        }

        TlsInPlaceDecryptor dec;
        // kAuthTagLen is 16 for AES-GCM; hardcode here to avoid pulling in
        // the full tls_constants.hpp chain.
        constexpr std::size_t kAuthTagLen = 16;
        if (!EVP_AEAD_CTX_init(&dec.ctx_, aead, key, key_len,
                                kAuthTagLen, nullptr)) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed,
                "TlsInPlaceDecryptor::create: EVP_AEAD_CTX_init failed"});
        }
        dec.init_ = true;
        std::memcpy(dec.iv_, iv, 12);
        dec.seq_ = seq;
        return dec;
    }

    ~TlsInPlaceDecryptor() {
        if (init_) EVP_AEAD_CTX_cleanup(&ctx_);
        OPENSSL_cleanse(iv_, sizeof(iv_));
    }

    TlsInPlaceDecryptor(const TlsInPlaceDecryptor&)            = delete;
    TlsInPlaceDecryptor& operator=(const TlsInPlaceDecryptor&) = delete;

    TlsInPlaceDecryptor(TlsInPlaceDecryptor&& other) noexcept
        : ctx_(other.ctx_), init_(other.init_), seq_(other.seq_) {
        std::memcpy(iv_, other.iv_, 12);
        other.init_ = false;
        // Cleanse both halves of the secret state so the moved-from
        // instance does not retain expanded AES round keys (held inside
        // EVP_AEAD_CTX for aws-lc AES-GCM) or the static IV. The
        // move-assignment operator below already does this; the
        // move-ctor was previously asymmetric and left ctx_ intact.
        OPENSSL_cleanse(&other.ctx_, sizeof(other.ctx_));
        OPENSSL_cleanse(other.iv_, sizeof(other.iv_));
    }
    TlsInPlaceDecryptor& operator=(TlsInPlaceDecryptor&& other) noexcept {
        if (this != &other) {
            if (init_) EVP_AEAD_CTX_cleanup(&ctx_);
            ctx_ = other.ctx_;
            init_ = other.init_;
            seq_  = other.seq_;
            std::memcpy(iv_, other.iv_, 12);
            OPENSSL_cleanse(&other.ctx_, sizeof(other.ctx_));
            other.init_ = false;
            OPENSSL_cleanse(other.iv_, sizeof(other.iv_));
        }
        return *this;
    }

    TlsInPlaceDecryptor() = default;

    /// @brief Decrypt one full TLS record in place.
    ///
    /// @param buf       Pointer to the start of the TLS record
    ///                  (`[header(5)] [ciphertext(payload_len-16)] [tag(16)]`).
    ///                  On success the header is unchanged, and
    ///                  `*plaintext_len` bytes of plaintext start at `buf+5`.
    /// @param record_len  Full record length (header + ciphertext + tag).
    /// @param[out] plaintext_len  Plaintext length on success (never counts
    ///                  the TLS 1.3 inner content-type trailing byte, which
    ///                  is stripped).
    /// @param[out] inner_ct  TLS 1.3 inner content type (the last byte of
    ///                  the decrypted plaintext). 23 = application_data,
    ///                  22 = handshake (e.g. NewSessionTicket), 21 = alert.
    ///                  May be nullptr if the caller does not need it.
    /// @return true on success, false on auth tag mismatch or malformed input.
    [[nodiscard]] bool
    open_in_place(uint8_t* buf, std::size_t record_len,
                   std::size_t* plaintext_len,
                   uint8_t* inner_ct = nullptr) noexcept {
        if (!buf || !plaintext_len) return false;
        constexpr std::size_t kHdr  = 5;
        constexpr std::size_t kTag  = 16;
        if (record_len < kHdr + kTag + 1) return false;

        const std::size_t payload_len = record_len - kHdr;

        // Build per-record nonce: first 4 bytes of iv || (iv[4..12] XOR seq_be).
        uint8_t nonce[12];
        std::memcpy(nonce, iv_, 4);
        uint64_t iv_tail;
        std::memcpy(&iv_tail, iv_ + 4, 8);
        // XOR big-endian sequence number into the tail (RFC 8446 §5.3).
        uint64_t seq_be = std::byteswap(seq_);
        uint64_t nonce_tail = iv_tail ^ seq_be;
        std::memcpy(nonce + 4, &nonce_tail, 8);

        std::size_t out_len = 0;
        // AAD is the 5-byte record header. Input and output buffers both
        // point at `buf + kHdr`: aws-lc / BoringSSL AES-GCM supports this.
        if (!EVP_AEAD_CTX_open(&ctx_,
                                /*out=*/buf + kHdr,
                                &out_len,
                                /*max_out_len=*/payload_len - kTag,
                                nonce, 12,
                                /*in=*/buf + kHdr,
                                /*in_len=*/payload_len,
                                /*ad=*/buf, kHdr)) {
            return false;
        }

        // TLS 1.3: the last byte of decrypted plaintext is the inner
        // content-type (RFC 8446 §5.2). Strip it and optionally report.
        if (out_len == 0) return false;
        const uint8_t ct = buf[kHdr + out_len - 1];
        out_len--;

        *plaintext_len = out_len;
        if (inner_ct) *inner_ct = ct;
        ++seq_;
        return true;
    }

    /// @brief Accessor for unit tests — current read sequence.
    [[nodiscard]] uint64_t read_seq() const noexcept { return seq_; }

private:
    EVP_AEAD_CTX ctx_{};
    bool         init_ = false;
    uint8_t      iv_[12]{};
    uint64_t     seq_ = 0;
};

// aws-lc's EVP_AEAD_CTX is a flat POD for AES-GCM (same invariant the
// legacy TlsDecryptor relies on). If the invariant ever breaks, the move
// constructor's bitwise copy would leak state.
static_assert(std::is_trivially_copyable_v<EVP_AEAD_CTX>,
    "EVP_AEAD_CTX is no longer trivially copyable — bitwise move is unsafe");

} // namespace eph::net::detail
