#pragma once

/// @file ed25519_signer.hpp
/// @brief Ed25519 (EdDSA) request signing for venues that use asymmetric API
///        keys — Binance Spot's WebSocket API `session.logon` in particular.
///
/// ## Why this exists (separate from `hmac.hpp` / `jwt_signed_request.hpp`)
///
/// `hmac.hpp` covers the symmetric HMAC-SHA256 venues (Bybit, OKX, classic
/// Binance) and `jwt_signed_request.hpp` covers Coinbase's ES256 JWT. Binance's
/// newer key type is **Ed25519**: the signature payload is the request's
/// parameters sorted alphabetically and joined `key=value&key=value`, signed
/// with the Ed25519 private key, and the 64-byte signature transmitted as
/// **standard base64** (not base64url). This header provides exactly that.
///
/// Built on aws-lc's `EVP_DigestSign` one-shot API (the same crypto library the
/// TLS stack and `hmac.hpp` use). Ed25519 signing is deterministic (no nonce),
/// so the same payload always yields the same signature — handy for KATs.
///
/// ## Key material safety
///
/// `Ed25519PrivateKey` is move-only and frees the underlying `EVP_PKEY` (which
/// owns the secret scalar) on destruction. Loaded once per process from a PEM
/// file referenced by an environment variable — never embedded in config or
/// logged.

#include <array>
#include <cstdint>
#include <expected>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "eph/core/error.hpp"
#include "eph/core/log.hpp"

namespace eph::net {

namespace detail {

inline spdlog::logger* ed25519_signer_logger() {
    static spdlog::logger* l = ::eph::log::get("net.ed25519_signer");
    return l;
}

/// @brief Standard base64 encode (RFC 4648 alphabet, '=' padded). aws-lc does
///        not always export EVP_EncodeBlock, and the encode is trivial, so it is
///        inlined here. Binance requires standard base64 for the signature.
[[nodiscard]] inline std::string base64_encode(const uint8_t* p, std::size_t n) {
    static constexpr char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += T[v & 63];
    }
    if (n - i == 1) {
        const uint32_t v = uint32_t(p[i]) << 16;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "==";
    } else if (n - i == 2) {
        const uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '=';
    }
    return out;
}

} // namespace detail

/// @brief RAII Ed25519 private key (move-only) with `sign` / `sign_base64`.
class Ed25519PrivateKey {
public:
    /// @brief Ed25519 raw signature size (always 64 bytes).
    static constexpr std::size_t kSignatureSize = 64;

    /// @brief Load an Ed25519 private key from a PEM string (PKCS#8
    ///        `-----BEGIN PRIVATE KEY-----`). Rejects any non-Ed25519 key.
    [[nodiscard]] static std::expected<Ed25519PrivateKey, ::eph::core::ErrorInfo>
    from_pem(std::string_view pem) noexcept {
        if (pem.empty()) {
            EPH_LOG_ERROR(detail::ed25519_signer_logger(), "Ed25519PrivateKey::from_pem: empty PEM");
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig, "empty PEM"});
        }
        BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
        if (bio == nullptr) {
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::OutOfMemory, "BIO alloc"});
        }
        EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (pkey == nullptr) {
            EPH_LOG_ERROR(detail::ed25519_signer_logger(), "Ed25519PrivateKey::from_pem: PEM parse failed "
                         "(malformed / encrypted / unrecognized key type)");
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig, "PEM parse failed"});
        }
        const int id = EVP_PKEY_id(pkey);
        if (id != EVP_PKEY_ED25519) {
            EPH_LOG_ERROR(detail::ed25519_signer_logger(), "Ed25519PrivateKey::from_pem: key is not Ed25519 "
                         "(EVP_PKEY_id={})", id);
            EVP_PKEY_free(pkey);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig, "key is not Ed25519"});
        }
        EPH_LOG_DEBUG(detail::ed25519_signer_logger(), "Ed25519PrivateKey::from_pem: loaded Ed25519 key OK");
        Ed25519PrivateKey out;
        out.key_ = pkey;
        return out;
    }

    /// @brief Load an Ed25519 private key from a PEM file path.
    [[nodiscard]] static std::expected<Ed25519PrivateKey, ::eph::core::ErrorInfo>
    from_pem_file(const std::string& path) noexcept {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            EPH_LOG_ERROR(detail::ed25519_signer_logger(), "Ed25519PrivateKey::from_pem_file: cannot open '{}'", path);
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig, "cannot open PEM file"});
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return from_pem(ss.str());
    }

    /// @brief Sign `msg` with Ed25519; returns the raw 64-byte signature.
    [[nodiscard]] std::expected<std::array<uint8_t, kSignatureSize>, ::eph::core::ErrorInfo>
    sign(std::string_view msg) const noexcept {
        if (key_ == nullptr)
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig, "no key loaded"});
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (ctx == nullptr)
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::OutOfMemory, "EVP_MD_CTX_new"});
        std::array<uint8_t, kSignatureSize> sig{};
        std::size_t siglen = sig.size();
        // Ed25519 uses the one-shot EVP_DigestSign (no streaming Update).
        const bool ok =
            EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr,
                               static_cast<EVP_PKEY*>(key_)) == 1 &&
            EVP_DigestSign(ctx, sig.data(), &siglen,
                           reinterpret_cast<const uint8_t*>(msg.data()),
                           msg.size()) == 1 &&
            siglen == kSignatureSize;
        EVP_MD_CTX_free(ctx);
        if (!ok) {
            EPH_LOG_ERROR(detail::ed25519_signer_logger(), "Ed25519PrivateKey::sign: EVP_DigestSign failed");
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::TlsCipherFailed, "ed25519 sign failed"});
        }
        return sig;
    }

    /// @brief Sign `msg` and return the signature as standard base64 (Binance
    ///        transmission form).
    [[nodiscard]] std::expected<std::string, ::eph::core::ErrorInfo>
    sign_base64(std::string_view msg) const {
        auto s = sign(msg);
        if (!s) return std::unexpected(s.error());
        return detail::base64_encode(s->data(), s->size());
    }

    Ed25519PrivateKey() noexcept : key_(nullptr) {}
    Ed25519PrivateKey(const Ed25519PrivateKey&)            = delete;
    Ed25519PrivateKey& operator=(const Ed25519PrivateKey&) = delete;
    Ed25519PrivateKey(Ed25519PrivateKey&& o) noexcept : key_(o.key_) { o.key_ = nullptr; }
    Ed25519PrivateKey& operator=(Ed25519PrivateKey&& o) noexcept {
        if (this != &o) { free_key_(); key_ = o.key_; o.key_ = nullptr; }
        return *this;
    }
    ~Ed25519PrivateKey() noexcept { free_key_(); }

    [[nodiscard]] explicit operator bool() const noexcept { return key_ != nullptr; }
    [[nodiscard]] void* native_handle() const noexcept { return key_; }

private:
    void free_key_() noexcept {
        if (key_ != nullptr) { EVP_PKEY_free(static_cast<EVP_PKEY*>(key_)); key_ = nullptr; }
    }
    void* key_;  ///< Opaque EVP_PKEY* (Ed25519).
};

} // namespace eph::net
