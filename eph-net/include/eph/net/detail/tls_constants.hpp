#pragma once

/// @file tls_constants.hpp
/// TLS record layer constants, key material types, configuration, and
/// record-layer utilities. Shared by TlsSession, TlsEncryptor, TlsDecryptor,
/// and TlsRecordCrypto.
///
/// This header is the foundation of the TLS stack: it defines the types and
/// constants that all TLS components depend on.  tls_session.hpp includes
/// this header (not the reverse) to maintain a clean dependency direction.

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <openssl/mem.h>     // OPENSSL_cleanse

#include "eph/core/detail/json_escape.hpp"
#include "eph/core/detail/logger.hpp"
#include "eph/core/error.hpp"

namespace eph::net {

namespace detail {
using eph::core::detail::json_escape;

/// Lazily-initialized logger for TLS record-layer operations.
/// @return Pointer to the "transport.tls_record" spdlog logger.
inline spdlog::logger* tls_record_logger() {
    static auto* l = ::eph::core::detail::make_logger("transport.tls_record");
    return l;
}
} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// TLS protocol version
// ─────────────────────────────────────────────────────────────────────────────

/// Negotiated TLS protocol version, fixed once handshake completes.
///
/// Used both as a `TlsConfig` policy knob (`min_version` — minimum protocol
/// version SSL_CTX is allowed to negotiate; security default is `Tls13`)
/// and as a `TlsHotState` field (the version actually negotiated, set by
/// `TlsSession::extract_hot_state()` and read by the record-layer encrypt /
/// decrypt branches).
///
/// The enum values are deliberately small (1 byte) so the field fits in the
/// existing `TlsHotState` cache-line layout without growing it.
enum class TlsVersion : uint8_t {
    Tls13 = 0,  ///< TLS 1.3 (RFC 8446) — default; HKDF-Expand-Label key derivation, AAD = record header (5B), nonce = static_iv XOR seq_be
    Tls12 = 1,  ///< TLS 1.2 GCM/CHACHA20 (RFC 5246/5288/7905) — opt-in; key block from SSL_generate_key_block, AAD = seq‖type‖version‖length (13B), nonce = implicit_IV(4B) ‖ explicit_nonce(8B)
};

/// Short label for a TLS version (for logging / `dump()` / `to_json()`).
[[nodiscard]] inline constexpr const char* to_string(TlsVersion v) noexcept {
    return v == TlsVersion::Tls12 ? "tls12" : "tls13";
}

/// Wire-level record format negotiated for the session.
///
/// `TlsVersion` answers "what protocol version did we negotiate?" — but
/// inside a single TLS 1.2 session the wire format depends on the AEAD
/// cipher: AES-GCM has an 8-byte explicit nonce on the wire (RFC 5288)
/// while CHACHA20-POLY1305 has none and reuses the TLS 1.3-style nonce
/// construction (RFC 7905). The record-layer encrypt/decrypt code
/// therefore branches on this field, not on `TlsVersion` alone.
///
/// Set once by `TlsSession::extract_hot_state()`; read by encryptor /
/// decryptor at construction. Internal — never exposed in `TlsConfig`.
enum class TlsRecordFormat : uint8_t {
    /// TLS 1.3 (RFC 8446): 12B static IV XOR seq nonce; 5B AAD = record header;
    /// inner content type appended pre-encryption; wire = header‖ciphertext+tag.
    Tls13 = 0,
    /// TLS 1.2 AES-GCM (RFC 5288): 4B implicit_IV ‖ 8B explicit_nonce nonce;
    /// 13B AAD = seq‖type‖version‖length; explicit_nonce on wire =
    /// header‖explicit_nonce(8B)‖ciphertext+tag.
    Tls12AesGcm = 1,
    /// TLS 1.2 CHACHA20-POLY1305 (RFC 7905): 12B IV XOR seq nonce (same shape
    /// as TLS 1.3); 13B AAD = seq‖type‖version‖length; no explicit nonce on
    /// wire = header‖ciphertext+tag.
    Tls12Chacha20 = 2,
};

[[nodiscard]] inline constexpr const char* to_string(TlsRecordFormat f) noexcept {
    switch (f) {
        case TlsRecordFormat::Tls13:         return "tls13";
        case TlsRecordFormat::Tls12AesGcm:   return "tls12-aes-gcm";
        case TlsRecordFormat::Tls12Chacha20: return "tls12-chacha20-poly1305";
    }
    return "unknown";
}

// ─────────────────────────────────────────────────────────────────────────────
// TLS constants
// ─────────────────────────────────────────────────────────────────────────────

/// TLS cryptographic constants shared across the transport layer.
///
/// These values are defined by the TLS 1.3 specification (RFC 8446),
/// the TLS 1.2 GCM specification (RFC 5288), and the AES-GCM AEAD
/// algorithm (NIST SP 800-38D).
namespace tls_const {

inline constexpr uint16_t kMaxRecordPayload  = 16384; ///< Maximum TLS plaintext fragment size (2^14, RFC 8446 section 5.1)
inline constexpr uint16_t kTls13NonceLen     = 12;    ///< AES-GCM nonce length in bytes (96 bits)
inline constexpr uint16_t kAes256KeyLen      = 32;    ///< AES-256 key length in bytes (256 bits)
inline constexpr uint16_t kTls12ImplicitIvLen = 4;    ///< TLS 1.2 GCM implicit IV (RFC 5288): static 4-byte prefix derived from the key block

} // namespace tls_const

// ─────────────────────────────────────────────────────────────────────────────
// TLS hot state — cache-line-sized for data plane
// ─────────────────────────────────────────────────────────────────────────────

/// AES key and initialization vector pair, aligned to a single cache line.
///
/// Contains the AES-256 key (32 bytes) and AES-GCM IV (12 bytes) for one
/// direction of the TLS session. Read-only after key extraction; padding
/// fills the remainder of the 64-byte cache line to prevent false sharing.
///
/// `iv` is interpreted differently per `TlsHotState::version`:
///   - TLS 1.3: full 12-byte static IV; per-record nonce = `iv XOR seq_be`
///   - TLS 1.2 GCM: only `iv[0..3]` (`tls_const::kTls12ImplicitIvLen`) is
///     populated — the implicit IV from the key block. Per-record nonce =
///     `implicit_iv ‖ explicit_nonce(8B = seq_be)`. The remaining 8 bytes
///     of `iv[]` are left zero so a misread under the wrong version surfaces
///     as an immediate AEAD failure rather than a silent garbage nonce.
///
/// @warning Contains sensitive cryptographic material. The owning TlsHotState
///          destructor scrubs this struct via OPENSSL_cleanse.
struct alignas(64) TlsKeyIv {
    uint8_t key[tls_const::kAes256KeyLen]{};  ///< AES-256 key (32 bytes)
    uint8_t iv[tls_const::kTls13NonceLen]{};  ///< AES-GCM IV: 12B static IV (TLS 1.3) or 4B implicit_IV in iv[0..3] (TLS 1.2)
    // 20 bytes padding to 64
};
static_assert(sizeof(TlsKeyIv) == 64, "TlsKeyIv must be exactly 1 cache line");

/// Per-direction TLS state spanning two cache lines for false-sharing avoidance.
///
/// Cache line 1 (TlsKeyIv): key and IV, read-only after handshake.
/// Cache line 2 (seq): sequence number, incremented on every encrypt/decrypt.
/// Separating these prevents the hot write to seq from invalidating the
/// read-only key/IV in the L1 cache of another core.
struct TlsKeyMaterial {
    TlsKeyIv ki{};                 ///< Key + IV (cache line 1, read-only after handshake)
    alignas(64) uint64_t seq = 0;  ///< Record sequence number (cache line 2, hot write path)
};
static_assert(sizeof(TlsKeyMaterial) == 128, "TlsKeyMaterial must be exactly 2 cache lines");

/// Complete TLS session key state for both directions (TX and RX).
///
/// Total size: 5 cache lines (320 bytes). After handshake and key extraction,
/// this struct is consumed by TlsEncryptor and TlsDecryptor (or the combined
/// TlsRecordCrypto) to initialize their AEAD contexts.
///
/// Layout:
///   - Lines 1-4 (offsets 0..255): write/read key material (unchanged from TLS 1.3)
///   - Line 5  (offset 256+):      `version` plus padding. `version` is set
///     once by `TlsSession::extract_hot_state()` and read on every record by
///     the encrypt / decrypt branches; placed on its own cache line via
///     alignas so it never shares with the hot `seq` write.
///
/// @warning Contains sensitive key material. Destructor scrubs all key/IV data
///          via OPENSSL_cleanse to prevent residual secrets in freed memory.
struct TlsHotState {
    TlsKeyMaterial write{};  ///< Write-direction key material (2 cache lines, used by TlsEncryptor)
    TlsKeyMaterial read{};   ///< Read-direction key material (2 cache lines, used by TlsDecryptor)
    /// Negotiated TLS version. Read-only after `extract_hot_state()`. Encryptor /
    /// decryptor branch on this once at construction; the field itself is not
    /// re-read on the per-record hot path.
    alignas(64) TlsVersion version = TlsVersion::Tls13;
    /// Wire-level record format. Set together with `version` in
    /// `extract_hot_state()`. The two are correlated but not redundant:
    /// `version` is user-facing (TLS 1.2 vs 1.3), `record_format` selects
    /// the encrypt/decrypt path inside the record layer (1.3 vs 1.2 GCM
    /// vs 1.2 CHACHA20-POLY1305). Co-located on the same 5th cache line
    /// as `version`; sizeof(TlsHotState) stays 320.
    TlsRecordFormat record_format = TlsRecordFormat::Tls13;

