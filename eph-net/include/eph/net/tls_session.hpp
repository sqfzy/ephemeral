#pragma once

/// @file tls_session.hpp
/// TLS 1.3 session management with custom BIO for generic TCP transport.
///
/// Uses aws-lc (BoringSSL-compatible) API via custom BIO that reads/writes
/// through a user-space TCP session. Supports:
///   - TLS 1.3 handshake over any TcpTransport backend
///   - Session key extraction for hot-path AEAD encryption
///   - Custom BIO backed by TcpTransport send/poll_rx
///
/// Responsibility: handshake + session key extraction only.
/// Data-plane I/O uses TlsRecordCrypto (EVP_AEAD) — no SSL_write/SSL_read.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <openssl/ssl.h>

#include "eph/net/tcp_concept.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// TLS constants
// ─────────────────────────────────────────────────────────────────────────────

namespace tls_const {

inline constexpr uint16_t kRecordHeaderLen   = 5;    // TLS record header
inline constexpr uint16_t kAuthTagLen        = 16;   // AES-GCM auth tag
inline constexpr uint16_t kMaxRecordPayload  = 16384; // TLS max fragment size
inline constexpr uint16_t kTls13NonceLen     = 12;   // AES-GCM nonce length
inline constexpr uint16_t kAes256KeyLen      = 32;   // AES-256 key length

} // namespace tls_const

// ─────────────────────────────────────────────────────────────────────────────
// TLS hot state — cache-line-sized for data plane
// ─────────────────────────────────────────────────────────────────────────────

/// Extracted TLS session material for hot-path AEAD operations.
/// Split into write/read halves on separate cache lines so TX and RX
/// lcores never cause false sharing — each touches only its own 64B line.
struct alignas(64) TlsKeyMaterial {
    uint8_t  key[tls_const::kAes256KeyLen]{};  // 32 bytes
    uint8_t  iv[tls_const::kTls13NonceLen]{};   // 12 bytes
    uint64_t seq = 0;                            // 8 bytes
    // 12 bytes padding to 64
};
static_assert(sizeof(TlsKeyMaterial) == 64, "TlsKeyMaterial must be exactly 1 cache line");

struct TlsHotState {
    TlsKeyMaterial write{};
    TlsKeyMaterial read{};

    // Compatibility accessors for existing code
    uint8_t*       write_key()       noexcept { return write.key; }
    const uint8_t* write_key() const noexcept { return write.key; }
    uint8_t*       write_iv()        noexcept { return write.iv; }
    const uint8_t* write_iv()  const noexcept { return write.iv; }
    uint8_t*       read_key()        noexcept { return read.key; }
    const uint8_t* read_key()  const noexcept { return read.key; }
    uint8_t*       read_iv()         noexcept { return read.iv; }
    const uint8_t* read_iv()   const noexcept { return read.iv; }
};

static_assert(sizeof(TlsHotState) == 128, "TlsHotState must be exactly 2 cache lines");

// ─────────────────────────────────────────────────────────────────────────────
// TLS session config
// ─────────────────────────────────────────────────────────────────────────────

struct TlsConfig {
    std::string hostname{};           // SNI hostname for TLS
    std::string ca_cert_path{};       // CA certificate file (PEM), empty = use default
    bool        verify_peer = true;   // Verify server certificate
    std::chrono::milliseconds handshake_timeout{5000};
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::shared_ptr<spdlog::logger> tls_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.tls");
        if (!lg) lg = spdlog::stdout_color_mt("net.tls");
        // Inherit level from spdlog global default
        return lg;
    }();
    return l;
}

