#pragma once

/// @file tls_ws_echo_server.hpp
/// In-process TLS 1.3 + WebSocket echo server for transport e2e tests.
///
/// Spawns a single accept thread on construction (after `start()`),
/// terminates self-signed ECDSA P-256 certificates in memory, runs the
/// aws-lc TLS handshake, parses the HTTP Upgrade request, sends an
/// RFC 6455 §4.2.2 conformant 101 response, then echoes WebSocket frames
/// back until the client closes (or `kill_active_sessions()` forces a
/// disconnect for reconnect tests).
///
/// **Why in-process**: zero external dependencies (no Python websockets
/// pip install, no `openssl s_server` subprocess, no fixture cert files
/// in the repo). Each test owns its own server instance with an
/// ephemeral port and ephemeral keypair, so tests can run in parallel.
///
/// **Why this fixture exists**: prior to commit 4eab3fb, the codebase
/// had no test that exercised `Transport<...> + use_tls=true + WsFramer`
/// end-to-end. The TLS hot-path AEAD ordering bug
/// (`.artifacts/fix-tls-ordering-symptoms-20260409.txt`) was therefore
/// latent for ~7 days. This fixture is the foundation of the regression
/// test that catches that class of bug.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace eph::test {

/// In-process TLS 1.3 + WebSocket echo server.
///
/// Lifecycle: construct -> start() -> port() -> ... -> stop() (or destruct).
class TlsWsEchoServer {
public:
    TlsWsEchoServer() {
        generate_self_signed_cert_();
        build_ssl_ctx_();
        listen_ephemeral_port_();
    }

    ~TlsWsEchoServer() {
        stop();
        if (ssl_ctx_) SSL_CTX_free(ssl_ctx_);
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    TlsWsEchoServer(const TlsWsEchoServer&) = delete;
    TlsWsEchoServer& operator=(const TlsWsEchoServer&) = delete;

    /// Launch the accept thread. Must be called once before clients connect.
    void start() {
        if (running_.exchange(true)) return;
        accept_thread_ = std::thread([this] { accept_loop_(); });
    }

    /// Stop accepting new connections, close all active sessions, join
    /// the accept thread. Idempotent.
    void stop() noexcept {
        if (!running_.exchange(false)) return;

        // Shutdown the listen socket so accept() returns.
        if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);

        if (accept_thread_.joinable()) accept_thread_.join();

        kill_active_sessions();
    }

    /// Forcibly close all active client sessions (for reconnect tests).
    /// Does not stop accepting new ones.
    void kill_active_sessions() noexcept {
        std::lock_guard lk(sessions_mu_);
        for (int fd : active_session_fds_) {
            ::shutdown(fd, SHUT_RDWR);
        }
        active_session_fds_.clear();
    }

    /// Bound port (only valid after construction).
    [[nodiscard]] uint16_t port() const noexcept { return port_; }

private:
    // ─── Cert generation ────────────────────────────────────────────────────

    void generate_self_signed_cert_() {
        // ECDSA P-256 keypair, in-memory, ephemeral.
        EVP_PKEY* pkey = nullptr;
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        if (!pctx ||
            EVP_PKEY_keygen_init(pctx) <= 0 ||
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0 ||
            EVP_PKEY_keygen(pctx, &pkey) <= 0) {
            EVP_PKEY_CTX_free(pctx);
            throw std::runtime_error("ECDSA keygen failed");
        }
        EVP_PKEY_CTX_free(pctx);

        X509* x = X509_new();
        if (!x) {
            EVP_PKEY_free(pkey);
            throw std::runtime_error("X509_new failed");
        }

        X509_set_version(x, 2);  // v3
        ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
        X509_gmtime_adj(X509_get_notBefore(x), -3600);
        X509_gmtime_adj(X509_get_notAfter(x), 24 * 3600);
        X509_set_pubkey(x, pkey);

        X509_NAME* name = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("eph-test-server"), -1, -1, 0);
        X509_set_issuer_name(x, name);

        if (!X509_sign(x, pkey, EVP_sha256())) {
            X509_free(x);
            EVP_PKEY_free(pkey);
            throw std::runtime_error("X509_sign failed");
        }