    ~TlsHotState() {
        // Scrub all key material (keys + IVs) to prevent residual
        // secret data in freed memory.  OPENSSL_cleanse is a compiler-
        // barrier-protected memset that won't be optimized away.
        // `version` is not secret — no need to cleanse.
        OPENSSL_cleanse(&write.ki, sizeof(write.ki));
        OPENSSL_cleanse(&read.ki,  sizeof(read.ki));
    }

    // Compatibility accessors
    uint8_t*       write_key()       noexcept { return write.ki.key; }
    const uint8_t* write_key() const noexcept { return write.ki.key; }
    uint8_t*       write_iv()        noexcept { return write.ki.iv; }
    const uint8_t* write_iv()  const noexcept { return write.ki.iv; }
    uint8_t*       read_key()        noexcept { return read.ki.key; }
    const uint8_t* read_key()  const noexcept { return read.ki.key; }
    uint8_t*       read_iv()         noexcept { return read.ki.iv; }
    const uint8_t* read_iv()   const noexcept { return read.ki.iv; }
};

static_assert(sizeof(TlsHotState) == 320, "TlsHotState must be exactly 5 cache lines (4 for key material + 1 for version)");

// ─────────────────────────────────────────────────────────────────────────────
// TLS session config
// ─────────────────────────────────────────────────────────────────────────────

/// Configuration for a TLS session.
///
/// Controls SNI hostname, certificate verification, CA trust store,
/// mutual TLS client credentials, handshake timeout, and minimum
/// negotiable protocol version.
///
/// Default `min_version = Tls13` — i.e. the SSL_CTX rejects any
/// negotiation below TLS 1.3, which matches the historical (pre-1.2)
/// behaviour of this stack and is the safe default. Setting
/// `min_version = Tls12` opens the door to TLS 1.2 GCM/CHACHA20 (e.g.
/// to traverse middleboxes that drop TLS 1.3) — see the `warnings()`
/// output and `docs/tls-crypto-recommendations.md` for the downgrade
/// risk discussion.
struct TlsConfig {
    std::string hostname{};           ///< SNI hostname (sent during ClientHello for virtual hosting)
    std::string ca_cert_path{};       ///< CA certificate file path (PEM format), empty = system default
    bool        verify_peer = true;   ///< Verify server certificate chain and hostname
    std::chrono::milliseconds handshake_timeout{5000}; ///< Maximum time for the TLS handshake

