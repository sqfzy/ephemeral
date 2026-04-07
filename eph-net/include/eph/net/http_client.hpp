#pragma once

/// @file http_client.hpp
/// Minimal synchronous HTTP/1.1 client for REST API calls.
///
/// Uses POSIX sockets + aws-lc TLS (OpenSSL-compatible SSL_* API).
/// NOT for hot-path use — this is for initialization, snapshotting,
/// and order management (latency ~1-10ms).
///
/// Design: one request per connection (no keep-alive, no chunked encoding,
/// no redirects). Intentionally minimal for crypto exchange REST APIs
/// (order placement, balance queries, orderbook snapshots).
///
/// For the pure HTTP message types and parsers (no network dependency),
/// see http_message.hpp.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <future>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "eph/core/detail/json_escape.hpp"
#include "eph/core/detail/string_checks.hpp"
#include "eph/net/http_message.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// SSL error helper
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @brief Retrieve the most recent OpenSSL error as a human-readable string.
/// @return Error description from the OpenSSL error queue.
inline std::string ssl_last_error() {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return std::string(buf);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// HttpClient — synchronous HTTP/1.1 over POSIX sockets + TLS
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Minimal synchronous HTTP/1.1 client for REST API calls.
///
/// Each request opens a new TCP connection, optionally performs TLS handshake,
/// sends the request, reads the full response, and closes. No keep-alive,
/// no pipelining, no chunked encoding, no redirects.
///
/// Designed for crypto exchange REST APIs: order placement, balance queries,
/// orderbook snapshots -- all off the hot path.
///
/// @warning NOT suitable for latency-sensitive hot paths. Typical latency is 1-10ms.
class HttpClient {
public:
    /// @brief Configuration for the HTTP client.
    struct Config {
        std::string                 host;             ///< Target hostname (e.g., "api.binance.com").
        uint16_t                    port = 443;       ///< Target port (default 443 for HTTPS).
        bool                        use_tls = true;   ///< Enable TLS (default true).
        std::chrono::milliseconds   timeout{5000};    ///< Timeout for DNS, connect, send, and recv.
        std::string                 ca_cert_path{};   ///< CA certificate path; empty = system default.
        size_t                      max_response_size = 8 * 1024 * 1024;  ///< Max response body size (8 MiB default).

        /// @brief Validate configuration, returning an error description or empty string on success.
        /// Call before constructing an HttpClient for early, actionable error messages.
        [[nodiscard]] constexpr std::string_view validate() const noexcept {
            if (host.empty())
                return "host must not be empty";
            if (eph::core::detail::contains_control_chars(host))
                return "host contains control characters (header injection risk)";
            if (port == 0)
                return "port must be > 0";
            if (timeout.count() <= 0)
                return "timeout must be positive";
            if (max_response_size == 0)
                return "max_response_size must be > 0";
            return {};
        }

        /// @brief Format configuration as a human-readable string for logging.
        /// @return Single-line description of all config fields.
        [[nodiscard]] std::string dump() const {
            return std::format(
                "HttpClient::Config(host='{}', port={}, tls={}, timeout={}ms, "
                "max_response={}B, ca='{}')",
                host, port, use_tls, timeout.count(),
                max_response_size,
                ca_cert_path.empty() ? "(system)" : ca_cert_path);
        }

        /// @brief JSON-formatted config for monitoring system integration.
        ///
        /// String fields are escaped per RFC 8259 section 7 to prevent
        /// malformed output from hostnames or paths containing special characters.
        [[nodiscard]] std::string to_json() const {
            return std::format(
                "{{\"host\":\"{}\",\"port\":{},\"use_tls\":{},\"timeout_ms\":{},"
                "\"max_response_size\":{},\"ca_cert_path\":\"{}\"}}",
                eph::core::detail::json_escape(host), port,
                use_tls ? "true" : "false", timeout.count(),
                max_response_size,
                ca_cert_path.empty() ? std::string{}
                    : eph::core::detail::json_escape(ca_cert_path));
        }

        /// @brief Parse an HTTP(S) URL into an HttpClient::Config.
        ///
        /// Supported formats:
        ///   https://host            (port defaults to 443, use_tls=true)
        ///   https://host:port       (explicit port, use_tls=true)
        ///   http://host             (port defaults to 80, use_tls=false)
        ///   http://host:port        (explicit port, use_tls=false)
        ///
        /// Only sets host, port, and use_tls. All other fields retain defaults.
        /// Path component (if any) is ignored -- pass it to get()/post() instead.
        ///
        /// @param url  HTTP(S) URL string
        /// @return Config on success, or error description
        [[nodiscard]] static std::expected<Config, std::string>
        from_url(std::string_view url) {
            Config cfg;

            // Determine scheme
            if (url.starts_with("https://")) {
                cfg.use_tls = true;
                cfg.port = 443;
                url.remove_prefix(8);
            } else if (url.starts_with("http://")) {
                cfg.use_tls = false;
                cfg.port = 80;
                url.remove_prefix(7);
            } else {
                return std::unexpected(std::format(
                    "unsupported scheme (expected http:// or https://): {}", url));
            }

            // Strip path/query/fragment -- only host[:port] is relevant
            auto path_pos = url.find('/');
            if (path_pos != std::string_view::npos) {
                url = url.substr(0, path_pos);
            }

            if (url.empty()) {
                return std::unexpected(std::string("URL has empty host"));
            }

            // IPv6 bracket notation: [::1]:port
            if (url.front() == '[') {
                auto bracket_end = url.find(']');
                if (bracket_end == std::string_view::npos) {
                    return std::unexpected(std::string("IPv6 address missing closing ']'"));
                }
                cfg.host = std::string(url.substr(1, bracket_end - 1));
                if (cfg.host.empty()) {
                    return std::unexpected(std::string("URL has empty IPv6 address"));
                }
                url.remove_prefix(bracket_end + 1);
                // Optional :port after bracket
                if (!url.empty() && url.front() == ':') {
                    url.remove_prefix(1);
                }
            } else {
                // Regular hostname -- check for optional :port
                auto colon = url.rfind(':');
                if (colon != std::string_view::npos) {
                    cfg.host = std::string(url.substr(0, colon));
                    url.remove_prefix(colon + 1);
                } else {
                    cfg.host = std::string(url);
                    url = {};
                }
            }

            if (cfg.host.empty()) {
                return std::unexpected(std::string("URL has empty host"));
            }

            // Reject control characters in hostname
            if (eph::core::detail::contains_control_chars(cfg.host)) {
                return std::unexpected(std::string("hostname contains control characters"));
            }

            // Parse optional port
            if (!url.empty()) {
                uint32_t port_val = 0;
                auto [ptr, ec] = std::from_chars(
                    url.data(), url.data() + url.size(), port_val);
                if (ec != std::errc{} || ptr != url.data() + url.size()) {
                    return std::unexpected(std::format("invalid port: '{}'", url));
                }
                if (port_val == 0 || port_val > 65535) {
                    return std::unexpected(std::format("port out of range: {}", port_val));
                }
                cfg.port = static_cast<uint16_t>(port_val);
            }

            return cfg;
        }

        /// @brief Serialize as an HTTP(S) URL string (inverse of from_url).
        ///
        /// Format: scheme://host[:port]
        /// Port is included only when it differs from the scheme default
        /// (443 for HTTPS, 80 for HTTP).
        ///
        /// @return URL string like "https://api.binance.com" or "http://localhost:8080"
        [[nodiscard]] std::string to_url() const {
            std::string_view scheme = use_tls ? "https" : "http";
            uint16_t default_port = use_tls ? 443 : 80;
            bool is_ipv6 = host.find(':') != std::string::npos;
            if (port == default_port) {
                if (is_ipv6) return std::format("{}://[{}]", scheme, host);
                return std::format("{}://{}", scheme, host);
            }
            if (is_ipv6) return std::format("{}://[{}]:{}", scheme, host, port);
            return std::format("{}://{}:{}", scheme, host, port);
        }

        /// Defaulted equality -- all fields must match exactly.
        [[nodiscard]] friend bool operator==(const Config&,
                                             const Config&) = default;

        /// Check for non-fatal contradictions or likely misconfigurations.
        /// Returns a list of warning messages (empty if no issues).
        [[nodiscard]] std::vector<std::string> warnings() const {
            std::vector<std::string> w;
            if (!use_tls)
                w.emplace_back("use_tls=false — HTTP requests will be sent "
                               "in plaintext (credentials may be exposed)");
            if (timeout.count() < 1000)
                w.emplace_back(std::format(
                    "timeout={}ms is short — DNS + TLS handshake alone "
                    "may exceed this on slow networks", timeout.count()));
            if (max_response_size > 64 * 1024 * 1024)
                w.emplace_back(std::format(
                    "max_response_size={}B (>64MB) — may consume excessive "
                    "memory for a single response", max_response_size));
            return w;
        }
    };

    /// @brief Construct an HttpClient with the given configuration.
    /// @param config  Client configuration (host, port, TLS, timeout, etc.).
    explicit HttpClient(Config config) : config_(std::move(config)) {
        SPDLOG_LOGGER_DEBUG(detail::http_client_logger(),
            "HttpClient created: {}", config_.dump());
    }

    /// @brief Send a GET request.
    /// @param path           Request URI (e.g. "/api/v1/ticker").
    /// @param extra_headers  Additional headers (\r\n terminated per line).
    /// @return Parsed HttpResponse or error string.
    [[nodiscard]] std::expected<HttpResponse, std::string>
    get(std::string_view path,
        std::string_view extra_headers = {}) noexcept {
        SPDLOG_LOGGER_DEBUG(detail::http_client_logger(),
            "GET {}:{}{}", config_.host, config_.port, path);

        auto req = build_http_request("GET", config_.host, path,
                                       /*body=*/{}, /*content_type=*/{},
                                       extra_headers);
        if (!req) return std::unexpected(req.error());
        return execute(*req);
    }

    /// @brief Send a POST request with a body.
    /// @param path           Request URI (e.g. "/api/v1/order").
    /// @param body           Request body.
    /// @param content_type   Content-Type header (default: "application/json").
    /// @param extra_headers  Additional headers (\r\n terminated per line).
    /// @return Parsed HttpResponse or error string.
    [[nodiscard]] std::expected<HttpResponse, std::string>
    post(std::string_view path,
         std::string_view body,
         std::string_view content_type = "application/json",
         std::string_view extra_headers = {}) noexcept {
        SPDLOG_LOGGER_DEBUG(detail::http_client_logger(),
            "POST {}:{}{} ({} bytes body)", config_.host, config_.port,
            path, body.size());

        auto req = build_http_request("POST", config_.host, path,
                                       body, content_type, extra_headers);
        if (!req) return std::unexpected(req.error());
        return execute(*req);
    }

    /// @brief Send a PUT request with a body.
    /// @param path           Request URI (e.g. "/api/v1/order").
    /// @param body           Request body.
    /// @param content_type   Content-Type header (default: "application/json").
    /// @param extra_headers  Additional headers (\r\n terminated per line).
    /// @return Parsed HttpResponse or error string.
    [[nodiscard]] std::expected<HttpResponse, std::string>
    put(std::string_view path,
        std::string_view body,
        std::string_view content_type = "application/json",
        std::string_view extra_headers = {}) noexcept {
        SPDLOG_LOGGER_DEBUG(detail::http_client_logger(),
            "PUT {}:{}{} ({} bytes body)", config_.host, config_.port,
            path, body.size());

        auto req = build_http_request("PUT", config_.host, path,
                                       body, content_type, extra_headers);
        if (!req) return std::unexpected(req.error());
        return execute(*req);
    }

    /// @brief Send a DELETE request.
    ///
    /// Named delete_request to avoid conflict with the C++ keyword `delete`.
    ///
    /// @param path           Request URI (e.g. "/api/v1/order?orderId=123").
    /// @param extra_headers  Additional headers (\r\n terminated per line).
    /// @return Parsed HttpResponse or error string.
    [[nodiscard]] std::expected<HttpResponse, std::string>
    delete_request(std::string_view path,
                   std::string_view extra_headers = {}) noexcept {
        SPDLOG_LOGGER_DEBUG(detail::http_client_logger(),
            "DELETE {}:{}{}", config_.host, config_.port, path);

        auto req = build_http_request("DELETE", config_.host, path,
                                       /*body=*/{}, /*content_type=*/{},
                                       extra_headers);
        if (!req) return std::unexpected(req.error());
        return execute(*req);
    }

    /// @brief Send a generic HTTP request with any method.
    ///
    /// Use this for PATCH, OPTIONS, HEAD, or custom methods not covered
    /// by the convenience methods above.
    ///
    /// @param method         HTTP method string (e.g. "PATCH", "HEAD").
    /// @param path           Request URI.
    /// @param body           Request body (empty for bodyless methods).
    /// @param content_type   Content-Type header (only added if body is non-empty).
    /// @param extra_headers  Additional headers (\r\n terminated per line).
    /// @return Parsed HttpResponse or error string.
    [[nodiscard]] std::expected<HttpResponse, std::string>
    request(std::string_view method,
            std::string_view path,
            std::string_view body = {},
            std::string_view content_type = {},
            std::string_view extra_headers = {}) noexcept {
        SPDLOG_LOGGER_DEBUG(detail::http_client_logger(),
            "{} {}:{}{} ({} bytes body)", method, config_.host, config_.port,
            path, body.size());

        auto req = build_http_request(method, config_.host, path,
                                       body, content_type, extra_headers);
        if (!req) return std::unexpected(req.error());
        return execute(*req);
    }

    /// @brief Access the current configuration (for logging/debugging).
    /// @return Const reference to the HttpClient::Config.
    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    Config config_;

    // ─────────────────────────────────────────────────────────────────────────
    // RAII wrappers for socket and SSL resources
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief RAII deleter for SSL* -- performs TLS shutdown then frees.
    struct SslDeleter {
        void operator()(SSL* s) const noexcept {
            if (s) {
                SSL_shutdown(s);
                SSL_free(s);
            }
        }
    };

    /// @brief RAII deleter for SSL_CTX*.
    struct SslCtxDeleter {
        void operator()(SSL_CTX* c) const noexcept {
            if (c) SSL_CTX_free(c);
        }
    };

    /// @brief RAII socket fd wrapper.
    ///
    /// Closes the file descriptor on destruction. Move-only.
    struct Socket {
        int fd = -1;
        Socket() = default;
        explicit Socket(int f) : fd(f) {}
        ~Socket() { if (fd >= 0) ::close(fd); }
        Socket(const Socket&) = delete;
        Socket& operator=(const Socket&) = delete;
        Socket(Socket&& o) noexcept : fd(o.fd) { o.fd = -1; }
        Socket& operator=(Socket&& o) noexcept {
            if (this != &o) { if (fd >= 0) ::close(fd); fd = o.fd; o.fd = -1; }
            return *this;
        }
        [[nodiscard]] bool valid() const noexcept { return fd >= 0; }
    };

    // ─────────────────────────────────────────────────────────────────────────
    // TCP connect
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief Resolve host and connect a TCP socket with timeout.
    /// @return Connected Socket wrapper, or error string on DNS/connect failure.
    [[nodiscard]] std::expected<Socket, std::string>
    tcp_connect() noexcept {
        [[maybe_unused]] auto* log = detail::http_client_logger();

        SPDLOG_LOGGER_DEBUG(log, "Resolving {}:{}", config_.host, config_.port);

        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_ADDRCONFIG;  // Only return addresses the host can reach

        auto port_str = std::to_string(config_.port);

        // Run getaddrinfo asynchronously with a timeout — blocking DNS
        // resolution can hang indefinitely if the nameserver is unreachable.
        // Capture host/port by value to avoid dangling references if we
        // timeout and return before the async thread completes. Use
        // shared_ptr so the async thread frees addrinfo even if we abandon it.
        auto dns_host = config_.host;
        auto dns_port = port_str;
        auto dns_future = std::async(std::launch::async,
            [dns_host, dns_port, hints]() mutable
                -> std::expected<std::shared_ptr<struct addrinfo>, std::string> {
                struct addrinfo* res = nullptr;
                int rc = ::getaddrinfo(dns_host.c_str(), dns_port.c_str(),
                                       &hints, &res);
                if (rc != 0) {
                    return std::unexpected(std::format(
                        "DNS resolution failed: {}", gai_strerror(rc)));
                }
                return std::shared_ptr<struct addrinfo>(res, ::freeaddrinfo);
            });
        auto dns_status = dns_future.wait_for(config_.timeout);
        if (dns_status != std::future_status::ready) {
            SPDLOG_LOGGER_ERROR(log, "DNS resolution timeout for {}:{} ({}ms)",
                config_.host, config_.port, config_.timeout.count());
            // The async thread holds a shared_ptr that will freeaddrinfo()
            // when getaddrinfo eventually returns. No leak, no dangling ref.
            return std::unexpected(std::format(
                "DNS resolution timeout for {}:{} ({}ms)",
                config_.host, config_.port, config_.timeout.count()));
        }
        auto dns_result = dns_future.get();
        if (!dns_result) {
            SPDLOG_LOGGER_ERROR(log, "getaddrinfo failed for {}:{}: {}",
                config_.host, config_.port, dns_result.error());
            return std::unexpected(std::format(
                "DNS resolution failed for {}:{}: {}",
                config_.host, config_.port, dns_result.error()));
        }
        auto result_ptr = *dns_result;

        // Try each resolved address
        for (auto* rp = result_ptr.get(); rp != nullptr; rp = rp->ai_next) {
            int fd = ::socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK,
                              rp->ai_protocol);
            if (fd < 0) continue;

            Socket sock(fd);

            int ret = ::connect(fd, rp->ai_addr, rp->ai_addrlen);
            if (ret == 0) {
                SPDLOG_LOGGER_DEBUG(log, "Connected to {}:{} (immediate)",
                    config_.host, config_.port);
                return sock;
            }

            if (errno != EINPROGRESS) {
                SPDLOG_LOGGER_TRACE(log,
                    "connect() failed for one address: {}", strerror(errno));
                continue;
            }

            // Wait for connection with poll()
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int poll_ret = ::poll(&pfd, 1,
                static_cast<int>(config_.timeout.count()));

            if (poll_ret <= 0) {
                SPDLOG_LOGGER_TRACE(log,
                    "connect() poll timeout/error for one address");
                continue;
            }

            // Check for connection error
            int so_err = 0;
            socklen_t so_len = sizeof(so_err);
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
            if (so_err != 0) {
                SPDLOG_LOGGER_TRACE(log,
                    "connect() completed with error: {}", strerror(so_err));
                continue;
            }

            SPDLOG_LOGGER_DEBUG(log, "Connected to {}:{}",
                config_.host, config_.port);
            return sock;
        }

        SPDLOG_LOGGER_ERROR(log, "Failed to connect to {}:{}",
            config_.host, config_.port);
        return std::unexpected(std::format(
            "Failed to connect to {}:{}", config_.host, config_.port));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // TLS handshake
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief Perform TLS handshake over an established TCP socket.
    ///
    /// Returns (SSL_CTX*, SSL*) wrapped in unique_ptrs for RAII cleanup.
    ///
    /// @param fd  Connected socket file descriptor.
    /// @return Pair of (SSL_CTX, SSL) RAII wrappers, or error string.
    [[nodiscard]] std::expected<
        std::pair<std::unique_ptr<SSL_CTX, SslCtxDeleter>,
                  std::unique_ptr<SSL, SslDeleter>>,
        std::string>
    tls_handshake(int fd) noexcept {
        [[maybe_unused]] auto* log = detail::http_client_logger();

        SPDLOG_LOGGER_DEBUG(log, "Starting TLS handshake with {}",
            config_.host);

        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            auto err = detail::ssl_last_error();
            SPDLOG_LOGGER_ERROR(log, "SSL_CTX_new failed: {}", err);
            return std::unexpected(std::format("SSL_CTX_new failed: {}", err));
        }
        std::unique_ptr<SSL_CTX, SslCtxDeleter> ctx_ptr(ctx);

        // Load CA certificates
        if (!config_.ca_cert_path.empty()) {
            if (!SSL_CTX_load_verify_locations(ctx,
                    config_.ca_cert_path.c_str(), nullptr)) {
                auto err = detail::ssl_last_error();
                SPDLOG_LOGGER_ERROR(log,
                    "Failed to load CA cert '{}': {}",
                    config_.ca_cert_path, err);
                return std::unexpected(std::format(
                    "Failed to load CA cert: {}", err));
            }
        } else {
            SSL_CTX_set_default_verify_paths(ctx);
        }

        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            auto err = detail::ssl_last_error();
            SPDLOG_LOGGER_ERROR(log, "SSL_new failed: {}", err);
            return std::unexpected(std::format("SSL_new failed: {}", err));
        }
        std::unique_ptr<SSL, SslDeleter> ssl_ptr(ssl);

        // Set SNI hostname and enable hostname verification
        if (!config_.host.empty()) {
            SSL_set_tlsext_host_name(ssl, config_.host.c_str());
            // Explicitly enable hostname verification — SNI alone only tells
            // the server which cert to present, it does not make the client
            // verify the certificate matches the target hostname.
            SSL_set1_host(ssl, config_.host.c_str());
        }

        // Attach fd to SSL
        if (!SSL_set_fd(ssl, fd)) {
            auto err = detail::ssl_last_error();
            SPDLOG_LOGGER_ERROR(log, "SSL_set_fd failed: {}", err);
            return std::unexpected(std::format("SSL_set_fd failed: {}", err));
        }

        // Perform handshake (may require multiple iterations for non-blocking)
        auto deadline = std::chrono::steady_clock::now() + config_.timeout;
        while (true) {
            ERR_clear_error();
            int ret = SSL_connect(ssl);
            if (ret == 1) {
                SPDLOG_LOGGER_DEBUG(log,
                    "TLS handshake complete: version={}, cipher={}",
                    SSL_get_version(ssl),
                    SSL_CIPHER_get_name(SSL_get_current_cipher(ssl)));
                return std::make_pair(std::move(ctx_ptr), std::move(ssl_ptr));
            }

            int err = SSL_get_error(ssl, ret);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    SPDLOG_LOGGER_ERROR(log,
                        "TLS handshake timeout ({}ms)", config_.timeout.count());
                    return std::unexpected("TLS handshake timeout");
                }

                // Wait for the socket to become ready
                struct pollfd pfd{};
                pfd.fd = fd;
                pfd.events = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                int poll_rc = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
                if (poll_rc < 0 && errno != EINTR) {
                    auto err_str = strerror(errno);
                    SPDLOG_LOGGER_ERROR(log, "TLS handshake poll() failed: {}", err_str);
                    return std::unexpected(std::format("TLS handshake poll failed: {}", err_str));
                }
                continue;
            }

            auto ssl_err = detail::ssl_last_error();
            SPDLOG_LOGGER_ERROR(log,
                "TLS handshake failed: ssl_error={}, detail={}", err, ssl_err);
            return std::unexpected(std::format(
                "TLS handshake failed (err={}): {}", err, ssl_err));
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // I/O helpers with timeout
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief Send all data through a plain socket.
    /// @param fd    Socket file descriptor.
    /// @param data  Data to send.
    /// @return void on success, error string on timeout or I/O failure.
    [[nodiscard]] std::expected<void, std::string>
    send_all(int fd, std::string_view data) noexcept {
        auto deadline = std::chrono::steady_clock::now() + config_.timeout;
        size_t sent = 0;
        while (sent < data.size()) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                return std::unexpected(std::string("Send timeout"));
            }

            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            int poll_ret = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
            if (poll_ret <= 0) {
                return std::unexpected(std::string("Send poll timeout"));
            }

            ssize_t n = ::send(fd, data.data() + sent, data.size() - sent,
                               MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return std::unexpected(std::format("send() failed: {}",
                    strerror(errno)));
            }
            sent += static_cast<size_t>(n);
        }
        return {};
    }

    /// @brief Send all data through TLS.
    /// @param ssl   SSL connection handle.
    /// @param fd    Underlying socket file descriptor (for poll).
    /// @param data  Data to send.
    /// @return void on success, error string on timeout or SSL failure.
    [[nodiscard]] std::expected<void, std::string>
    ssl_send_all(SSL* ssl, int fd, std::string_view data) noexcept {
        auto deadline = std::chrono::steady_clock::now() + config_.timeout;
        size_t sent = 0;
        while (sent < data.size()) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                return std::unexpected(std::string("SSL send timeout"));
            }

            ERR_clear_error();
            int n = SSL_write(ssl, data.data() + sent,
                              static_cast<int>(data.size() - sent));
            if (n > 0) {
                sent += static_cast<size_t>(n);
                continue;
            }

            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                struct pollfd pfd{};
                pfd.fd = fd;
                pfd.events = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                int prc = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
                if (prc < 0 && errno != EINTR) {
                    return std::unexpected(std::format(
                        "SSL send poll failed: {}", strerror(errno)));
                }
                continue;
            }

            return std::unexpected(std::format("SSL_write failed (err={}): {}",
                err, detail::ssl_last_error()));
        }
        return {};
    }

    /// @brief Read all response data (until connection close or Content-Length satisfied).
    /// @param fd  Socket file descriptor.
    /// @return Complete raw HTTP response string, or error string.
    [[nodiscard]] std::expected<std::string, std::string>
    recv_all(int fd) noexcept {
        auto deadline = std::chrono::steady_clock::now() + config_.timeout;
        std::string buf;
        buf.reserve(4096);
        char chunk[4096];

        while (true) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                return std::unexpected(std::string("Receive timeout"));
            }

            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            int poll_ret = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
            if (poll_ret < 0) {
                if (errno == EINTR) continue;  // Interrupted — retry
                return std::unexpected(std::format("poll() failed: {}",
                    strerror(errno)));
            }
            if (poll_ret == 0) {
                // Timeout — only acceptable if the response is already complete.
                // For Connection: close without Content-Length, we rely on
                // recv() returning 0 (server closes), not poll timeout.
                if (is_response_complete(buf)) break;
                return std::unexpected(std::format(
                    "Receive timeout (got {}B, response incomplete)", buf.size()));
            }

            ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return std::unexpected(std::format("recv() failed: {}",
                    strerror(errno)));
            }
            if (n == 0) {
                // Connection closed by server — expected with Connection: close
                break;
            }
            if (buf.size() + static_cast<size_t>(n) > config_.max_response_size) {
                return std::unexpected(std::format(
                    "Response exceeds max_response_size ({}+{}B > {}B)",
                    buf.size(), n, config_.max_response_size));
            }
            buf.append(chunk, static_cast<size_t>(n));

            // Check if we have complete response (Content-Length based)
            if (auto complete = is_response_complete(buf); complete) {
                break;
            }
        }

        if (buf.empty()) {
            return std::unexpected(std::string("Empty response"));
        }
        return buf;
    }

    /// @brief Read all response data through TLS.
    /// @param ssl  SSL connection handle.
    /// @param fd   Underlying socket file descriptor (for poll).
    /// @return Complete raw HTTP response string, or error string.
    [[nodiscard]] std::expected<std::string, std::string>
    ssl_recv_all(SSL* ssl, int fd) noexcept {
        auto deadline = std::chrono::steady_clock::now() + config_.timeout;
        std::string buf;
        buf.reserve(4096);
        char chunk[4096];

        while (true) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                return std::unexpected(std::string("SSL receive timeout"));
            }

            ERR_clear_error();
            int n = SSL_read(ssl, chunk, sizeof(chunk));
            if (n > 0) {
                if (buf.size() + static_cast<size_t>(n) > config_.max_response_size) {
                    return std::unexpected(std::format(
                        "SSL response exceeds max_response_size ({}+{}B > {}B)",
                        buf.size(), n, config_.max_response_size));
                }
                buf.append(chunk, static_cast<size_t>(n));

                // Check if we have complete response
                if (auto complete = is_response_complete(buf); complete) {
                    break;
                }
                continue;
            }

            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_ZERO_RETURN) {
                // Clean TLS shutdown — peer closed connection
                break;
            }
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                // Re-check deadline — remaining may have gone negative since
                // the check at the top of the loop, which would cause poll()
                // to block indefinitely (negative timeout = infinite wait).
                auto remaining_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (remaining_now.count() <= 0) {
                    return std::unexpected(std::string("SSL receive timeout"));
                }
                struct pollfd pfd{};
                pfd.fd = fd;
                pfd.events = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                int prc = ::poll(&pfd, 1, static_cast<int>(remaining_now.count()));
                if (prc < 0 && errno != EINTR) {
                    return std::unexpected(std::format(
                        "SSL recv poll failed: {}", strerror(errno)));
                }
                continue;
            }

            // On connection reset (syscall error with n=0), treat as closed
            if (err == SSL_ERROR_SYSCALL && n == 0) {
                break;
            }

            return std::unexpected(std::format("SSL_read failed (err={}): {}",
                err, detail::ssl_last_error()));
        }

        if (buf.empty()) {
            return std::unexpected(std::string("Empty SSL response"));
        }
        return buf;
    }

