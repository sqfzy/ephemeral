/// @file mockex/tls_server.hpp
/// Minimal TLS 1.2 / 1.3 server for the mockex bench harness.
///
/// `eph-net`'s `TlsSession` is intentionally client-side only — it
/// uses `TLS_client_method()` and exposes no `accept_state` path
/// (see `eph-net/include/eph/net/detail/tls_session.hpp`). Adding
/// server-mode there would broaden the public surface beyond the
/// HFT-client-only design intent. Instead, this file gives the
/// mockex bench harness a small, self-contained TLS server wrapper
/// using `aws-lc` directly.
///
/// **Bench-only**:
///   - Self-signed cert; client side runs `verify_peer = false`
///   - Single SSL_CTX shared across connections (mockex is one-conn-at-a-time)
///   - SSL_accept is blocking (matches mockex's existing accept loop)
///   - No session resumption, no mTLS — out of scope for echo bench
///
/// Header-only per project convention. Linked via the `aws-lc`
/// xmake package (already pulled in by `eph-net`).
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>

#include <unistd.h>

#include "eph/net/detail/tls_constants.hpp"  // eph::net::TlsVersion / to_string

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <spdlog/spdlog.h>

namespace mockex::tls {

namespace detail {

/// Format the top aws-lc error from the queue, popping it.
inline std::string pop_ssl_error(const char* prefix) {
    const unsigned long e = ERR_get_error();
    if (e == 0) return std::string{prefix} + ": no SSL error in queue";
    char buf[256] = {};
    ERR_error_string_n(e, buf, sizeof(buf));
    return std::string{prefix} + ": " + buf;
}

} // namespace detail

/// Per-connection TLS state. Wraps an SSL* and the underlying TCP fd.
/// Move-only; closes both on destruction.
class TlsConn {
public:
    TlsConn() noexcept = default;
    TlsConn(SSL* ssl, int fd) noexcept : ssl_(ssl), fd_(fd) {}

    TlsConn(const TlsConn&) = delete;
    TlsConn& operator=(const TlsConn&) = delete;

    TlsConn(TlsConn&& other) noexcept
        : ssl_(std::exchange(other.ssl_, nullptr)),
          fd_(std::exchange(other.fd_, -1)) {}