    /// Minimum negotiable TLS version. Default `Tls13` keeps the historical
    /// "TLS 1.3 only" behaviour. Set `Tls12` to opt into TLS 1.2 GCM/CHACHA20
    /// negotiation; the SSL_CTX is then configured with a strict AEAD-only
    /// cipher whitelist (`ECDHE-{RSA,ECDSA}-AES{128,256}-GCM` +
    /// `ECDHE-{RSA,ECDSA}-CHACHA20-POLY1305`). CBC suites are never accepted
    /// at any version of this stack. Opting into 1.2 also accepts the
    /// possibility of an attacker forcing a downgrade — only set Tls12 when
    /// you actually need it (e.g. proxy interop).
    TlsVersion min_version = TlsVersion::Tls13;

    /// Client certificate for mutual TLS (mTLS).
    /// Both must be set together; leave both empty to disable client authentication.
    std::string client_cert_path{};   ///< Client certificate file path (PEM format)
    std::string client_key_path{};    ///< Client private key file path (PEM format)

    /// SPKI SHA-256 hashes for certificate pinning (base64-encoded).
    /// Empty = no pinning (default). Non-empty = verify peer cert SPKI hash
    /// against this list after TLS handshake.
    std::vector<std::string> pinned_spki_sha256{};

    /// Optional TLS 1.3 session ticket from a prior connection.
    ///
    /// Format: opaque DER-encoded `SSL_SESSION` produced by
    /// `i2d_SSL_SESSION` (aws-lc / OpenSSL). Empty = no resumption attempt
    /// (default), perform a full handshake.
    ///
    /// When non-empty, the ticket is parsed via `d2i_SSL_SESSION` and
    /// applied with `SSL_set_session` BEFORE `SSL_do_handshake`, which
    /// turns the next ClientHello into a TLS 1.3 PSK / NewSessionTicket
    /// resumption attempt — saving the +1 RTT certificate exchange. If
    /// the ticket is corrupt, expired, or rejected by the server the
    /// stack falls back to a full handshake (the connection still
    /// succeeds; it's not a fatal error).
    ///
    /// Conservative defaults: this is opt-in. HFT venues like Binance
    /// (24h forced reconnect), OKX, Bybit issue tickets aggressively;
    /// pass the bytes from a prior session's `tls_resumption_ticket()`
    /// getter on the next connect.
    ///
    /// 0-RTT (early data) is **out of scope** — only the 1-RTT
    /// abbreviated handshake is supported here.
    std::vector<uint8_t> tls_resumption_ticket{};

