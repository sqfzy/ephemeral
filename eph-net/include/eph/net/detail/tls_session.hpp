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
#include <climits>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/mem.h>     // OPENSSL_cleanse
#include <openssl/ssl.h>
#include <openssl/x509.h>    // X509_get_X509_PUBKEY, i2d_X509_PUBKEY

#include "eph/net/detail/tls_constants.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @return Pointer to the "transport.tls" spdlog logger.
inline spdlog::logger* tls_logger() {
    static auto l = [] {
        auto lg = spdlog::get("transport.tls");
        if (!lg) lg = spdlog::stdout_color_mt("transport.tls");
        return lg;
    }();
    return l.get();
}

/// Get the last OpenSSL error as a string.
inline std::string ssl_error_string() {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return std::string(buf);
}

} // namespace detail

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
/// @brief Duck-typed template: any type providing `send`, `poll_rx`, `state` and
/// related methods compatible with the legacy `TcpTransport` concept works.
/// The concept constraint was removed in Phase 7 because the concept itself
/// is deleted; the method-set requirement survives as ordinary template
/// instantiation errors.
template <class TcpImpl>
class TlsSession {

    // ─────────────────────────────────────────────────────────────────────────
    // Nested BIO context and callbacks
    // ─────────────────────────────────────────────────────────────────────────

    /// BIO context: holds pointer to TcpImpl and intermediate buffers.
    /// @warning Not thread-safe. TlsSession must be used from a single thread.
    /// BIO callbacks access this struct without synchronization; concurrent
    /// use from multiple threads is undefined behavior.
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

