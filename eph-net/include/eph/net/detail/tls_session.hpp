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
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/mem.h>     // OPENSSL_cleanse
#include <openssl/ssl.h>
#include <openssl/x509.h>    // X509_get_X509_PUBKEY, i2d_X509_PUBKEY

#include "eph/core/detail/logger.hpp"
#include "eph/net/detail/tls_constants.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @return Pointer to the "transport.tls" spdlog logger.
inline spdlog::logger* tls_logger() {
    static auto* l = ::eph::core::detail::make_logger("transport.tls");
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
/// @brief Duck-typed template: any type providing `send`, `poll_rx`, `state`
/// and related methods compatible with the `eph::net::TcpTransport` concept
/// works. We do not reference the concept by name in the template-parameter
/// constraint to keep this header buildable without pulling in the full
/// concept definition (a minor compile-time-only concern); the
/// `static_assert(eph::net::TcpTransport<TcpSession<>>, …)` in
/// `eph-net-dpdk/include/eph/dpdk/tcp.hpp` is the authoritative compliance
/// check for the in-tree DPDK backend, and the kernel
/// `eph::net::kernel::detail::ByteSocket` adapter ports the same shape.
/// Method-set mismatches surface as ordinary template instantiation errors.
template <class TcpImpl>
class TlsSession {

    // ─────────────────────────────────────────────────────────────────────────
    // Nested BIO context and callbacks
    // ─────────────────────────────────────────────────────────────────────────

    /// BIO context: holds pointer to TcpImpl and intermediate buffers.
    /// @warning Not thread-safe. TlsSession must be used from a single thread.
    /// BIO callbacks access this struct without synchronization; concurrent
    /// use from multiple threads is undefined behavior.
    ///
    /// Also doubles as the "session-ticket box": the SSL object's app_data
    /// points at this struct so `new_session_cb_` (a free C function called
    /// by aws-lc when the server delivers a NewSessionTicket) can serialize
    /// the ticket via `i2d_SSL_SESSION` into `captured_ticket` without
    /// having to chase the owning TlsSession. The TlsSession itself is
    /// moved by `create()`'s return statement, so its `this` pointer is not
    /// stable; the heap-allocated BioContext is stable because TlsSession
    /// owns it via `std::unique_ptr`.
    struct BioContext {
        TcpImpl*             tcp = nullptr;
        std::vector<uint8_t> read_buf;   // Buffered data from TCP rx
        size_t               read_pos = 0;

        /// Serialized session ticket bytes captured by new_session_cb_.
        /// Empty until the server sends a NewSessionTicket post-handshake;
        /// some servers send it during the Finished flight, others
        /// asynchronously after the first application record. Capturing
        /// is best-effort — if the server never sends a ticket the field
        /// stays empty and the next reconnect simply does a full handshake.
        std::vector<uint8_t> captured_ticket{};

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

    /// SSL_CTX session-cache callback: aws-lc invokes this on the client
    /// once per NewSessionTicket handshake message. We pull the BioContext
    /// from the SSL's app_data and serialize the ticket into its
    /// `captured_ticket` field via `i2d_SSL_SESSION`.
    ///
    /// Return value contract (OpenSSL/aws-lc): non-zero takes ownership of
    /// the SSL_SESSION (caller must not free it); 0 leaves ownership with
    /// the caller (which then frees it on next handshake/SSL_free). We
    /// return 0 because we serialize the bytes immediately and don't need
    /// the live `SSL_SESSION` object — letting aws-lc free it keeps memory
    /// management symmetric with the no-resumption path.
    static int new_session_cb_(SSL* ssl, SSL_SESSION* sess) noexcept {
        [[maybe_unused]] auto log = detail::tls_logger();
        if (!ssl || !sess) return 0;

        auto* ctx = static_cast<BioContext*>(SSL_get_app_data(ssl));
        if (!ctx) {
            SPDLOG_LOGGER_WARN(log,
                "TlsSession::new_session_cb_: app_data is null — "
                "ticket will be discarded");
            return 0;
        }

        // i2d_SSL_SESSION with NULL `out` returns the required buffer
        // size; with a non-null `*out` it writes into the buffer and
        // advances the pointer.
        int needed = i2d_SSL_SESSION(sess, nullptr);
        if (needed <= 0) {
            SPDLOG_LOGGER_WARN(log,
                "TlsSession::new_session_cb_: i2d_SSL_SESSION sizing "
                "returned {}", needed);
            return 0;
        }

        std::vector<uint8_t> buf(static_cast<size_t>(needed));
        uint8_t* p = buf.data();
        int written = i2d_SSL_SESSION(sess, &p);
        if (written <= 0) {
            SPDLOG_LOGGER_WARN(log,
                "TlsSession::new_session_cb_: i2d_SSL_SESSION serialize "
                "returned {}", written);
            return 0;
        }
        buf.resize(static_cast<size_t>(written));

        SPDLOG_LOGGER_DEBUG(log,
            "TlsSession::new_session_cb_: captured TLS ticket ({} bytes)",
            written);
        ctx->captured_ticket = std::move(buf);
        return 0;  // we don't take ownership — aws-lc frees the SSL_SESSION
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

        SPDLOG_LOGGER_DEBUG(log,
            "Creating TLS session for host: {} (resumption_ticket={}B)",
            config.hostname, config.tls_resumption_ticket.size());

        // Create SSL context
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            auto err = detail::ssl_error_string();
            SPDLOG_LOGGER_ERROR(log, "SSL_CTX_new failed: {}", err);
            return std::unexpected(std::format("SSL_CTX_new failed: {}", err));
        }

        // Enable client-side session caching so SSL_CTX_sess_set_new_cb fires
        // when the server delivers a NewSessionTicket. SSL_SESS_CACHE_CLIENT
        // is the OpenSSL/aws-lc switch that makes the new_session callback
        // run on the client side; without it the callback is a no-op.
        SSL_CTX_set_session_cache_mode(ctx,
            SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_sess_set_new_cb(ctx, &TlsSession::new_session_cb_);

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

        // Hostname verification — `SSL_VERIFY_PEER` alone validates the
        // chain against the trust store but does NOT bind the cert
        // identity to `hostname`. Without `SSL_set1_host`, a server
        // presenting a chain trusted by the local CA store for ANY
        // hostname is accepted — exactly the "trusted CA, wrong domain"
        // MITM class. The TlsConfig::verify_peer doc explicitly promises
        // "Verify server certificate chain AND hostname", so wire it
        // here. aws-lc / BoringSSL inherit this from upstream OpenSSL
        // 1.0.2+. We pass the SNI hostname as the expected name; if the
        // server cert SAN/CN doesn't match, the handshake fails with
        // X509_V_ERR_HOSTNAME_MISMATCH at SSL_do_handshake time.
        if (config.verify_peer && !config.hostname.empty()) {
            if (SSL_set1_host(ssl, config.hostname.c_str()) != 1) {
                auto host_err = detail::ssl_error_string();
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                SPDLOG_LOGGER_ERROR(log,
                    "SSL_set1_host('{}') failed with verify_peer=true: {}",
                    config.hostname, host_err);
                return std::unexpected(std::format(
                    "SSL_set1_host('{}') failed: {}",
                    config.hostname, host_err));
            }
            SPDLOG_LOGGER_DEBUG(log,
                "TLS hostname verification enabled for '{}'",
                config.hostname);
        } else if (config.verify_peer && config.hostname.empty()) {
            // verify_peer=true with empty hostname means the caller has
            // explicitly opted out of hostname binding (e.g. connecting
            // by IP literal in test harnesses). Surface a WARN so this
            // is auditable in logs — the chain is still verified, just
            // not the identity.
            SPDLOG_LOGGER_WARN(log,
                "TLS verify_peer=true but hostname is empty — chain is "
                "verified, but cert identity binding is OFF (any trusted "
                "cert for any hostname is accepted; only safe when "
                "connecting to a fixed pinned peer)");
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

        // Stash the BioContext pointer on the SSL so `new_session_cb_`
        // can find it when the server sends a NewSessionTicket. The
        // BioContext is heap-allocated (unique_ptr), so the pointer
        // stays stable across the TlsSession move at the end of this
        // function.
        SSL_set_app_data(ssl, bio_ctx.get());

        // Apply caller-supplied resumption ticket BEFORE SSL_do_handshake
        // so the next ClientHello carries the pre_shared_key extension.
        // Failure mode is graceful: if the ticket is corrupt or expired
        // we log and proceed to a full handshake — connection still
        // succeeds.
        if (!config.tls_resumption_ticket.empty()) {
            const uint8_t* p = config.tls_resumption_ticket.data();
            SSL_SESSION* restored = d2i_SSL_SESSION(
                nullptr, &p,
                static_cast<long>(config.tls_resumption_ticket.size()));
            if (!restored) {
                SPDLOG_LOGGER_WARN(log,
                    "TlsSession::create: d2i_SSL_SESSION failed for "
                    "resumption ticket ({} bytes) — proceeding with "
                    "full handshake: {}",
                    config.tls_resumption_ticket.size(),
                    detail::ssl_error_string());
            } else {
                if (SSL_set_session(ssl, restored) != 1) {
                    SPDLOG_LOGGER_WARN(log,
                        "TlsSession::create: SSL_set_session rejected "
                        "the restored ticket — proceeding with full "
                        "handshake: {}",
                        detail::ssl_error_string());
                } else {
                    SPDLOG_LOGGER_DEBUG(log,
                        "TlsSession::create: resumption ticket applied "
                        "({} bytes), abbreviated handshake will be "
                        "attempted",
                        config.tls_resumption_ticket.size());
                }
                // SSL_set_session bumps the refcount on success, so we
                // free our local handle either way.
                SSL_SESSION_free(restored);
            }
        }

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
            // Only attempt TLS close_notify if handshake completed,
            // the underlying TCP is likely still alive, AND we were not
            // explicitly told to suppress the alert (e.g. after
            // extract_hot_state() transferred key material to an
            // in-place AEAD context that takes over the connection).
            if (handshake_done_ && !suppress_close_notify_ &&
                bio_ctx_ && bio_ctx_->tcp &&
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
        , handshake_done_(other.handshake_done_)
        , suppress_close_notify_(other.suppress_close_notify_) {
        other.ssl_ = nullptr;
        other.ctx_ = nullptr;
        other.handshake_done_ = false;
        other.suppress_close_notify_ = false;
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
            suppress_close_notify_ = other.suppress_close_notify_;
            other.ssl_ = nullptr;
            other.ctx_ = nullptr;
            other.handshake_done_ = false;
            other.suppress_close_notify_ = false;
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
                // Match cipher_name()'s null-safety: aws-lc post-handshake
                // always returns a non-null cipher, but `SSL_CIPHER_get_name
                // (nullptr)` is UB and we already guard against it in the
                // public getter — keep the log path equally hardened so a
                // stub / mock TLS impl that returns null cipher cannot crash
                // the INFO log.
                const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl_);
                SPDLOG_LOGGER_INFO(log,
                    "TLS handshake complete: version={}, cipher={}",
                    SSL_get_version(ssl_),
                    cipher ? SSL_CIPHER_get_name(cipher) : "<unknown>");

                // ─── Drain post-handshake records (NewSessionTicket capture) ───
                //
                // In TLS 1.3 the server delivers NewSessionTicket (NSE)
                // messages as separate post-handshake records. Two timing
                // patterns matter for `new_session_cb_` capture:
                //
                //   A) The server packs NSE(s) into the same flight as
                //      ServerFinished (typical OpenSSL 1.1+/3.x default).
                //      The NSE bytes are already buffered by `bio_read_cb`
                //      when `SSL_do_handshake` returns 1; one extra
                //      `SSL_peek` call drives the record-layer past them
                //      and fires `new_session_cb_` synchronously.
                //
                //   B) The server defers NSE until its next `SSL_write`
                //      (aws-lc 1.x and BoringSSL default: NSEs are written
                //      out only when the server's BIO is next driven, e.g.
                //      during the SSL_write that emits the application-
                //      level response such as the WS HTTP 101). In this
                //      case the kernel TLS architecture cannot capture the
                //      ticket at all: `extract_hot_state()` runs as soon
                //      as `tls_state::handshake()` returns and dismantles
                //      the SSL session before the WS upgrade exchange
                //      surfaces NSE bytes. This is a known structural
                //      limitation — see the SKIP rationale in
                //      tests/integration/test_tls_resumption.cpp and the
                //      `take_post_handshake_ticket` follow-up note in
                //      `tls_state.hpp`.
                //
                // The drain below targets pattern (A). It is also a useful
                // defensive measure: it reduces the chance of a stray
                // post-handshake record (NSE, KeyUpdate, alert) sitting
                // unprocessed in the BIO when `extract_hot_state()` later
                // hands the connection over to the AEAD-only hot path.
                //
                // The drain is intentionally **non-blocking**: it never
                // waits on the wire. If `bio_ctx_->read_buf` already
                // contains pending bytes (pattern A) we surface them via
                // `SSL_peek`; otherwise we exit immediately. Burning
                // 50-500ms here on every handshake — in the hope that an
                // aws-lc server might decide to send NSEs early (it
                // doesn't) — would inflate connect latency without
                // moving the needle on resumption capture.
                //
                // Bound: at most `kDrainMaxRounds` (4) `SSL_peek` calls.
                // Each round either:
                //   • returns >0 → real app data already queued (server
                //     packed NSE + app data into one flight); stop and
                //     leave the data for the caller's hot-path read,
                //   • returns WANT_READ with NSE buffer drained → loop,
                //   • returns WANT_READ with no buffered data → stop,
                //   • any other error → stop and let the hot path
                //     resurface the condition naturally.
                constexpr int kDrainMaxRounds = 4;
                for (int i = 0; i < kDrainMaxRounds; ++i) {
                    if (!bio_ctx_) break;

                    // Cheap non-blocking poll: a single recv(MSG_DONTWAIT).
                    // Picks up any NSE bytes the kernel TCP buffer has
                    // already received but `bio_read_cb` hasn't yet
                    // surfaced.
                    int polled = bio_ctx_->poll_rx();
                    if (polled < 0) {
                        // TCP error — let the normal record-layer path
                        // raise it; do not promote to handshake failure.
                        SPDLOG_LOGGER_TRACE(log,
                            "TlsSession::handshake: post-handshake drain "
                            "saw TCP poll error on round {}, stopping",
                            i);
                        break;
                    }
                    if (polled == 0 &&
                        bio_ctx_->read_pos == bio_ctx_->read_buf.size()) {
                        // Nothing buffered, nothing new on the wire —
                        // server hasn't packed NSE with Finished. Quit
                        // without burning further wall-clock time.
                        SPDLOG_LOGGER_TRACE(log,
                            "TlsSession::handshake: post-handshake drain "
                            "round {} found no data, stopping",
                            i);
                        break;
                    }

                    // Drive the SSL record-layer state machine. SSL_peek
                    // processes pending records — including NSE, which
                    // fires `new_session_cb_` — without consuming the
                    // application data behind them.
                    ERR_clear_error();
                    char peek_buf[1];
                    int pr = SSL_peek(ssl_, peek_buf, sizeof(peek_buf));
                    if (pr > 0) {
                        // Real app data already buffered (server packed
                        // NSE + app data into one flight). Stop the
                        // drain; the caller's hot-path read will pick
                        // these bytes up through the AEAD context.
                        SPDLOG_LOGGER_TRACE(log,
                            "TlsSession::handshake: post-handshake drain "
                            "round {} surfaced {} byte(s) of app data, "
                            "leaving for caller",
                            i, pr);
                        break;
                    }
                    int perr = SSL_get_error(ssl_, pr);
                    if (perr != SSL_ERROR_WANT_READ &&
                        perr != SSL_ERROR_WANT_WRITE) {
                        // Other condition — let the hot path resurface
                        // it on its first read attempt.
                        SPDLOG_LOGGER_TRACE(log,
                            "TlsSession::handshake: post-handshake drain "
                            "SSL_peek err={} on round {}, stopping",
                            perr, i);
                        break;
                    }
                    SPDLOG_LOGGER_TRACE(log,
                        "TlsSession::handshake: post-handshake drain "
                        "round {} processed {} TCP byte(s), "
                        "peek=WANT_READ",
                        i, polled);
                    // Loop again — TLS 1.3 servers usually emit two NSEs.
                }

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
    /// called AFTER the WS HTTP Upgrade request/response, which the
    /// `TlsWsSink` byte-sink drives through `encrypt_for_send` /
    /// `decrypt_into` (both internally call `SSL_write` / `SSL_read`).
    ///
    /// Key length is determined dynamically from the negotiated cipher:
    ///   AES_128_GCM -> 16-byte key    AES_256_GCM -> 32-byte key
    [[nodiscard]] std::expected<TlsHotState, std::string> extract_hot_state() {
        [[maybe_unused]] auto log = detail::tls_logger();

        if (!handshake_done_) {
            return std::unexpected("Cannot extract keys: handshake not done");
        }

        // Determine key length from negotiated cipher. Note we deliberately
        // defer setting `suppress_close_notify_ = true` until *after* every
        // fallible step below succeeds: the suppress flag's contract is
        // "the caller has taken over the data path — do not send a
        // close_notify when ~TlsSession runs". On a mid-extract failure
        // the caller has NOT taken over, so the destructor should still
        // attempt the orderly close (or be cleanly ignored if the session
        // is being torn down anyway). Setting it eagerly mis-classifies
        // the failure path as "takeover succeeded" and silently degrades
        // the wire-level shutdown.
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
            OPENSSL_cleanse(write_secret, sizeof(write_secret));
            return std::unexpected("SSL_get_read_traffic_secret failed");
        }

        TlsHotState state{};

        if (!tls_keygen::derive_key_iv(write_secret, ws_len,
                state.write.ki.key, key_len,
                state.write.ki.iv, tls_const::kTls13NonceLen)) {
            OPENSSL_cleanse(write_secret, sizeof(write_secret));
            OPENSSL_cleanse(read_secret, sizeof(read_secret));
            return std::unexpected("HKDF derive failed for write key");
        }

        if (!tls_keygen::derive_key_iv(read_secret, rs_len,
                state.read.ki.key, key_len,
                state.read.ki.iv, tls_const::kTls13NonceLen)) {
            OPENSSL_cleanse(write_secret, sizeof(write_secret));
            OPENSSL_cleanse(read_secret, sizeof(read_secret));
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
        // Commit the takeover only now — every fallible step above has
        // succeeded. ~TlsSession will skip close_notify because the
        // caller now owns the AEAD context (see suppress_close_notify_
        // doc comment). Rationale-deep: setting this earlier would
        // suppress close_notify even on failure paths where the caller
        // never actually took over.
        suppress_close_notify_ = true;
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

    // ─────────────────────────────────────────────────────────────────────────
    // Session resumption
    // ─────────────────────────────────────────────────────────────────────────

    /// True if the just-completed handshake was a TLS 1.3 resumption
    /// (PSK / abbreviated). Reflects `SSL_session_reused`. Only meaningful
    /// after `handshake()` returned success.
    [[nodiscard]] bool was_resumed() const noexcept {
        return ssl_ && SSL_session_reused(ssl_) == 1;
    }

    /// Move-out the most recently captured resumption ticket. The bytes
    /// are produced by `i2d_SSL_SESSION` against the server-issued
    /// NewSessionTicket; pass them back as
    /// `TlsConfig::tls_resumption_ticket` on the next connect to attempt
    /// a 1-RTT abbreviated handshake.
    ///
    /// Returns an empty vector if the server never sent a ticket (or
    /// the handshake hasn't happened yet). Move-out semantics: a
    /// subsequent call returns empty until the server sends another
    /// ticket. Servers typically send tickets immediately after the
    /// handshake completes; HFT venues like Binance also rotate
    /// tickets every few minutes, so polling this getter periodically
    /// (e.g. via the reconnect orchestrator) keeps the freshest one.
    [[nodiscard]] std::vector<uint8_t> take_resumption_ticket() noexcept {
        if (!bio_ctx_) return {};
        return std::move(bio_ctx_->captured_ticket);
    }

    /// Read-only view of the captured ticket (does not move-out).
    /// Useful for tests / diagnostics where the caller wants to assert
    /// "a ticket was captured" without consuming it.
    [[nodiscard]] std::span<const uint8_t> peek_resumption_ticket() const noexcept {
        if (!bio_ctx_) return {};
        return std::span<const uint8_t>(bio_ctx_->captured_ticket.data(),
                                         bio_ctx_->captured_ticket.size());
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
    bool                          handshake_done_        = false;
    /// When true, ~TlsSession skips SSL_shutdown (close_notify). Set by
    /// extract_hot_state() because the caller takes over key material and
    /// continues using the TCP connection with its own AEAD context.
    bool                          suppress_close_notify_ = false;
};

} // namespace eph::net