    /// Callback invoked when peer cert SPKI hash doesn't match any pin.
    /// Receives the actual base64-encoded SPKI SHA-256 hash.
    /// Return true to continue connection despite mismatch, false to abort.
    ///
    /// nullptr = HARD PIN: reject the connection on mismatch (default).
    /// This is the safe default — an empty callback combined with a non-empty
    /// `pinned_spki_sha256` list makes a mismatch fatal so a MITM with a
    /// valid CA-signed cert can't bypass the pin. To opt into soft-pin
    /// (warn + continue) install a callback that returns `true` regardless
    /// of `actual_hash`. See `TlsSession::verify_spki_pin()` for the
    /// enforcement path.
    std::function<bool(std::string_view actual_hash)> on_pin_mismatch{};

    /// Validate configuration, returning an error on failure.
    /// Consistent with ProxyConfig::validate() — both return
    /// std::expected<void, ErrorInfo> for uniform error handling.
    [[nodiscard]] std::expected<void, ::eph::core::ErrorInfo>
    validate() const noexcept {
        if (handshake_timeout.count() <= 0)
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "TlsConfig: handshake_timeout must be positive"});
        if (client_cert_path.empty() != client_key_path.empty())
            return std::unexpected(::eph::core::ErrorInfo{
                ::eph::core::Error::InvalidConfig,
                "TlsConfig: client_cert_path and client_key_path must both be set or both empty"});
        return {};
    }

    /// Equality comparison (excludes on_pin_mismatch callback which is not comparable).
    [[nodiscard]] friend bool operator==(const TlsConfig& a, const TlsConfig& b) {
        return a.hostname == b.hostname
            && a.ca_cert_path == b.ca_cert_path
            && a.verify_peer == b.verify_peer
            && a.handshake_timeout == b.handshake_timeout
            && a.client_cert_path == b.client_cert_path
            && a.client_key_path == b.client_key_path
            && a.pinned_spki_sha256 == b.pinned_spki_sha256
            && a.tls_resumption_ticket == b.tls_resumption_ticket
            && a.min_version == b.min_version;
    }

    /// JSON-formatted config for logging/monitoring.
    /// @note client_key_path is redacted to prevent private key path leakage in logs.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{"
            "\"hostname\":\"{}\",\"ca_cert_path\":\"{}\","
            "\"verify_peer\":{},\"handshake_timeout_ms\":{},"
            "\"client_cert_path\":\"{}\",\"has_client_key\":{},"
            "\"min_version\":\"{}\"}}",
            detail::json_escape(hostname),
            detail::json_escape(ca_cert_path),
            verify_peer ? "true" : "false",
            handshake_timeout.count(),
            detail::json_escape(client_cert_path),
            client_key_path.empty() ? "false" : "true",
            to_string(min_version));
    }

    /// Human-readable dump for debug logging.
    /// @note client_key_path is redacted to prevent private key path leakage in logs.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "TlsConfig(hostname='{}', verify_peer={}, timeout={}ms, "
            "ca='{}', client_cert='{}', client_key='{}', min_version={})",
            hostname, verify_peer, handshake_timeout.count(),
            ca_cert_path,
            client_cert_path.empty() ? "(none)" : client_cert_path,
            client_key_path.empty() ? "(none)" : "(redacted)",
            to_string(min_version));
    }

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate() which blocks construction, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        if (!verify_peer)
            w.emplace_back("verify_peer=false -- server certificate will NOT be "
                           "validated (vulnerable to MITM attacks)");
        if (hostname.empty())
            w.emplace_back("hostname is empty -- SNI will not be sent, which may "
                           "cause TLS handshake failures with virtual-hosted servers");
        if (verify_peer && hostname.empty())
            w.emplace_back("verify_peer=true with empty hostname -- chain is "
                           "verified but cert identity binding is OFF "
                           "(any trusted cert for any hostname is accepted; "
                           "only safe for fixed pinned peers)");
        if (handshake_timeout.count() < 1000)
            w.emplace_back(std::format(
                "handshake_timeout={}ms is very short -- TLS 1.3 handshake "
                "may fail on high-latency links", handshake_timeout.count()));
        if (handshake_timeout.count() > 30000)
            w.emplace_back(std::format(
                "handshake_timeout={}ms is very long -- consider a shorter "
                "timeout to detect unresponsive peers faster",
                handshake_timeout.count()));
        if (!ca_cert_path.empty() && !verify_peer)
            w.emplace_back("ca_cert_path is set but verify_peer=false -- "
                           "CA certificate will be loaded but not used for "
                           "verification");
        if (min_version == TlsVersion::Tls12)
            w.emplace_back("min_version=Tls12 -- TLS 1.2 negotiation is "
                           "permitted; an attacker who can interpose may force "
                           "the connection to 1.2 even if the server supports "
                           "1.3. Only opt in when interop with TLS 1.2-only "
                           "middleboxes is actually required");
        return w;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// SPKI SHA-256 pin verification utilities