    TlsConn& operator=(TlsConn&& other) noexcept {
        if (this != &other) {
            close();
            ssl_ = std::exchange(other.ssl_, nullptr);
            fd_  = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~TlsConn() noexcept { close(); }

    /// Underlying TCP fd (for poll/select diagnostics; do NOT read/write directly).
    [[nodiscard]] int fd() const noexcept { return fd_; }

    /// Blocking read of up to `n` bytes into `buf`.
    /// @return >0 bytes read, 0 on clean peer close, -1 on error (logged).
    [[nodiscard]] ssize_t read(void* buf, std::size_t n) noexcept {
        if (!ssl_) return -1;
        ERR_clear_error();
        const int r = SSL_read(ssl_, buf, static_cast<int>(n));
        if (r > 0) return r;
        const int err = SSL_get_error(ssl_, r);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;  // clean shutdown
        if (err == SSL_ERROR_SYSCALL && r == 0) return 0;  // peer closed mid-stream
        SPDLOG_DEBUG("[tls_conn] SSL_read err={} ret={}", err, r);
        return -1;
    }

    /// Blocking write of all `n` bytes from `buf`. Loops until done or error.
    /// @return n on success, -1 on error.
    [[nodiscard]] ssize_t write(const void* buf, std::size_t n) noexcept {
        if (!ssl_) return -1;
        const auto* p = static_cast<const std::uint8_t*>(buf);
        std::size_t left = n;
        while (left > 0) {
            ERR_clear_error();
            const int w = SSL_write(ssl_, p, static_cast<int>(left));
            if (w > 0) {
                p    += static_cast<std::size_t>(w);
                left -= static_cast<std::size_t>(w);
                continue;
            }
            SPDLOG_DEBUG("[tls_conn] SSL_write err={} ret={}",
                         SSL_get_error(ssl_, w), w);
            return -1;
        }
        return static_cast<ssize_t>(n);
    }

    // ── IoStream interface (matches mockex::io::RawFdStream) ─────────
    //
    // These two methods let scenario handlers (tcp_echo / ws_server / ...)
    // treat plain TCP and TLS sockets uniformly via templates instead of
    // duplicating handler bodies. Their semantics mirror eph-net's
    // posix::recv_exact / send_all.

    /// `read(2)`-like single-shot read: blocks until ≥1 byte is
    /// available; returns 0 on clean close, -1 on error.
    [[nodiscard]] ssize_t read_some(void* buf, std::size_t n) noexcept {
        return read(buf, n);
    }

    /// Block until exactly `n` bytes are read into `buf`, or return
    /// false on EOF / error / partial read.
    [[nodiscard]] bool read_exact(void* buf, std::size_t n) noexcept {
        auto* p = static_cast<std::uint8_t*>(buf);
        while (n > 0) {
            const ssize_t r = read(p, n);
            if (r <= 0) return false;
            p += static_cast<std::size_t>(r);
            n -= static_cast<std::size_t>(r);
        }
        return true;
    }

    /// Block until all `n` bytes from `buf` are written, or return
    /// false on error.
    [[nodiscard]] bool write_all(const void* buf, std::size_t n) noexcept {
        return write(buf, n) == static_cast<ssize_t>(n);
    }

    /// Tear down the TLS session and close the fd. Idempotent.
    void close() noexcept {
        if (ssl_) {
            // best-effort close_notify; if the TCP fd is already gone the
            // SSL_shutdown will return -1 which we ignore.
            (void)SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    SSL* ssl_ = nullptr;
    int  fd_  = -1;
};

/// Configuration for the TLS server context.
struct TlsServerConfig {
    std::string cert_path;   ///< Server cert (PEM)
    std::string key_path;    ///< Server private key (PEM, unencrypted)
    /// Minimum negotiable TLS version. Default `Tls13` mirrors the
    /// historical mockex behaviour. Set `Tls12` to test the eph-net
    /// client's TLS 1.2 GCM/CHACHA20 path; mockex installs the same
    /// AEAD-only cipher whitelist as the client (no CBC, ever).
    eph::net::TlsVersion min_version = eph::net::TlsVersion::Tls13;
};

/// Server-side TLS context. One instance per mockex process; wraps
/// each accepted client fd via `accept_tls()`.
class TlsServer {
public:
    /// Build a server context from a cert+key on disk. Pins min version
    /// per `cfg.min_version` (default Tls13 — mirrors the eph-net client
    /// default). When set to Tls12, an AEAD-only cipher whitelist is
    /// installed; CBC suites are forbidden at every version. Returns an
    /// error string (logged at create time) on any aws-lc failure.
    [[nodiscard]] static std::expected<std::unique_ptr<TlsServer>, std::string>
    create(const TlsServerConfig& cfg) noexcept {
        if (cfg.cert_path.empty() || cfg.key_path.empty()) {
            return std::unexpected{"TlsServer: cert_path and key_path required"};
        }

        SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx) return std::unexpected{detail::pop_ssl_error("SSL_CTX_new")};

        const int min_proto = (cfg.min_version == eph::net::TlsVersion::Tls12)
                            ? TLS1_2_VERSION : TLS1_3_VERSION;
        if (!SSL_CTX_set_min_proto_version(ctx, min_proto)) {
            SSL_CTX_free(ctx);
            return std::unexpected{detail::pop_ssl_error("set_min_proto_version")};
        }

        if (cfg.min_version == eph::net::TlsVersion::Tls12) {
            // Server-side AEAD-only cipher whitelist for TLS 1.2.
            // Same set as the client (tls_session.hpp); CBC forbidden.
            static constexpr const char* kTls12AeadOnly =
                "ECDHE-ECDSA-AES256-GCM-SHA384:"
                "ECDHE-ECDSA-AES128-GCM-SHA256:"
                "ECDHE-RSA-AES256-GCM-SHA384:"
                "ECDHE-RSA-AES128-GCM-SHA256:"
                "ECDHE-ECDSA-CHACHA20-POLY1305:"
                "ECDHE-RSA-CHACHA20-POLY1305";
            if (!SSL_CTX_set_cipher_list(ctx, kTls12AeadOnly)) {
                SSL_CTX_free(ctx);
                return std::unexpected{detail::pop_ssl_error("set_cipher_list")};
            }
        }

        if (SSL_CTX_use_certificate_file(
                ctx, cfg.cert_path.c_str(), SSL_FILETYPE_PEM) != 1) {
            std::string e =
                detail::pop_ssl_error("use_certificate_file") + " (path=" + cfg.cert_path + ")";
            SSL_CTX_free(ctx);
            return std::unexpected{std::move(e)};
        }
        if (SSL_CTX_use_PrivateKey_file(
                ctx, cfg.key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
            std::string e =
                detail::pop_ssl_error("use_PrivateKey_file") + " (path=" + cfg.key_path + ")";
            SSL_CTX_free(ctx);
            return std::unexpected{std::move(e)};
        }
        if (SSL_CTX_check_private_key(ctx) != 1) {
            std::string e = detail::pop_ssl_error("check_private_key");
            SSL_CTX_free(ctx);
            return std::unexpected{std::move(e)};
        }

        // No client cert verification (verify_peer=false on the client).
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

        SPDLOG_INFO("[tls_server] context ready (cert={}, key={}, min_version={})",
                    cfg.cert_path, cfg.key_path,
                    eph::net::to_string(cfg.min_version));

        return std::unique_ptr<TlsServer>(new TlsServer(ctx));
    }

    TlsServer(const TlsServer&) = delete;
    TlsServer& operator=(const TlsServer&) = delete;

    ~TlsServer() noexcept {
        if (ctx_) SSL_CTX_free(ctx_);
    }

    /// Wrap an accepted client fd with a TLS session and run the
    /// handshake. On failure the fd is closed for the caller and an
    /// empty TlsConn (fd_=-1) is returned (caller checks `conn.fd() < 0`).
    [[nodiscard]] TlsConn accept_tls(int cfd) noexcept {
        if (!ctx_ || cfd < 0) {
            if (cfd >= 0) ::close(cfd);
            return {};
        }

        SSL* ssl = SSL_new(ctx_);
        if (!ssl) {
            SPDLOG_WARN("[tls_server] SSL_new failed: {}",
                        detail::pop_ssl_error("SSL_new"));
            ::close(cfd);
            return {};
        }
        if (SSL_set_fd(ssl, cfd) != 1) {
            SPDLOG_WARN("[tls_server] SSL_set_fd failed: {}",
                        detail::pop_ssl_error("SSL_set_fd"));
            SSL_free(ssl);
            ::close(cfd);
            return {};
        }
        SSL_set_accept_state(ssl);

        ERR_clear_error();
        const int r = SSL_accept(ssl);
        if (r != 1) {
            const int err = SSL_get_error(ssl, r);
            SPDLOG_WARN("[tls_server] SSL_accept failed err={} detail={}",
                        err, detail::pop_ssl_error("SSL_accept"));
            SSL_free(ssl);
            ::close(cfd);
            return {};
        }

        // Null-guard the cipher/version strings before formatting.
        // aws-lc returns non-null after a successful SSL_accept (r==1),
        // but a stub / mock TLS implementation that returns null would
        // crash this INFO log via `fmt`. Mirrors the client-side guard
        // at eph-net/include/eph/net/detail/tls_session.hpp.
        const char* cipher  = SSL_get_cipher_name(ssl);
        const char* version = SSL_get_version(ssl);
        SPDLOG_INFO("[tls_server] handshake OK fd={} cipher={} version={}",
                    cfd,
                    cipher  ? cipher  : "(unknown)",
                    version ? version : "(unknown)");
        return TlsConn{ssl, cfd};
    }

private:
    explicit TlsServer(SSL_CTX* ctx) noexcept : ctx_(ctx) {}
    SSL_CTX* ctx_ = nullptr;
};

} // namespace mockex::tls