        cert_ = x;
        pkey_ = pkey;
    }

    // ─── SSL_CTX setup ──────────────────────────────────────────────────────

    void build_ssl_ctx_() {
        ssl_ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ssl_ctx_) throw std::runtime_error("SSL_CTX_new failed");

        SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_3_VERSION);

        if (SSL_CTX_use_certificate(ssl_ctx_, cert_) <= 0) {
            throw std::runtime_error("SSL_CTX_use_certificate failed");
        }
        if (SSL_CTX_use_PrivateKey(ssl_ctx_, pkey_) <= 0) {
            throw std::runtime_error("SSL_CTX_use_PrivateKey failed");
        }
        // Cert and key are now owned by the SSL_CTX; drop our refs.
        X509_free(cert_); cert_ = nullptr;
        EVP_PKEY_free(pkey_); pkey_ = nullptr;

        // Enable server-side session ticket issuance for TLS 1.3
        // resumption tests. aws-lc/BoringSSL require an explicit
        // session_id_context to identify ticket-issuing servers; the
        // value is opaque and only used to disambiguate cached
        // sessions across different server roles. A 16-byte literal
        // is sufficient for in-process tests.
        static const unsigned char kSidCtx[] = "eph-test-tls-sidctx";
        SSL_CTX_set_session_id_context(
            ssl_ctx_, kSidCtx, sizeof(kSidCtx) - 1);
        SSL_CTX_set_session_cache_mode(ssl_ctx_, SSL_SESS_CACHE_SERVER);
    }

    // ─── Listener ───────────────────────────────────────────────────────────

    void listen_ephemeral_port_() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");

        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;  // ephemeral

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error("bind() failed");
        }
        if (::listen(listen_fd_, 8) < 0) {
            throw std::runtime_error("listen() failed");
        }

        socklen_t alen = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &alen) < 0) {
            throw std::runtime_error("getsockname() failed");
        }
        port_ = ntohs(addr.sin_port);
    }

    // ─── Accept loop ────────────────────────────────────────────────────────

    void accept_loop_() {
        while (running_.load(std::memory_order_acquire)) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int fd = ::accept(listen_fd_,
                              reinterpret_cast<sockaddr*>(&peer), &plen);
            if (fd < 0) {
                if (!running_.load(std::memory_order_acquire)) break;
                continue;
            }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

            {
                std::lock_guard lk(sessions_mu_);
                active_session_fds_.push_back(fd);
            }
            // Detach a per-connection handler thread so the accept loop
            // can keep accepting new connections (needed for reconnect
            // tests where multiple sessions arrive serially).
            std::thread([this, fd] { handle_session_(fd); }).detach();
        }
    }

    void handle_session_(int fd) {
        SSL* ssl = SSL_new(ssl_ctx_);
        if (!ssl) { ::close(fd); return; }
        SSL_set_fd(ssl, fd);

        if (SSL_accept(ssl) <= 0) {
            SSL_free(ssl);
            ::close(fd);
            return;
        }

        // ── Read HTTP Upgrade request ──────────────────────────────
        std::string req;
        req.reserve(2048);
        char buf[1024];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
            int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) { teardown_(ssl, fd); return; }
            req.append(buf, static_cast<size_t>(n));
        }

        // Extract Sec-WebSocket-Key
        std::string ws_key = extract_ws_key_(req);
        if (ws_key.empty()) { teardown_(ssl, fd); return; }
        std::string accept_val = compute_ws_accept_(ws_key);

        // ── Send 101 Switching Protocols ───────────────────────────
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept_val + "\r\n"
            "\r\n";
        if (SSL_write(ssl, resp.data(), static_cast<int>(resp.size())) <= 0) {
            teardown_(ssl, fd);
            return;
        }

        // ── Echo loop ──────────────────────────────────────────────
        // From this point on, every record on the wire is a hot-path
        // application record. If the client's hot-path crypto seq is
        // out of sync with SSL's, SSL_read here will fail to decrypt.
        echo_loop_(ssl, fd);

        teardown_(ssl, fd);
    }

    // Minimal WebSocket frame echo. Supports unfragmented client→server
    // frames with payloads up to 65535 bytes (extended length 16-bit).
    void echo_loop_(SSL* ssl, int fd) {
        uint8_t hdr[2];
        while (running_.load(std::memory_order_acquire)) {
            // Read 2-byte minimal header
            int n = SSL_read(ssl, hdr, 2);
            if (n != 2) return;

            uint8_t b0 = hdr[0];
            uint8_t b1 = hdr[1];
            uint8_t opcode = b0 & 0x0F;
            bool fin = (b0 & 0x80) != 0;
            bool masked = (b1 & 0x80) != 0;
            uint64_t plen = b1 & 0x7F;

            // Extended length
            if (plen == 126) {
                uint8_t ext[2];
                if (SSL_read(ssl, ext, 2) != 2) return;
                plen = (uint64_t(ext[0]) << 8) | ext[1];
            } else if (plen == 127) {
                uint8_t ext[8];
                if (SSL_read(ssl, ext, 8) != 8) return;
                plen = 0;
                for (int i = 0; i < 8; ++i) plen = (plen << 8) | ext[i];
            }

            uint8_t mask_key[4]{};
            if (masked) {
                if (SSL_read(ssl, mask_key, 4) != 4) return;
            }

            std::vector<uint8_t> payload(plen);
            size_t got = 0;
            while (got < plen) {
                int rd = SSL_read(ssl, payload.data() + got,
                                  static_cast<int>(plen - got));
                if (rd <= 0) return;
                got += static_cast<size_t>(rd);
            }
            if (masked) {
                for (size_t i = 0; i < plen; ++i)
                    payload[i] ^= mask_key[i % 4];
            }

            // Close frame: send close back and exit
            if (opcode == 0x8) {
                uint8_t close_resp[2] = { 0x88, 0x00 };
                SSL_write(ssl, close_resp, 2);
                return;
            }
            // Ping → Pong
            if (opcode == 0x9) {
                std::vector<uint8_t> pong;
                pong.push_back(0x80 | 0x0A);
                pong.push_back(static_cast<uint8_t>(plen));
                pong.insert(pong.end(), payload.begin(), payload.end());
                SSL_write(ssl, pong.data(), static_cast<int>(pong.size()));
                continue;
            }

            // Text/Binary echo: send back unmasked frame with same opcode
            std::vector<uint8_t> out;
            out.reserve(plen + 14);
            out.push_back(static_cast<uint8_t>((fin ? 0x80 : 0) | opcode));
            if (plen <= 125) {
                out.push_back(static_cast<uint8_t>(plen));
            } else if (plen <= 65535) {
                out.push_back(126);
                out.push_back(static_cast<uint8_t>(plen >> 8));
                out.push_back(static_cast<uint8_t>(plen & 0xFF));
            } else {
                out.push_back(127);
                for (int i = 7; i >= 0; --i)
                    out.push_back(static_cast<uint8_t>((plen >> (8 * i)) & 0xFF));
            }
            out.insert(out.end(), payload.begin(), payload.end());
            if (SSL_write(ssl, out.data(), static_cast<int>(out.size())) <= 0)
                return;
        }
    }

    void teardown_(SSL* ssl, int fd) noexcept {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        ::close(fd);
        std::lock_guard lk(sessions_mu_);
        for (auto it = active_session_fds_.begin();
             it != active_session_fds_.end(); ++it) {
            if (*it == fd) { active_session_fds_.erase(it); break; }
        }
    }

    // ─── HTTP / WS helpers ──────────────────────────────────────────────────

    static std::string extract_ws_key_(std::string_view request) {
        constexpr std::string_view kHdr = "Sec-WebSocket-Key:";
        // Case-insensitive scan (clients always use the canonical case but
        // be lenient).
        for (size_t i = 0; i + kHdr.size() <= request.size(); ++i) {
            bool m = true;
            for (size_t j = 0; j < kHdr.size(); ++j) {
                char a = request[i + j];
                char b = kHdr[j];
                auto lo = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
                if (lo(a) != lo(b)) { m = false; break; }
            }
            if (!m) continue;
            size_t v = i + kHdr.size();
            while (v < request.size() && (request[v] == ' ' || request[v] == '\t')) ++v;
            size_t end = request.find("\r\n", v);
            if (end == std::string::npos) return {};
            return std::string(request.substr(v, end - v));
        }
        return {};
    }

    static std::string compute_ws_accept_(const std::string& key) {
        static constexpr std::string_view kMagic =
            "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string concat = key + std::string(kMagic);

        uint8_t digest[20];
        unsigned int dlen = 0;
        EVP_Digest(concat.data(), concat.size(), digest, &dlen,
                   EVP_sha1(), nullptr);

        // Base64 encode (BIO chain)
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO* mem = BIO_new(BIO_s_mem());
        BIO_push(b64, mem);
        BIO_write(b64, digest, static_cast<int>(dlen));
        BIO_flush(b64);
        char* out_buf = nullptr;
        long out_len = BIO_get_mem_data(mem, &out_buf);
        std::string out(out_buf, static_cast<size_t>(out_len));
        BIO_free_all(b64);
        return out;
    }

    // ─── State ──────────────────────────────────────────────────────────────

    SSL_CTX* ssl_ctx_{nullptr};
    X509*    cert_{nullptr};   // released after build_ssl_ctx_
    EVP_PKEY* pkey_{nullptr};  // released after build_ssl_ctx_

    int  listen_fd_{-1};
    uint16_t port_{0};

    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    std::mutex sessions_mu_;
    std::vector<int> active_session_fds_;
};

} // namespace eph::test
