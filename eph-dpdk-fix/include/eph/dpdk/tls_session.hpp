#pragma once

/// @file tls_session.hpp
/// TLS 1.3 session management with custom BIO for DPDK transport.
///
/// Uses aws-lc (BoringSSL-compatible) API via custom BIO that reads/writes
/// through the user-space TCP session. Supports:
///   - TLS 1.3 handshake over DPDK TCP
///   - Session key extraction for hot-path AEAD encryption
///   - Custom BIO backed by TcpSession send/recv
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

#include "eph/dpdk/tcp.hpp"

namespace eph::dpdk {

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
    uint16_t key_len = tls_const::kAes256KeyLen; // Actual key length (16 or 32)

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
        auto lg = spdlog::stdout_color_mt("dpdk.tls");
        lg->set_level(spdlog::level::trace);
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
// Custom BIO for DPDK TCP — bridges SSL I/O to TcpSession
// ─────────────────────────────────────────────────────────────────────────────

namespace bio_dpdk {

/// BIO context: holds pointer to TcpSession and intermediate buffers.
struct BioContext {
    TcpSession*          tcp = nullptr;
    rte_mempool*         pool = nullptr;
    std::vector<uint8_t> read_buf;   // Buffered data from TCP rx
    size_t               read_pos = 0;

    /// Timeout for busy-wait polling (used by bio_read).
    /// Set before handshake to match TlsConfig::handshake_timeout.
    std::chrono::milliseconds poll_timeout{5000};