/// Get the last OpenSSL error as a string.
inline std::string ssl_error_string() {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return std::string(buf);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// TLS 1.3 HKDF-Expand-Label for traffic key derivation
// ─────────────────────────────────────────────────────────────────────────────

namespace tls_keygen {

/// HKDF-Expand-Label (RFC 8446 §7.1)
inline bool hkdf_expand_label(const EVP_MD* digest,
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

    return HKDF_expand(out, out_len, digest, secret, secret_len, info, pos) == 1;
}

/// Derive AES key + IV from a TLS 1.3 traffic secret.
inline bool derive_key_iv(const uint8_t* secret, size_t secret_len,
                           uint8_t* key, size_t key_len,
                           uint8_t* iv, size_t iv_len) noexcept {
    const EVP_MD* md = (secret_len == 48) ? EVP_sha384() : EVP_sha256();
    return hkdf_expand_label(md, secret, secret_len, "key", 3, key, key_len) &&
           hkdf_expand_label(md, secret, secret_len, "iv",  2, iv,  iv_len);
}

} // namespace tls_keygen

// ─────────────────────────────────────────────────────────────────────────────
// TLS Session — templated on TcpTransport backend
// ─────────────────────────────────────────────────────────────────────────────

/// TLS session wrapping a TcpTransport with aws-lc/BoringSSL.
///
/// After handshake, session keys can be extracted for hot-path AEAD
/// operations, bypassing the SSL_* API on the data plane.
///
/// The BIO callbacks, BioContext, and BIO method are nested inside
/// TlsSession to avoid namespace-level template complications with
/// C function pointers used by OpenSSL's BIO interface.
template <TcpTransport TcpImpl>
class TlsSession {
    static_assert(TcpTransport<TcpImpl>,
                  "TcpImpl must satisfy TcpTransport concept");

    // ─────────────────────────────────────────────────────────────────────────
    // Nested BIO context and callbacks
    // ─────────────────────────────────────────────────────────────────────────

    /// BIO context: holds pointer to TcpImpl and intermediate buffers.
    struct BioContext {
        TcpImpl*             tcp = nullptr;
        std::vector<uint8_t> read_buf;   // Buffered data from TCP rx
        size_t               read_pos = 0;

        /// Timeout for busy-wait polling (used by bio_read).
        /// Set before handshake to match TlsConfig::handshake_timeout.
        std::chrono::milliseconds poll_timeout{5000};

        /// Poll TCP rx once and append to read buffer.
        /// Returns number of new bytes available, or -1 on TCP error.
        int poll_rx() {
            int total_new = 0;
            auto append_data = [this, &total_new](const uint8_t* data, uint16_t len) {
                read_buf.insert(read_buf.end(), data, data + len);
                total_new += len;
            };
            auto result = tcp->poll_rx(append_data);
            if (!result) {
                SPDLOG_LOGGER_WARN(detail::tls_logger(),
                    "TCP rx error during BIO poll: {}", result.error());
                return -1;
            }
            return total_new;
        }

        /// Busy-wait poll until at least some data is available or timeout.
        /// Returns number of new bytes, 0 on timeout, -1 on TCP error.
        int poll_rx_blocking() {
            auto deadline = std::chrono::steady_clock::now() + poll_timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                int n = poll_rx();
                if (n != 0) return n;  // Got data (>0) or error (-1)
                // Reduce CPU waste while waiting for handshake data
                std::this_thread::yield();
            }
            SPDLOG_LOGGER_WARN(detail::tls_logger(),
                "BIO poll_rx_blocking timed out after {}ms",
                poll_timeout.count());
            return 0; // Timeout
        }
    };

    /// Custom BIO write: sends data through TCP session.
    static int bio_write_cb(BIO* bio, const char* data, int len) {
        auto* ctx = static_cast<BioContext*>(BIO_get_data(bio));
        if (!ctx || !ctx->tcp || len <= 0) return -1;

        BIO_clear_retry_flags(bio);

        // Send data through TCP (may need to split into MSS-sized segments)
        size_t total_sent = 0;
        const auto* ptr = reinterpret_cast<const uint8_t*>(data);
        size_t remaining = static_cast<size_t>(len);

        while (remaining > 0) {
            size_t chunk = std::min(remaining, static_cast<size_t>(ctx->tcp->mss()));
            auto result = ctx->tcp->send(ptr, chunk);
            if (!result) {
                SPDLOG_LOGGER_ERROR(detail::tls_logger(),
                    "BIO write failed: {}", result.error());
                if (total_sent > 0) break;
                return -1;
            }
            total_sent += *result;
            ptr += *result;
            remaining -= *result;
        }

        return static_cast<int>(total_sent);
    }

    /// Custom BIO read: reads data from TCP session rx buffer.
    /// Busy-waits for data to avoid SSL treating WANT_READ as fatal during
    /// multi-record TLS handshake processing.
    static int bio_read_cb(BIO* bio, char* buf, int len) {
        auto* ctx = static_cast<BioContext*>(BIO_get_data(bio));
        if (!ctx || !ctx->tcp || len <= 0) return -1;

        BIO_clear_retry_flags(bio);

        // If buffer is exhausted, busy-wait poll for more data
        size_t available = ctx->read_buf.size() - ctx->read_pos;
        if (available == 0) {
            // Compact buffer
            ctx->read_buf.clear();
            ctx->read_pos = 0;

            // Busy-wait poll TCP rx until data arrives or timeout
            int new_bytes = ctx->poll_rx_blocking();
            if (new_bytes < 0) {
                // TCP error (RST, out-of-order)
                return -1;
            }
            if (new_bytes == 0) {
                // Timeout — let SSL know it should retry
                BIO_set_retry_read(bio);
                return -1;
            }
            available = ctx->read_buf.size();
        }

        // Copy available data to caller
        size_t to_copy = std::min(static_cast<size_t>(len), available);
        std::memcpy(buf, ctx->read_buf.data() + ctx->read_pos, to_copy);
        ctx->read_pos += to_copy;

        // Compact if fully consumed
        if (ctx->read_pos == ctx->read_buf.size()) {
            ctx->read_buf.clear();
            ctx->read_pos = 0;
        }

        return static_cast<int>(to_copy);
    }

    /// Custom BIO ctrl: handles flush and other control operations.
    static long bio_ctrl_cb(BIO* /*bio*/, int cmd, long /*num*/, void* /*ptr*/) {
        switch (cmd) {
            case BIO_CTRL_FLUSH:
                return 1; // Success — TCP sends immediately
            case BIO_CTRL_PUSH:
            case BIO_CTRL_POP:
                return 0;
            default:
                return 0;
        }
    }

    /// Create the custom BIO method (singleton per TcpImpl instantiation).
    static const BIO_METHOD* bio_method() {
        static BIO_METHOD* method = [] {
            auto* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK,
                                   "net_tcp_bio");
            BIO_meth_set_write(m, bio_write_cb);
            BIO_meth_set_read(m, bio_read_cb);
            BIO_meth_set_ctrl(m, bio_ctrl_cb);
            return m;
        }();
        return method;
    }