// ─────────────────────────────────────────────────────────────────────────────

namespace spki_pin {

/// Compute the base64-encoded SHA-256 hash of a DER-encoded SPKI blob.
///
/// @param spki_der  DER-encoded Subject Public Key Info bytes
/// @param spki_len  Length of the DER data
/// @return Base64-encoded SHA-256 hash, or empty string on failure
[[nodiscard]] inline std::string compute_spki_sha256_b64(
        const uint8_t* spki_der, size_t spki_len) {
    // SHA-256 digest
    uint8_t digest[32];
    unsigned int digest_len = 0;
    if (!EVP_Digest(spki_der, spki_len, digest, &digest_len,
                    EVP_sha256(), nullptr) || digest_len != 32) {
        return {};
    }

    // Base64 encode (EVP_EncodeBlock outputs null-terminated string,
    // output size = 4 * ceil(n/3) + 1 for null terminator)
    // For 32 bytes: 4 * ceil(32/3) = 4 * 11 = 44 chars + null
    char b64[48]{};
    int b64_len = EVP_EncodeBlock(
        reinterpret_cast<uint8_t*>(b64), digest, 32);
    if (b64_len <= 0) return {};

    return std::string(b64, static_cast<size_t>(b64_len));
}

/// Check whether a base64-encoded SPKI SHA-256 hash matches any pin
/// in the given list.
///
/// @param actual_hash  The computed base64-encoded SHA-256 hash
/// @param pins         List of expected base64-encoded hashes
/// @return true if actual_hash matches at least one pin
[[nodiscard]] inline bool matches_any_pin(
        std::string_view actual_hash,
        const std::vector<std::string>& pins) {
    for (const auto& pin : pins) {
        if (actual_hash == pin) return true;
    }
    return false;
}

} // namespace spki_pin

// ─────────────────────────────────────────────────────────────────────────────
// TLS 1.3 HKDF-Expand-Label for traffic key derivation
// ─────────────────────────────────────────────────────────────────────────────

/// TLS 1.3 key derivation functions (HKDF-based).
///
/// Used to derive per-direction AES keys and IVs from the TLS traffic
/// secrets exported after the handshake completes.
namespace tls_keygen {

/// HKDF-Expand-Label as specified in RFC 8446 section 7.1.
///
/// Derives output keying material from a secret using the TLS 1.3
/// label format: "tls13 " + label.
///
/// @param digest     Hash algorithm (SHA-256 or SHA-384)
/// @param secret     Input secret (traffic secret from handshake)
/// @param secret_len Length of input secret
/// @param label      Label string (e.g., "key" or "iv")
/// @param label_len  Length of label string
/// @param[out] out   Output buffer for derived material
/// @param out_len    Desired output length
/// @return true on success, false on failure
[[nodiscard]] inline bool hkdf_expand_label(const EVP_MD* digest,
                                const uint8_t* secret, size_t secret_len,
                                const char* label, size_t label_len,
                                uint8_t* out, size_t out_len) noexcept {
    static constexpr const char* kPrefix = "tls13 ";
    static constexpr size_t kPrefixLen = 6;

    uint8_t info[256];
    size_t pos = 0;
    info[pos++] = static_cast<uint8_t>((out_len >> 8) & 0xFF);
    info[pos++] = static_cast<uint8_t>(out_len & 0xFF);
    info[pos++] = static_cast<uint8_t>(kPrefixLen + label_len);
    std::memcpy(info + pos, kPrefix, kPrefixLen); pos += kPrefixLen;
    std::memcpy(info + pos, label, label_len);    pos += label_len;
    info[pos++] = 0; // empty context

    // Verify we haven't overflowed the info buffer.
    // 3 (header) + kPrefixLen(6) + label_len + 1 (context) must fit in 256 bytes.
    // Return false instead of assert so callers can handle the error gracefully
    // without crashing the process in production builds.
    if (pos > sizeof(info)) [[unlikely]] return false;

    return HKDF_expand(out, out_len, digest, secret, secret_len, info, pos) == 1;
}

/// Derive AES key and IV from a TLS 1.3 traffic secret.
///
/// Uses HKDF-Expand-Label with "key" and "iv" labels to extract the
/// per-direction encryption key and initialization vector.
///
/// @param secret      Traffic secret (client_application_traffic_secret or server_...)
/// @param secret_len  Length of traffic secret (32 for SHA-256, 48 for SHA-384)
/// @param[out] key    Output AES key buffer
/// @param key_len     Desired key length (16 for AES-128, 32 for AES-256)
/// @param[out] iv     Output IV buffer
/// @param iv_len      Desired IV length (typically 12 for AES-GCM)
/// @return true if both key and IV derivations succeeded
[[nodiscard]] inline bool derive_key_iv(const uint8_t* secret, size_t secret_len,
                           uint8_t* key, size_t key_len,
                           uint8_t* iv, size_t iv_len) noexcept {
    // Only TLS 1.3's two defined hash sizes are valid here. Reject anything
    // else explicitly rather than silently defaulting to SHA-256, which would
    // mask a programming error or a downgrade attempt.
    if (secret_len != 32 && secret_len != 48) {
        return false;
    }
    const EVP_MD* md = (secret_len == 48) ? EVP_sha384() : EVP_sha256();
    return hkdf_expand_label(md, secret, secret_len, "key", 3, key, key_len) &&
           hkdf_expand_label(md, secret, secret_len, "iv",  2, iv,  iv_len);
}

} // namespace tls_keygen