public:
    /// @brief Check if we've received the complete HTTP response.
    ///
    /// Delegates to the free function is_http_response_complete() in http_message.hpp.
    /// Retained as a static method for backward compatibility with existing callers.
    ///
    /// @param buf  Raw response data received so far.
    /// @return true if complete, false if more data needed.
    [[nodiscard]] static bool is_response_complete(std::string_view buf) noexcept {
        return is_http_response_complete(buf);
    }

private:
    // ─────────────────────────────────────────────────────────────────────────
    // Main execute path
    // ─────────────────────────────────────────────────────────────────────────

    /// @brief Execute a pre-built HTTP request: connect, (TLS), send, recv, parse.
    /// @param request  Complete HTTP/1.1 request string (from build_http_request).
    /// @return Parsed HttpResponse or error string.
    [[nodiscard]] std::expected<HttpResponse, std::string>
    execute(const std::string& request) noexcept {
        [[maybe_unused]] auto* log = detail::http_client_logger();

        // 1. TCP connect
        auto sock_result = tcp_connect();
        if (!sock_result) return std::unexpected(sock_result.error());
        auto& sock = *sock_result;

        std::string raw_response;

        if (config_.use_tls) {
            // 2a. TLS handshake
            auto tls_result = tls_handshake(sock.fd);
            if (!tls_result) return std::unexpected(tls_result.error());
            auto& [ctx_ptr, ssl_ptr] = *tls_result;

            // 3a. Send request over TLS
            auto send_result = ssl_send_all(ssl_ptr.get(), sock.fd, request);
            if (!send_result) return std::unexpected(send_result.error());

            // 4a. Read response over TLS
            auto recv_result = ssl_recv_all(ssl_ptr.get(), sock.fd);
            if (!recv_result) return std::unexpected(recv_result.error());
            raw_response = std::move(*recv_result);
        } else {
            // 2b. Send request over plain TCP
            auto send_result = send_all(sock.fd, request);
            if (!send_result) return std::unexpected(send_result.error());

            // 3b. Read response over plain TCP
            auto recv_result = recv_all(sock.fd);
            if (!recv_result) return std::unexpected(recv_result.error());
            raw_response = std::move(*recv_result);
        }

        // 5. Parse response
        SPDLOG_LOGGER_TRACE(log, "Raw response ({} bytes): {}",
            raw_response.size(),
            raw_response.size() <= 512
                ? raw_response
                : raw_response.substr(0, 512) + "...");

        return parse_http_response(raw_response);
    }
};

} // namespace eph::net

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations
// ─────────────────────────────────────────────────────────────────────────────

/// @brief std::formatter for HttpClient::Config -- compact one-line summary.
template <>
struct std::formatter<eph::net::HttpClient::Config> : std::formatter<std::string> {
    auto format(const eph::net::HttpClient::Config& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("HttpClient::Config({}:{}, tls={}, timeout={}ms)",
                c.host, c.port, c.use_tls, c.timeout.count()),
            ctx);
    }
};