    /// Poll DPDK rx once and append to read buffer.
    /// Returns number of new bytes available, or -1 on TCP error.
    int poll_rx() {
        rte_mbuf* pkts[32];
        uint16_t nb_rx = rte_eth_rx_burst(
            tcp->config().port_id, tcp->config().rx_queue_id, pkts, 32);

        int total_new = 0;
        auto append_data = [this, &total_new](const uint8_t* data, uint16_t len) {
            read_buf.insert(read_buf.end(), data, data + len);
            total_new += len;
        };

        if (nb_rx > 0) {
            auto result = tcp->process_rx(pkts, nb_rx, append_data);
            if (!result) {
                SPDLOG_LOGGER_WARN(detail::tls_logger(),
                    "TCP rx error during BIO poll: {}", result.error());
                return -1;
            }
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
        }
        return 0; // Timeout
    }
};

/// Custom BIO write: sends data through TCP session.
inline int bio_write(BIO* bio, const char* data, int len) {
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
inline int bio_read(BIO* bio, char* buf, int len) {
    auto* ctx = static_cast<BioContext*>(BIO_get_data(bio));
    if (!ctx || !ctx->tcp || len <= 0) return -1;

    BIO_clear_retry_flags(bio);

    // If buffer is exhausted, busy-wait poll for more data
    size_t available = ctx->read_buf.size() - ctx->read_pos;
    if (available == 0) {
        // Compact buffer
        ctx->read_buf.clear();
        ctx->read_pos = 0;

        // Busy-wait poll DPDK rx until data arrives or timeout
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
inline long bio_ctrl(BIO* /*bio*/, int cmd, long /*num*/, void* /*ptr*/) {
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

/// Create the custom BIO method (singleton).
inline const BIO_METHOD* bio_method() {
    static BIO_METHOD* method = [] {
        auto* m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK,
                               "dpdk_tcp_bio");
        BIO_meth_set_write(m, bio_write);
        BIO_meth_set_read(m, bio_read);
        BIO_meth_set_ctrl(m, bio_ctrl);
        return m;
    }();
    return method;
}

} // namespace bio_dpdk

// ─────────────────────────────────────────────────────────────────────────────
// TLS Session
// ─────────────────────────────────────────────────────────────────────────────

/// TLS session wrapping a TcpSession with aws-lc/BoringSSL.
///
/// After handshake, session keys can be extracted for hot-path AEAD
/// operations, bypassing the SSL_* API on the data plane.
class TlsSession {
public:
    /// Create a TLS session over an established TCP connection.
    static std::expected<TlsSession, std::string>
    create(TcpSession& tcp, rte_mempool* pool, const TlsConfig& config) {
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
        BIO* bio = BIO_new(bio_dpdk::bio_method());
        if (!bio) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            return std::unexpected("BIO_new failed");
        }

        // Set up BIO context
        auto bio_ctx = std::make_unique<bio_dpdk::BioContext>();
        bio_ctx->tcp = &tcp;
        bio_ctx->pool = pool;
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

    /// Perform TLS 1.3 handshake (blocking, polls DPDK rx through BIO).
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

    /// Extract TLS 1.3 application traffic keys for direct AEAD encryption.
    /// Call after handshake is complete. The returned TlsHotState contains
    /// the write key/IV for encryption and read key/IV for decryption.
    ///
    /// Uses AWS-LC/BoringSSL SSL_get_{read,write}_traffic_secret() to obtain
    /// the actual traffic secrets, then derives key+IV via HKDF-Expand-Label
    /// per RFC 8446 Section 7.3.
    std::expected<TlsHotState, std::string> extract_hot_state() const {
        auto log = detail::tls_logger();

        if (!handshake_done_) {
            return std::unexpected("Cannot extract keys: handshake not done");
        }

        // Determine cipher parameters from negotiated cipher
        const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl_);
        if (!cipher) {
            return std::unexpected("No cipher negotiated");
        }

        // Get the hash function for HKDF from the cipher
        const EVP_MD* md = SSL_CIPHER_get_handshake_digest(cipher);
        if (!md) {
            return std::unexpected("Cannot determine hash for cipher");
        }
        size_t hash_len = EVP_MD_size(md);

        // Determine key length from cipher name:
        //   TLS_AES_128_GCM_SHA256 → 16 bytes
        //   TLS_AES_256_GCM_SHA384 → 32 bytes
        //   TLS_CHACHA20_POLY1305_SHA256 → 32 bytes
        const char* cipher_name = SSL_CIPHER_get_name(cipher);
        size_t key_len = (cipher_name && strstr(cipher_name, "128")) ? 16 : 32;
        size_t iv_len = tls_const::kTls13NonceLen; // Always 12 for TLS 1.3

        SPDLOG_LOGGER_DEBUG(log,
            "Cipher: {}, key_len={}, iv_len={}, hash_len={}",
            SSL_CIPHER_get_name(cipher), key_len, iv_len, hash_len);

        // Extract traffic secrets from SSL session.
        // On entry, out_len must contain the buffer size; on exit, actual secret length.
        uint8_t write_secret[EVP_MAX_MD_SIZE];
        uint8_t read_secret[EVP_MAX_MD_SIZE];
        size_t write_secret_len = sizeof(write_secret);
        size_t read_secret_len = sizeof(read_secret);

        if (!SSL_get_write_traffic_secret(ssl_, write_secret, &write_secret_len)) {
            auto err = detail::ssl_error_string();
            SPDLOG_LOGGER_ERROR(log, "SSL_get_write_traffic_secret failed: {}", err);
            return std::unexpected("Failed to get write traffic secret");
        }
        if (!SSL_get_read_traffic_secret(ssl_, read_secret, &read_secret_len)) {
            auto err = detail::ssl_error_string();
            SPDLOG_LOGGER_ERROR(log, "SSL_get_read_traffic_secret failed: {}", err);
            return std::unexpected("Failed to get read traffic secret");
        }

        SPDLOG_LOGGER_DEBUG(log,
            "Traffic secrets: write_len={}, read_len={}",
            write_secret_len, read_secret_len);

        // TLS 1.3 HKDF-Expand-Label to derive key and IV from traffic secret.
        // Label format: "tls13 " + label, with empty context.
        // key  = HKDF-Expand-Label(secret, "key", "", key_len)
        // iv   = HKDF-Expand-Label(secret, "iv",  "", iv_len)
        auto hkdf_expand_label = [&](const uint8_t* secret, size_t secret_len,
                                     const char* label, size_t label_len,
                                     uint8_t* out, size_t out_len)
                -> bool {
            // Build HkdfLabel structure per RFC 8446 Section 7.1:
            //   struct { uint16 length; opaque label<7..255>; opaque context<0..255>; }
            //   label = "tls13 " + label
            uint8_t hkdf_label[256];
            size_t pos = 0;

            // length (2 bytes, big-endian)
            hkdf_label[pos++] = static_cast<uint8_t>(out_len >> 8);
            hkdf_label[pos++] = static_cast<uint8_t>(out_len);

            // label: "tls13 " + label
            const char* prefix = "tls13 ";
            size_t prefix_len = 6;
            size_t total_label_len = prefix_len + label_len;
            hkdf_label[pos++] = static_cast<uint8_t>(total_label_len);
            std::memcpy(hkdf_label + pos, prefix, prefix_len);
            pos += prefix_len;
            std::memcpy(hkdf_label + pos, label, label_len);
            pos += label_len;

            // context (empty)
            hkdf_label[pos++] = 0;

            // HKDF-Expand(secret, hkdf_label, out_len)
            return HKDF_expand(out, out_len, md, secret, secret_len,
                               hkdf_label, pos) == 1;
        };

        TlsHotState state{};

        // Derive write key + IV
        if (!hkdf_expand_label(write_secret, write_secret_len,
                               "key", 3, state.write.key, key_len)) {
            return std::unexpected("HKDF-Expand-Label failed for write key");
        }
        if (!hkdf_expand_label(write_secret, write_secret_len,
                               "iv", 2, state.write.iv, iv_len)) {
            return std::unexpected("HKDF-Expand-Label failed for write IV");
        }

        // Derive read key + IV
        if (!hkdf_expand_label(read_secret, read_secret_len,
                               "key", 3, state.read.key, key_len)) {
            return std::unexpected("HKDF-Expand-Label failed for read key");
        }
        if (!hkdf_expand_label(read_secret, read_secret_len,
                               "iv", 2, state.read.iv, iv_len)) {
            return std::unexpected("HKDF-Expand-Label failed for read IV");
        }

        // Sequence numbers must continue from where SSL left off.
        // SSL_get_{read,write}_sequence returns the next expected sequence number
        // after all handshake + upgrade records have been processed.
        state.write.seq = SSL_get_write_sequence(ssl_);
        state.read.seq  = SSL_get_read_sequence(ssl_);
        state.key_len = static_cast<uint16_t>(key_len);

        SPDLOG_LOGGER_DEBUG(log,
            "TLS seq: write={}, read={}", state.write.seq, state.read.seq);

        SPDLOG_LOGGER_INFO(log, "TLS session keys extracted for hot path");
        return state;
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

    SSL*                                ssl_   = nullptr;
    SSL_CTX*                            ctx_   = nullptr;
    std::unique_ptr<bio_dpdk::BioContext> bio_ctx_;
    TlsConfig                           config_;
    bool                                handshake_done_ = false;
};

} // namespace eph::dpdk