// ─────────────────────────────────────────────────────────────────────────────
// TLS record layer utilities
// ─────────────────────────────────────────────────────────────────────────────

/// TLS record-layer constants and utility functions.
///
/// Implements the wire format for TLS 1.3 application data records:
/// header construction, header parsing, and nonce derivation for AES-GCM.
namespace tls_record {

inline constexpr uint8_t  kContentTypeAppData = 0x17; ///< TLS 1.3 application data content type
inline constexpr uint16_t kLegacyVersion      = 0x0303; ///< Legacy TLS version bytes (0x0303 = TLS 1.2, per TLS 1.3 spec)
inline constexpr uint16_t kRecordHeaderLen     = 5;   ///< TLS record header: content_type(1) + version(2) + length(2)
inline constexpr uint16_t kAuthTagLen          = 16;  ///< AES-GCM authentication tag length in bytes

/// Maximum TLS 1.3 record sequence number before forced reconnection.
///
/// Set to 2^32 (~4.3B records) per NIST SP 800-38D §8.3 for AES-GCM
/// with deterministic nonces. This library uses the TLS 1.3 nonce
/// construction (seq XOR static_iv) which is deterministic — the 2^32
/// limit ensures nonce uniqueness within a single session.
///
/// At HFT rates (~100K messages/sec), this allows ~12 hours per session,
/// well beyond typical exchange maintenance windows (24h forced disconnect).
inline constexpr uint64_t kMaxSequenceNumber = (1ULL << 32);
/// Threshold at which a log warning is emitted (90% of max).
inline constexpr uint64_t kSequenceWarnThreshold = kMaxSequenceNumber * 9 / 10;
/// Threshold at which a preemptive reconnect is triggered (95% of max).
inline constexpr uint64_t kSequenceReconnectThreshold = kMaxSequenceNumber * 95 / 100;

/// Build a per-record nonce by XOR-ing the IV with the big-endian sequence number.
///
/// Implements the TLS 1.3 nonce construction from RFC 8446 section 5.3:
/// the 64-bit sequence number is zero-padded to the nonce length and
/// XOR-ed with the static IV derived during key schedule.
///
/// @param[out] out  Output nonce buffer (must be tls_const::kTls13NonceLen bytes)
/// @param      iv   Static per-connection IV (tls_const::kTls13NonceLen bytes)
/// @param      seq  Record sequence number (monotonically increasing)
inline void build_nonce(uint8_t out[tls_const::kTls13NonceLen],
                        const uint8_t iv[tls_const::kTls13NonceLen],
                        uint64_t seq) noexcept {
    std::memcpy(out, iv, 4);
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

/// Write a 5-byte TLS record header to the output buffer.
///
/// @param[out] dst          Output buffer (must have at least kRecordHeaderLen bytes)
/// @param      content_type TLS content type byte (typically kContentTypeAppData)
/// @param      payload_len  Length of the encrypted payload (ciphertext + tag)
inline void write_record_header(uint8_t* dst, uint8_t content_type,
                                 uint16_t payload_len) noexcept {
    dst[0] = content_type;
    dst[1] = static_cast<uint8_t>(kLegacyVersion >> 8);
    dst[2] = static_cast<uint8_t>(kLegacyVersion & 0xFF);
    dst[3] = static_cast<uint8_t>(payload_len >> 8);
    dst[4] = static_cast<uint8_t>(payload_len & 0xFF);
}

/// Parse a TLS record header and validate content type and payload bounds.
///
/// @param      src          Input buffer (must have at least kRecordHeaderLen bytes)
/// @param[out] content_type Parsed content type byte
/// @param[out] payload_len  Parsed payload length
/// @return true if the record header describes a valid application data record
///         within the maximum TLS record size; false otherwise.
///
/// **RFC 8446 §5.2 cap caveat**: `TLSCiphertext.length` may be up to
/// `2^14 + 256 = 16640` bytes (plaintext + content-type + padding +
/// auth tag). The cap below — `kMaxRecordPayload + kAuthTagLen + 1 =
/// 16401` — rejects any record with padding present. HFT exchanges
/// (the only servers eph-net is built to talk to) do not emit padded
/// TLS records, so the tighter bound is a defensive shrink on
/// untrusted data rather than a strict-RFC compliance gate. If a
/// future server starts using padding, raise this to
/// `kTlsCiphertextHardLimit = 16640` and add per-record padding
/// length validation (RFC 8446 §5.4).
[[nodiscard]] inline bool parse_record_header(const uint8_t* src,
                                 uint8_t& content_type,
                                 uint16_t& payload_len) noexcept {
    content_type = src[0];
    payload_len = static_cast<uint16_t>((src[3] << 8) | src[4]);

    // RFC 8446 §5.1: legacy_record_version must be 0x0303 for TLS 1.3
    // records.  Non-conforming values indicate protocol violations or
    // middlebox interference — warn instead of silently accepting.
    if (src[1] != 0x03 || src[2] != 0x03) {
        SPDLOG_LOGGER_WARN(detail::tls_record_logger(),
            "TLS record header: unexpected version bytes 0x{:02X}{:02X} "
            "(expected 0x0303 per RFC 8446)",
            src[1], src[2]);
    }

    return content_type == kContentTypeAppData &&
           payload_len <= tls_const::kMaxRecordPayload + kAuthTagLen + 1;
}

} // namespace tls_record

// ─────────────────────────────────────────────────────────────────────────────
// TLS 1.2 record layer (RFC 5288 GCM, RFC 7905 CHACHA20-POLY1305)
// ─────────────────────────────────────────────────────────────────────────────

/// TLS 1.2 record-layer constants and helpers.
///
/// Differences from `tls_record` (which targets TLS 1.3):
///   - AAD includes `seq_num`, `content_type`, `version`, `length` —
///     a 13-byte structure rather than the 5-byte TLS 1.3 header.
///   - For AES-GCM the per-record nonce is `implicit_IV(4B) ‖ explicit_nonce(8B)`,
///     where `explicit_nonce` is the 8-byte sequence number (RFC 5288). The
///     explicit nonce is also written on the wire ahead of the ciphertext.
///   - For CHACHA20-POLY1305 the per-record nonce is
///     `implicit_IV(12B) XOR seq_padded(12B)` (RFC 7905), and there is NO
///     explicit nonce on the wire — wire layout matches TLS 1.3's shape.
///   - The plaintext is NOT extended with an inner content-type byte
///     before encryption; the record header carries the real content type.
///   - The AEAD `length` field embedded in the AAD is the **plaintext
///     length** (NOT ciphertext+tag length). The record-header `length`
///     field on the wire IS ciphertext+tag (and, for AES-GCM, also the
///     8B explicit nonce).
namespace tls_record_v12 {

inline constexpr uint16_t kAadLen          = 13;  ///< 8(seq) + 1(type) + 2(version) + 2(length)
inline constexpr uint16_t kExplicitNonceLen = 8; ///< AES-GCM explicit nonce on wire (RFC 5288)
inline constexpr uint16_t kImplicitIvLen    = tls_const::kTls12ImplicitIvLen; ///< AES-GCM implicit IV (4B)

/// Build the 13-byte TLS 1.2 AEAD additional_data (RFC 5246 §6.2.3.3).
///
/// @param[out] aad           Output buffer (must have at least 13 bytes)
/// @param      seq           Record sequence number (host-order; written BE)
/// @param      content_type  TLS content type byte (0x17 for application_data)
/// @param      plaintext_len Plaintext length in bytes (NOT ciphertext+tag)
inline void build_aad(uint8_t aad[kAadLen],
                      uint64_t seq,
                      uint8_t content_type,
                      uint16_t plaintext_len) noexcept {
    // seq_num (8 bytes, big-endian)
    uint64_t seq_be;
    if constexpr (std::endian::native == std::endian::little) {
        seq_be = std::byteswap(seq);
    } else {
        seq_be = seq;
    }
    std::memcpy(aad, &seq_be, 8);
    aad[8]  = content_type;
    aad[9]  = static_cast<uint8_t>(tls_record::kLegacyVersion >> 8);    // 0x03
    aad[10] = static_cast<uint8_t>(tls_record::kLegacyVersion & 0xFF);  // 0x03
    aad[11] = static_cast<uint8_t>(plaintext_len >> 8);
    aad[12] = static_cast<uint8_t>(plaintext_len & 0xFF);
}

/// Build the 12-byte AES-GCM nonce for a TLS 1.2 GCM record (RFC 5288 §3).
///
/// nonce = implicit_IV (4B from key block) ‖ explicit_nonce (8B = seq big-endian)
///
/// @param[out] nonce        Output buffer (must have at least 12 bytes)
/// @param      implicit_iv  Per-direction 4-byte implicit IV
/// @param      seq          Record sequence number (host-order; written BE
///                          as the explicit nonce)
inline void build_nonce_aes_gcm(
        uint8_t nonce[tls_const::kTls13NonceLen],
        const uint8_t implicit_iv[kImplicitIvLen],
        uint64_t seq) noexcept {
    std::memcpy(nonce, implicit_iv, kImplicitIvLen);
    uint64_t seq_be;
    if constexpr (std::endian::native == std::endian::little) {
        seq_be = std::byteswap(seq);
    } else {
        seq_be = seq;
    }
    std::memcpy(nonce + kImplicitIvLen, &seq_be, 8);
}

/// Build the 12-byte ChaCha20-Poly1305 nonce for a TLS 1.2 CHACHA20 record
/// (RFC 7905 §2).
///
/// nonce = static_iv XOR (zero_pad(4B) ‖ seq_be(8B))
///
/// Identical shape to the TLS 1.3 nonce construction (`tls_record::build_nonce`)
/// — RFC 7905 deliberately reused that pattern for CHACHA20 in TLS 1.2.
///
/// @param[out] nonce      Output buffer (must have at least 12 bytes)
/// @param      static_iv  Per-direction 12-byte IV (full key-block IV)
/// @param      seq        Record sequence number (host-order; XOR'd as BE)
inline void build_nonce_chacha20(
        uint8_t nonce[tls_const::kTls13NonceLen],
        const uint8_t static_iv[tls_const::kTls13NonceLen],
        uint64_t seq) noexcept {
    // Reuse the TLS 1.3 helper — the algorithm is bit-for-bit identical.
    tls_record::build_nonce(nonce, static_iv, seq);
}

/// Total wire-record size for a TLS 1.2 AES-GCM record encrypting `plaintext_len`
/// plaintext bytes: 5B header + 8B explicit nonce + plaintext + 16B auth tag.
inline constexpr uint16_t encrypted_size_aes_gcm(uint16_t plaintext_len) noexcept {
    return tls_record::kRecordHeaderLen + kExplicitNonceLen +
           plaintext_len + tls_record::kAuthTagLen;
}

/// Total wire-record size for a TLS 1.2 CHACHA20-POLY1305 record:
/// 5B header + plaintext + 16B auth tag (no explicit nonce on wire).
inline constexpr uint16_t encrypted_size_chacha20(uint16_t plaintext_len) noexcept {
    return tls_record::kRecordHeaderLen + plaintext_len + tls_record::kAuthTagLen;
}

} // namespace tls_record_v12

/// Compute the wire-record size for `plaintext_len` plaintext bytes under
/// the given record format. Centralizes the per-format byte-budget so
/// buffer sizing in the kernel / DPDK fan-out paths can pick the right
/// allocation without duplicating the layout knowledge.
[[nodiscard]] inline constexpr uint16_t encrypted_size_for(
        TlsRecordFormat fmt, uint16_t plaintext_len) noexcept {
    switch (fmt) {
        case TlsRecordFormat::Tls13:
            // RFC 8446: 5B header + plaintext + 1B inner_type + 16B tag
            return tls_record::kRecordHeaderLen + plaintext_len + 1 +
                   tls_record::kAuthTagLen;
        case TlsRecordFormat::Tls12AesGcm:
            return tls_record_v12::encrypted_size_aes_gcm(plaintext_len);
        case TlsRecordFormat::Tls12Chacha20:
            return tls_record_v12::encrypted_size_chacha20(plaintext_len);
    }
    return 0;  // unreachable
}

/// Plaintext byte offset inside a wire record relative to its start.
/// TLS 1.3 and TLS 1.2 CHACHA20 keep plaintext immediately after the 5B
/// header; TLS 1.2 AES-GCM has an 8B explicit nonce in between.
///
/// Used by in-place decryptors so callers can locate plaintext after a
/// successful AEAD open without duplicating the format knowledge.
[[nodiscard]] inline constexpr uint16_t plaintext_offset_for(TlsRecordFormat fmt) noexcept {
    return (fmt == TlsRecordFormat::Tls12AesGcm)
         ? static_cast<uint16_t>(tls_record::kRecordHeaderLen +
                                  tls_record_v12::kExplicitNonceLen)
         : tls_record::kRecordHeaderLen;
}

} // namespace eph::net

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for TlsConfig
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::net::TlsConfig> : std::formatter<std::string> {
    auto format(const eph::net::TlsConfig& c, auto& ctx) const {
        return std::formatter<std::string>::format(c.dump(), ctx);
    }
};