public:
    /// Create a TLS session over an established TCP connection.
    static std::expected<TlsSession, std::string>
    create(TcpImpl& tcp, const TlsConfig& config) {
        auto log = detail::tls_logger();

        if (!tcp.is_established()) {
            return std::unexpected("TCP session not established");
        }

        SPDLOG_LOGGER_DEBUG(log, "Creating TLS session for host: {}",
                            config.hostname);

        // Create SSL context
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            auto err = detail::ssl_error_string();
            SPDLOG_LOGGER_ERROR(log, "SSL_CTX_new failed: {}", err);
            return std::unexpected(std::format("SSL_CTX_new failed: {}", err));
        }

        // Configure TLS 1.3 minimum
        if (!SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION)) {
            auto err = detail::ssl_error_string();
            SSL_CTX_free(ctx);
            SPDLOG_LOGGER_ERROR(log, "Failed to set TLS 1.3 minimum: {}", err);
            return std::unexpected(std::format(
                "Failed to set TLS 1.3 minimum: {}", err));
        }

        // Load CA certificates
        if (!config.ca_cert_path.empty()) {
            if (!SSL_CTX_load_verify_locations(ctx,
                    config.ca_cert_path.c_str(), nullptr)) {
                auto err = detail::ssl_error_string();
                SSL_CTX_free(ctx);
                SPDLOG_LOGGER_ERROR(log,
                    "Failed to load CA cert {}: {}",
                    config.ca_cert_path, err);
                return std::unexpected(std::format(
                    "Failed to load CA cert: {}", err));
            }
        } else {
            SSL_CTX_set_default_verify_paths(ctx);
        }

        if (config.verify_peer) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        }

        // Create SSL object
        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            auto err = detail::ssl_error_string();
            SSL_CTX_free(ctx);
            SPDLOG_LOGGER_ERROR(log, "SSL_new failed: {}", err);
            return std::unexpected(std::format("SSL_new failed: {}", err));
        }

        // Set SNI hostname
        if (!config.hostname.empty()) {
            SSL_set_tlsext_host_name(ssl, config.hostname.c_str());
        }

        // Mark as client-side (required by aws-lc before SSL_do_handshake)
        SSL_set_connect_state(ssl);

        // Create and attach custom BIO
        BIO* bio = BIO_new(bio_method());
        if (!bio) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            return std::unexpected("BIO_new failed");
        }

        // Set up BIO context
        auto bio_ctx = std::make_unique<BioContext>();
        bio_ctx->tcp = &tcp;
        bio_ctx->poll_timeout = config.handshake_timeout;
        BIO_set_data(bio, bio_ctx.get());
        BIO_set_init(bio, 1);

        // Attach BIO to SSL (SSL takes ownership)
        SSL_set_bio(ssl, bio, bio);

        TlsSession session;
        session.ssl_ = ssl;
        session.ctx_ = ctx;
        session.bio_ctx_ = std::move(bio_ctx);
        session.config_ = config;

        SPDLOG_LOGGER_DEBUG(log, "TLS session created, ready for handshake");
        return session;
    }

    ~TlsSession() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_); // Also frees the BIO
        }
        if (ctx_) {
            SSL_CTX_free(ctx_);
        }
    }

    TlsSession(const TlsSession&)            = delete;
    TlsSession& operator=(const TlsSession&) = delete;

    TlsSession(TlsSession&& other) noexcept
        : ssl_(other.ssl_)
        , ctx_(other.ctx_)
        , bio_ctx_(std::move(other.bio_ctx_))
        , config_(std::move(other.config_))
        , handshake_done_(other.handshake_done_) {
        other.ssl_ = nullptr;
        other.ctx_ = nullptr;
        other.handshake_done_ = false;
    }

    TlsSession& operator=(TlsSession&& other) noexcept {
        if (this != &other) {
            if (ssl_) { SSL_free(ssl_); }
            if (ctx_) { SSL_CTX_free(ctx_); }
            ssl_ = other.ssl_;
            ctx_ = other.ctx_;
            bio_ctx_ = std::move(other.bio_ctx_);
            config_ = std::move(other.config_);
            handshake_done_ = other.handshake_done_;
            other.ssl_ = nullptr;
            other.ctx_ = nullptr;
            other.handshake_done_ = false;
        }
        return *this;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Handshake
    // ─────────────────────────────────────────────────────────────────────────

    /// Perform TLS 1.3 handshake (blocking, polls TCP rx through BIO).
    /// Must be called from the control thread, NOT the hot-path lcore.
    std::expected<void, std::string> handshake() {
        auto log = detail::tls_logger();

        if (handshake_done_) {
            return std::unexpected("Handshake already completed");
        }

        SPDLOG_LOGGER_INFO(log, "Starting TLS 1.3 handshake with {}",
                           config_.hostname);

        auto deadline = std::chrono::steady_clock::now() +
                        config_.handshake_timeout;

        while (true) {
            // Clear stale errors from previous WANT_READ iterations
            ERR_clear_error();
            int ret = SSL_do_handshake(ssl_);
            if (ret == 1) {
                // Handshake completed
                handshake_done_ = true;
                SPDLOG_LOGGER_INFO(log,
                    "TLS handshake complete: version={}, cipher={}",
                    SSL_get_version(ssl_),
                    SSL_CIPHER_get_name(SSL_get_current_cipher(ssl_)));
                return {};
            }

            int err = SSL_get_error(ssl_, ret);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                // bio_read already busy-waits internally, so if we get
                // WANT_READ here it means poll_rx_blocking timed out.
                if (std::chrono::steady_clock::now() >= deadline) {
                    SPDLOG_LOGGER_ERROR(log,
                        "TLS handshake timeout ({}ms)",
                        config_.handshake_timeout.count());
                    return std::unexpected("TLS handshake timeout");
                }
                continue;
            }

            // Fatal error
            auto ssl_err = detail::ssl_error_string();
            SPDLOG_LOGGER_ERROR(log,
                "TLS handshake failed: ssl_error={}, detail={}",
                err, ssl_err);
            return std::unexpected(std::format(
                "TLS handshake failed (err={}): {}", err, ssl_err));
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Handshake-phase I/O (for WebSocket Upgrade after TLS handshake)
    // ─────────────────────────────────────────────────────────────────────────

    /// Write data through TLS during handshake/upgrade phase only.
    /// NOT for hot-path use — hot path uses TlsRecordCrypto directly.
    /// Must be called from the control thread (single-threaded).
    std::expected<int, std::string> handshake_write(const void* data, int len) {
        if (!handshake_done_) {
            return std::unexpected("TLS handshake not completed");
        }
        int ret = SSL_write(ssl_, data, len);
        if (ret <= 0) {
            int err = SSL_get_error(ssl_, ret);
            if (err == SSL_ERROR_WANT_WRITE) return 0;
            return std::unexpected(std::format(
                "SSL_write failed (err={}): {}",
                err, detail::ssl_error_string()));
        }
        return ret;
    }

    /// Read data through TLS during handshake/upgrade phase only.
    /// Returns 0 if no data available (would block).
    std::expected<int, std::string> handshake_read(void* buf, int len) {
        if (!handshake_done_) {
            return std::unexpected("TLS handshake not completed");
        }
        if (bio_ctx_) bio_ctx_->poll_rx();
        int ret = SSL_read(ssl_, buf, len);
        if (ret <= 0) {
            int err = SSL_get_error(ssl_, ret);
            if (err == SSL_ERROR_WANT_READ) return 0;
            return std::unexpected(std::format(
                "SSL_read failed (err={}): {}",
                err, detail::ssl_error_string()));
        }
        return ret;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Session key extraction (for hot-path AEAD via TlsRecordCrypto)
    // ─────────────────────────────────────────────────────────────────────────

    /// Extract TLS 1.3 traffic keys for direct AEAD encryption.
    ///
    /// Uses aws-lc SSL_get_{read,write}_traffic_secret (NOT exporter keys)
    /// to obtain the REAL application traffic secrets, then derives key/IV
    /// via HKDF-Expand-Label. Reads current seq numbers from SSL so
    /// TlsRecordCrypto stays in sync after any SSL_write/SSL_read usage.
    ///
    /// Key length is determined dynamically from the negotiated cipher:
    ///   AES_128_GCM -> 16-byte key    AES_256_GCM -> 32-byte key
    std::expected<TlsHotState, std::string> extract_hot_state() const {
        auto log = detail::tls_logger();

        if (!handshake_done_) {
            return std::unexpected("Cannot extract keys: handshake not done");
        }

        // Determine key length from negotiated cipher
        const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl_);
        if (!cipher) return std::unexpected("No cipher negotiated");

        int cipher_nid = SSL_CIPHER_get_cipher_nid(cipher);
        size_t key_len;
        if (cipher_nid == NID_aes_128_gcm)      key_len = 16;
        else if (cipher_nid == NID_aes_256_gcm)  key_len = 32;
        else return std::unexpected(std::format(
            "Unsupported cipher NID {} for AEAD takeover", cipher_nid));

        // Get write (client->server) traffic secret
        // NOTE: out_len must be initialized to buffer capacity before calling
        uint8_t write_secret[64]; size_t ws_len = sizeof(write_secret);
        if (!SSL_get_write_traffic_secret(ssl_, write_secret, &ws_len)) {
            return std::unexpected("SSL_get_write_traffic_secret failed");
        }

        // Get read (server->client) traffic secret
        uint8_t read_secret[64]; size_t rs_len = sizeof(read_secret);
        if (!SSL_get_read_traffic_secret(ssl_, read_secret, &rs_len)) {
            return std::unexpected("SSL_get_read_traffic_secret failed");
        }

        TlsHotState state{};

        if (!tls_keygen::derive_key_iv(write_secret, ws_len,
                state.write.key, key_len,
                state.write.iv, tls_const::kTls13NonceLen)) {
            return std::unexpected("HKDF derive failed for write key");
        }

        if (!tls_keygen::derive_key_iv(read_secret, rs_len,
                state.read.key, key_len,
                state.read.iv, tls_const::kTls13NonceLen)) {
            return std::unexpected("HKDF derive failed for read key");
        }

        // Current TLS record sequence numbers from SSL
        state.write.seq = SSL_get_write_sequence(ssl_);
        state.read.seq  = SSL_get_read_sequence(ssl_);

        SPDLOG_LOGGER_INFO(log,
            "TLS traffic keys extracted: cipher={}, key_len={}, "
            "write_seq={}, read_seq={}",
            SSL_CIPHER_get_name(cipher), key_len,
            state.write.seq, state.read.seq);

        OPENSSL_cleanse(write_secret, sizeof(write_secret));
        OPENSSL_cleanse(read_secret, sizeof(read_secret));
        return state;
    }

    /// Return the key length for the negotiated cipher (16 or 32).
    [[nodiscard]] size_t cipher_key_len() const noexcept {
        if (!ssl_) return 0;
        const SSL_CIPHER* c = SSL_get_current_cipher(ssl_);
        if (!c) return 0;
        int nid = SSL_CIPHER_get_cipher_nid(c);
        if (nid == NID_aes_128_gcm) return 16;
        if (nid == NID_aes_256_gcm) return 32;
        return 0;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // State queries
    // ─────────────────────────────────────────────────────────────────────────

    [[nodiscard]] bool is_handshake_done() const noexcept {
        return handshake_done_;
    }

    [[nodiscard]] const char* cipher_name() const noexcept {
        if (!ssl_) return "none";
        auto* cipher = SSL_get_current_cipher(ssl_);
        return cipher ? SSL_CIPHER_get_name(cipher) : "none";
    }

    [[nodiscard]] const char* tls_version() const noexcept {
        return ssl_ ? SSL_get_version(ssl_) : "none";
    }

private:
    TlsSession() = default;

    SSL*                          ssl_   = nullptr;
    SSL_CTX*                      ctx_   = nullptr;
    std::unique_ptr<BioContext>   bio_ctx_;
    TlsConfig                     config_;
    bool                          handshake_done_ = false;
};

} // namespace eph::net