        // Cap to INT_MAX to avoid signed overflow on the int return.
        return static_cast<int>(std::min(total_sent, static_cast<size_t>(INT_MAX)));
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
    [[nodiscard]] static std::expected<TlsSession, std::string>
    create(TcpImpl& tcp, const TlsConfig& config) {
        [[maybe_unused]] auto log = detail::tls_logger();

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
            if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
                if (config.verify_peer) {
                    auto err = detail::ssl_error_string();
                    SSL_CTX_free(ctx);
                    SPDLOG_LOGGER_ERROR(log,
                        "SSL_CTX_set_default_verify_paths failed with "
                        "verify_peer=true: no CA certificates available: {}",
                        err);
                    return std::unexpected(std::format(
                        "No CA certificates available: {}", err));
                }
                SPDLOG_LOGGER_WARN(log,
                    "SSL_CTX_set_default_verify_paths failed: "
                    "default CA certificates may not be available "
                    "(continuing because verify_peer=false)");
            }
        }

        if (config.verify_peer) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        }

        // Load client certificate and key for mutual TLS (mTLS)
        if (!config.client_cert_path.empty() && !config.client_key_path.empty()) {
            if (SSL_CTX_use_certificate_chain_file(
                    ctx, config.client_cert_path.c_str()) != 1) {
                auto err = detail::ssl_error_string();
                SSL_CTX_free(ctx);
                SPDLOG_LOGGER_ERROR(log,
                    "Failed to load client certificate {}: {}",
                    config.client_cert_path, err);
                return std::unexpected(std::format(
                    "Failed to load client certificate: {}", err));
            }
            if (SSL_CTX_use_PrivateKey_file(
                    ctx, config.client_key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
                auto err = detail::ssl_error_string();
                SSL_CTX_free(ctx);
                SPDLOG_LOGGER_ERROR(log,
                    "Failed to load client private key {}: {}",
                    config.client_key_path, err);
                return std::unexpected(std::format(
                    "Failed to load client private key: {}", err));
            }
            if (SSL_CTX_check_private_key(ctx) != 1) {
                auto err = detail::ssl_error_string();
                SSL_CTX_free(ctx);
                SPDLOG_LOGGER_ERROR(log,
                    "Client certificate/key mismatch: {}", err);
                return std::unexpected(std::format(
                    "Client certificate/key mismatch: {}", err));
            }
            SPDLOG_LOGGER_DEBUG(log, "mTLS: loaded client certificate {}",
                config.client_cert_path);
        }

        // Create SSL object
        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            auto err = detail::ssl_error_string();
            SSL_CTX_free(ctx);
            SPDLOG_LOGGER_ERROR(log, "SSL_new failed: {}", err);
            return std::unexpected(std::format("SSL_new failed: {}", err));
        }

        // Set SNI hostname for virtual hosting and certificate matching.
        // When verify_peer is enabled, SNI is critical: the server uses it
        // to select the correct certificate.  Failing silently would cause
        // an opaque handshake failure or, worse, verification against the
        // wrong certificate.
        if (!config.hostname.empty()) {
            if (!SSL_set_tlsext_host_name(ssl, config.hostname.c_str())) {
                auto sni_err = detail::ssl_error_string();
                if (config.verify_peer) {
                    SSL_free(ssl);
                    SSL_CTX_free(ctx);
                    SPDLOG_LOGGER_ERROR(log,
                        "Failed to set SNI hostname '{}' with verify_peer=true: {}",
                        config.hostname, sni_err);
                    return std::unexpected(std::format(
                        "Failed to set SNI hostname '{}': {}",
                        config.hostname, sni_err));
                }
                SPDLOG_LOGGER_WARN(log,
                    "Failed to set SNI hostname '{}': {} "
                    "(continuing because verify_peer=false)",
                    config.hostname, sni_err);
            }
        }

        // Mark as client-side (required by aws-lc before SSL_do_handshake)
        SSL_set_connect_state(ssl);

        // Create and attach custom BIO
        BIO* bio = BIO_new(bio_method());
        if (!bio) {
            auto err = detail::ssl_error_string();
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            SPDLOG_LOGGER_ERROR(log, "BIO_new failed: {}", err);
            return std::unexpected(std::format("BIO_new failed: {}", err));
        }

        // BIO context: TlsSession retains ownership; BIO holds a non-owning
        // pointer.  Safe because ~TlsSession calls SSL_free (which destroys the
        // BIO) before bio_ctx_ is freed.  Single-threaded usage guaranteed by
        // the transport's RX/TX worker design.
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
            // Only attempt TLS close_notify if handshake completed and
            // the underlying TCP is likely still alive.  When the TCP
            // connection is already broken, SSL_shutdown would block on
            // BIO read/write or return an error—both are harmless to
            // ignore, but we skip the attempt entirely to avoid noisy
            // error logs and potential delays.
            if (handshake_done_ && bio_ctx_ && bio_ctx_->tcp &&
                bio_ctx_->tcp->is_established()) {
                // Best-effort: ignore return value since we are tearing down
                SSL_shutdown(ssl_);
            }
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
    [[nodiscard]] std::expected<void, std::string> handshake() {
        [[maybe_unused]] auto log = detail::tls_logger();

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

                // SPKI certificate pin verification (soft pinning)
                if (!config_.pinned_spki_sha256.empty()) {
                    auto pin_result = verify_spki_pin();
                    if (!pin_result) {
                        handshake_done_ = false;
                        return std::unexpected(pin_result.error());
                    }
                }

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
                    return std::unexpected(std::format(
                        "tls_timeout: handshake exceeded {}ms",
                        config_.handshake_timeout.count()));
                }
                continue;
            }

            // Fatal error
            auto ssl_err = detail::ssl_error_string();
            SPDLOG_LOGGER_ERROR(log,
                "TLS handshake failed: ssl_error={}, detail={}",
                err, ssl_err);
            // Classify error for callers: certificate/auth failures vs other
            const char* prefix = (err == SSL_ERROR_SSL &&
                (ssl_err.find("certificate") != std::string::npos ||
                 ssl_err.find("alert") != std::string::npos))
                ? "tls_rejected" : "tls_error";
            return std::unexpected(std::format(
                "{}: handshake failed (err={}): {}", prefix, err, ssl_err));
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Handshake-phase I/O (for WebSocket Upgrade after TLS handshake)
    // ─────────────────────────────────────────────────────────────────────────

    /// Write data through TLS during handshake/upgrade phase only.
    /// NOT for hot-path use — hot path uses TlsRecordCrypto directly.
    /// Must be called from the control thread (single-threaded).
    [[nodiscard]] std::expected<int, std::string> handshake_write(const void* data, int len) {
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
    [[nodiscard]] std::expected<int, std::string> handshake_read(void* buf, int len) {
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
    /// via HKDF-Expand-Label. Reads the current seq numbers from SSL into
    /// the returned `TlsHotState` so the hot-path crypto starts from the
    /// SSL session's current next-record sequence.
    ///
    /// **Pre-condition**: the caller MUST guarantee that no further
    /// `SSL_write` / `SSL_read` will occur on this session after this call.
    /// This is a one-shot snapshot — once a `TlsRecordCrypto` built from
    /// the returned state is in use, any subsequent SSL operation will
    /// silently advance the SSL session's internal seq counters while the
    /// hot-path counters stay frozen at the snapshot, causing immediate
    /// `bad_record_mac` AEAD failures on the very next record.
    ///
    /// Concretely: when the WebSocket framer is in use, this must be
    /// called AFTER the WS HTTP Upgrade request/response (which goes
    /// through `handshake_write`/`handshake_read` and therefore
    /// `SSL_write`/`SSL_read`).
    ///
    /// Key length is determined dynamically from the negotiated cipher:
    ///   AES_128_GCM -> 16-byte key    AES_256_GCM -> 32-byte key
    [[nodiscard]] std::expected<TlsHotState, std::string> extract_hot_state() const {
        [[maybe_unused]] auto log = detail::tls_logger();

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
                state.write.ki.key, key_len,
                state.write.ki.iv, tls_const::kTls13NonceLen)) {
            return std::unexpected("HKDF derive failed for write key");
        }

        if (!tls_keygen::derive_key_iv(read_secret, rs_len,
                state.read.ki.key, key_len,
                state.read.ki.iv, tls_const::kTls13NonceLen)) {
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

    /// Verify peer certificate SPKI hash against pinned hashes.
    /// Called after successful TLS handshake when pinned_spki_sha256 is non-empty.
    /// @return success if pin matches or soft-pin callback allows continuation,
    ///         error string if pin mismatch and callback rejects (or no cert).
    [[nodiscard]] std::expected<void, std::string> verify_spki_pin() {
        [[maybe_unused]] auto log = detail::tls_logger();

        // Get peer certificate
        X509* cert = SSL_get_peer_certificate(ssl_);
        if (!cert) {
            SPDLOG_LOGGER_WARN(log,
                "SPKI pin check: no peer certificate available");
            return std::unexpected("spki_pin: no peer certificate");
        }

        // Extract SPKI in DER format
        EVP_PKEY* pubkey_handle = X509_get0_pubkey(cert);
        X509_PUBKEY* spki = X509_get_X509_PUBKEY(cert);
        if (!spki) {
            X509_free(cert);
            SPDLOG_LOGGER_ERROR(log,
                "SPKI pin check: failed to extract SPKI from peer certificate");
            return std::unexpected("spki_pin: failed to extract SPKI");
        }
        (void)pubkey_handle; // only spki is needed

        // Serialize SPKI to DER
        uint8_t* der_buf = nullptr;
        int der_len = i2d_X509_PUBKEY(spki, &der_buf);
        if (der_len <= 0 || !der_buf) {
            X509_free(cert);
            SPDLOG_LOGGER_ERROR(log,
                "SPKI pin check: i2d_X509_PUBKEY failed (der_len={})", der_len);
            return std::unexpected("spki_pin: SPKI DER serialization failed");
        }

        // Compute base64-encoded SHA-256 hash
        auto actual_hash = spki_pin::compute_spki_sha256_b64(
            der_buf, static_cast<size_t>(der_len));
        OPENSSL_free(der_buf);
        X509_free(cert);

        if (actual_hash.empty()) {
            SPDLOG_LOGGER_ERROR(log,
                "SPKI pin check: SHA-256 hash computation failed");
            return std::unexpected("spki_pin: hash computation failed");
        }

        SPDLOG_LOGGER_DEBUG(log,
            "SPKI pin check: actual_hash={}, pin_count={}",
            actual_hash, config_.pinned_spki_sha256.size());

        // Check against pin list
        if (spki_pin::matches_any_pin(actual_hash, config_.pinned_spki_sha256)) {
            SPDLOG_LOGGER_INFO(log,
                "SPKI pin check: peer certificate matches pinned hash");
            return {};
        }

        // Mismatch — invoke callback or apply default soft-pin behavior
        SPDLOG_LOGGER_WARN(log,
            "SPKI pin MISMATCH: peer_hash={}, expected one of [{}]",
            actual_hash,
            [&] {
                std::string joined;
                for (size_t i = 0; i < config_.pinned_spki_sha256.size(); ++i) {
                    if (i > 0) joined += ", ";
                    joined += config_.pinned_spki_sha256[i];
                }
                return joined;
            }());

        if (config_.on_pin_mismatch) {
            bool allow = config_.on_pin_mismatch(actual_hash);
            if (!allow) {
                SPDLOG_LOGGER_WARN(log,
                    "SPKI pin mismatch: on_pin_mismatch callback rejected connection");
                return std::unexpected(std::format(
                    "spki_pin: mismatch rejected by callback (actual={})",
                    actual_hash));
            }
            SPDLOG_LOGGER_WARN(log,
                "SPKI pin mismatch: on_pin_mismatch callback allowed connection to continue");
        } else {
            // P0-3: Hard pin default — reject connection when pin list is set but no
            // override callback is configured. Safe default prevents MITM bypass.
            SPDLOG_LOGGER_ERROR(log,
                "SPKI pin mismatch: no on_pin_mismatch callback set, "
                "rejecting connection (configure callback to override)");
            return std::unexpected(std::format(
                "SPKI pin mismatch: actual={}, no override callback configured",
                actual_hash));
        }
        return {};
    }

    SSL*                          ssl_   = nullptr;
    SSL_CTX*                      ctx_   = nullptr;
    std::unique_ptr<BioContext>   bio_ctx_;
    TlsConfig                     config_;
    bool                          handshake_done_ = false;
};

} // namespace eph::net
