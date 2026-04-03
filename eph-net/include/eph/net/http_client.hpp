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

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <future>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "eph/core/detail/string_checks.hpp"

namespace eph::net {

// ─────────────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Parsed HTTP response.
///
/// Contains the status code, raw header block, and body text
/// from a completed HTTP/1.1 response.
struct HttpResponse {
    int         status_code = 0;  ///< HTTP status code (e.g. 200, 404, 500).
    std::string body;             ///< Response body text.
    std::string headers_raw;      ///< Raw header block (excluding status line) for optional parsing.

    /// Check if the response indicates success (2xx status code).
    [[nodiscard]] constexpr bool is_success() const noexcept {
        return status_code >= 200 && status_code < 300;
    }

    /// Check if the response indicates a client error (4xx status code).
    [[nodiscard]] constexpr bool is_client_error() const noexcept {
        return status_code >= 400 && status_code < 500;
    }

    /// Check if the response indicates a server error (5xx status code).
    [[nodiscard]] constexpr bool is_server_error() const noexcept {
        return status_code >= 500 && status_code < 600;
    }

    /// @brief JSON-formatted summary for monitoring system integration.
    ///
    /// Body is truncated to prevent massive JSON blobs. Use .body directly
    /// for the full response content.
    [[nodiscard]] std::string to_json() const {
        // Truncate body at 256 chars to keep JSON manageable
        std::string_view body_preview = body;
        bool truncated = false;
        if (body_preview.size() > 256) {
            body_preview = body_preview.substr(0, 256);
            truncated = true;
        }
        return std::format(
            "{{\"status_code\":{},\"body_size\":{},\"is_success\":{}"
            ",\"body_preview\":\"{}{}\"}}",
            status_code, body.size(),
            is_success() ? "true" : "false",
            body_preview, truncated ? "..." : "");
    }

    /// Defaulted equality -- all fields must match exactly.
    [[nodiscard]] friend bool operator==(const HttpResponse&,
                                         const HttpResponse&) = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// Logger
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

/// @brief Lazily-initialized logger for the HttpClient subsystem.
/// @return Pointer to the "net.http_client" spdlog logger.
inline spdlog::logger* http_client_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.http_client");
        if (!lg) lg = spdlog::stdout_color_mt("net.http_client");
        return lg;
    }();
    return l.get();
}

/// @brief Retrieve the most recent OpenSSL error as a human-readable string.
/// @return Error description from the OpenSSL error queue.
inline std::string ssl_last_error() {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return std::string(buf);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// HTTP request builder (pure, testable)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Build an HTTP/1.1 request string.
///
/// @param method          HTTP method (e.g. "GET", "POST")
/// @param host            Host header value
/// @param path            Request URI path (e.g. "/api/v1/order")
/// @param body            Request body (empty for GET)
/// @param content_type    Content-Type header value (only added if body is non-empty)
/// @param extra_headers   Additional headers, each terminated with \r\n
/// @return Complete HTTP/1.1 request string, or error if params are invalid
[[nodiscard]] inline std::expected<std::string, std::string>
build_http_request(std::string_view method,
                   std::string_view host,
                   std::string_view path,
                   std::string_view body = {},
                   std::string_view content_type = {},
                   std::string_view extra_headers = {}) noexcept {
    if (method.empty()) {
        SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
            "build_http_request: method is empty");
        return std::unexpected(std::string("HTTP method must not be empty"));
    }
    if (host.empty()) {
        SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
            "build_http_request: host is empty");
        return std::unexpected(std::string("Host must not be empty"));
    }
    if (path.empty()) {
        SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
            "build_http_request: path is empty");
        return std::unexpected(std::string("Path must not be empty"));
    }

    std::string req;
    // Pre-allocate a reasonable size to avoid reallocations
    req.reserve(256 + body.size() + extra_headers.size());

    // Request line
    req.append(method);
    req.append(" ");
    req.append(path);
    req.append(" HTTP/1.1\r\n");

    // Mandatory headers
    req.append("Host: ");
    req.append(host);
    req.append("\r\n");

    // Close connection after response (no keep-alive)
    req.append("Connection: close\r\n");

    // Body-related headers
    if (!body.empty()) {
        if (!content_type.empty()) {
            req.append("Content-Type: ");
            req.append(content_type);
            req.append("\r\n");
        }
        req.append(std::format("Content-Length: {}\r\n", body.size()));
    }

    // User-supplied extra headers (must already be \r\n terminated per header).
    // Reject if it contains a blank line (\r\n\r\n) which would terminate
    // headers early — this prevents HTTP request smuggling via header injection.
    if (!extra_headers.empty()) {
        if (extra_headers.find("\r\n\r\n") != std::string_view::npos) {
            SPDLOG_LOGGER_ERROR(detail::http_client_logger(),
                "build_http_request: extra_headers contains blank line (potential injection)");
            return std::unexpected(std::string(
                "extra_headers must not contain \\r\\n\\r\\n (header injection risk)"));
        }
        req.append(extra_headers);
    }

    // End of headers
    req.append("\r\n");

    // Body
    if (!body.empty()) {
        req.append(body);
    }

    SPDLOG_LOGGER_DEBUG(detail::http_client_logger(),
        "Built HTTP request: {} {} ({} bytes total)",
        method, path, req.size());

    return req;
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP response parser (pure, testable)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Parse an HTTP/1.1 response from raw bytes.
///
/// Supports Content-Length and connection-close semantics.
/// Does NOT support chunked transfer encoding.
///
/// @param data  Complete raw HTTP response (headers + body)
/// @return Parsed response, or error string
[[nodiscard]] inline std::expected<HttpResponse, std::string>
parse_http_response(std::string_view data) noexcept {
    [[maybe_unused]] auto* log = detail::http_client_logger();

    // Find end of headers
    auto header_end = data.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        SPDLOG_LOGGER_WARN(log,
            "parse_http_response: no header terminator found in {} bytes",
            data.size());
        return std::unexpected(
            std::string("Incomplete HTTP response: no \\r\\n\\r\\n found"));
    }

    size_t body_start = header_end + 4;

    // Parse status line: "HTTP/1.1 200 OK\r\n"
    auto first_line_end = data.find("\r\n");
    if (first_line_end == std::string_view::npos) {
        return std::unexpected(std::string("Malformed HTTP response: no status line"));
    }

    auto status_line = data.substr(0, first_line_end);

    // Find status code (after first space)
    auto space1 = status_line.find(' ');
    if (space1 == std::string_view::npos) {
        SPDLOG_LOGGER_WARN(log,
            "parse_http_response: malformed status line: '{}'",
            std::string(status_line));
        return std::unexpected(std::format(
            "Malformed HTTP status line: '{}'", status_line));
    }

    auto code_start = space1 + 1;
    auto space2 = status_line.find(' ', code_start);
    auto code_str = status_line.substr(code_start,
        (space2 != std::string_view::npos)
            ? space2 - code_start
            : std::string_view::npos);

    HttpResponse resp;
    auto [ptr, ec] = std::from_chars(
        code_str.data(), code_str.data() + code_str.size(),
        resp.status_code);
    if (ec != std::errc{}) {
        SPDLOG_LOGGER_WARN(log,
            "parse_http_response: invalid status code: '{}'",
            std::string(code_str));
        return std::unexpected(std::format(
            "Invalid HTTP status code: '{}'", code_str));
    }

    // Extract raw headers (between status line and the blank line)
    resp.headers_raw = std::string(
        data.substr(first_line_end + 2, header_end - (first_line_end + 2)));

    // Body is everything after \r\n\r\n
    resp.body = std::string(data.substr(body_start));

    SPDLOG_LOGGER_DEBUG(log,
        "Parsed HTTP response: status={}, headers_len={}, body_len={}",
        resp.status_code, resp.headers_raw.size(), resp.body.size());

    return resp;
}

/// @brief Extract a header value by name (case-insensitive) from raw headers.
///
/// @param headers_raw  Raw header block (lines separated by \r\n)
/// @param name         Header name to search for (case-insensitive)
/// @return Header value (trimmed), or empty string if not found
[[nodiscard]] inline std::string
find_header(std::string_view headers_raw, std::string_view name) noexcept {
    size_t pos = 0;
    while (pos < headers_raw.size()) {
        auto line_end = headers_raw.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            line_end = headers_raw.size();
        }

        auto line = headers_raw.substr(pos, line_end - pos);
        pos = (line_end == headers_raw.size()) ? line_end : line_end + 2;

        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;

        auto hdr_name = line.substr(0, colon);

        // Case-insensitive name comparison
        if (hdr_name.size() != name.size()) continue;
        bool match = true;
        for (size_t i = 0; i < hdr_name.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(hdr_name[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) {
                match = false;
                break;
            }
        }
        if (!match) continue;

        // Trim OWS from value
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            value.remove_suffix(1);

        return std::string(value);
    }
    return {};
}

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

        auto port_str = std::to_string(config_.port);

        struct addrinfo* result = nullptr;
        // Run getaddrinfo asynchronously with a timeout — blocking DNS
        // resolution can hang indefinitely if the nameserver is unreachable.
        auto dns_future = std::async(std::launch::async, [&]() {
            return ::getaddrinfo(config_.host.c_str(), port_str.c_str(),
                                &hints, &result);
        });
        auto dns_status = dns_future.wait_for(config_.timeout);
        if (dns_status != std::future_status::ready) {
            SPDLOG_LOGGER_ERROR(log, "DNS resolution timeout for {}:{} ({}ms)",
                config_.host, config_.port, config_.timeout.count());
            return std::unexpected(std::format(
                "DNS resolution timeout for {}:{} ({}ms)",
                config_.host, config_.port, config_.timeout.count()));
        }
        int gai_err = dns_future.get();
        if (gai_err != 0) {
            SPDLOG_LOGGER_ERROR(log, "getaddrinfo failed for {}:{}: {}",
                config_.host, config_.port, gai_strerror(gai_err));
            return std::unexpected(std::format(
                "DNS resolution failed for {}:{}: {}",
                config_.host, config_.port, gai_strerror(gai_err)));
        }

        // RAII cleanup for addrinfo
        struct AddrInfoGuard {
            struct addrinfo* info;
            ~AddrInfoGuard() { if (info) ::freeaddrinfo(info); }
        } guard{result};

        // Try each resolved address
        for (auto* rp = result; rp != nullptr; rp = rp->ai_next) {
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
            buf.append(chunk, static_cast<size_t>(n));

            if (buf.size() > config_.max_response_size) {
                return std::unexpected(std::format(
                    "Response exceeds max_response_size ({}B > {}B)",
                    buf.size(), config_.max_response_size));
            }

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
                buf.append(chunk, static_cast<size_t>(n));

                if (buf.size() > config_.max_response_size) {
                    return std::unexpected(std::format(
                        "SSL response exceeds max_response_size ({}B > {}B)",
                        buf.size(), config_.max_response_size));
                }

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
    /// @brief Check if we've received the complete HTTP response based on Content-Length.
    ///
    /// Handles Content-Length, chunked Transfer-Encoding, and connection-close semantics.
    /// Public for testing -- also used internally by recv loops.
    ///
    /// @param buf  Raw response data received so far.
    /// @return true if complete, false if more data needed.
    [[nodiscard]] static bool is_response_complete(std::string_view buf) noexcept {
        auto header_end = buf.find("\r\n\r\n");
        if (header_end == std::string_view::npos) return false;

        size_t body_start = header_end + 4;
        size_t body_len = buf.size() - body_start;

        // Extract raw headers (between status line end and blank line)
        auto headers_raw = buf.substr(0, header_end);

        // Use case-insensitive find_header instead of manual substring search
        auto cl_value = find_header(headers_raw, "Content-Length");
        if (cl_value.empty()) {
            // No Content-Length — check for chunked transfer encoding.
            auto te = find_header(headers_raw, "Transfer-Encoding");
            if (te.find("chunked") != std::string_view::npos) {
                // Chunked: complete when final chunk marker "0\r\n\r\n" is present
                return buf.find("0\r\n\r\n", body_start) != std::string_view::npos
                    || buf.find("\r\n0\r\n\r\n", body_start) != std::string_view::npos;
            }
            // Neither Content-Length nor chunked — rely on connection close.
            // Cap total buffer to prevent unbounded growth.
            constexpr size_t kMaxNoClBuffer = 16 * 1024 * 1024; // 16 MiB
            if (buf.size() > kMaxNoClBuffer) {
                SPDLOG_LOGGER_WARN(detail::http_client_logger(),
                    "Response without Content-Length exceeds {} bytes, "
                    "treating as complete", kMaxNoClBuffer);
                return true;
            }
            return false;
        }

        size_t content_length = 0;
        auto [ptr, ec] = std::from_chars(
            cl_value.data(),
            cl_value.data() + cl_value.size(),
            content_length);
        if (ec != std::errc{}) return false;

        // Reject absurdly large Content-Length to prevent OOM (256 MiB limit for REST responses)
        constexpr size_t kMaxContentLength = 256 * 1024 * 1024;
        if (content_length > kMaxContentLength) {
            SPDLOG_LOGGER_WARN(detail::http_client_logger(),
                "Content-Length {} exceeds maximum allowed {} bytes",
                content_length, kMaxContentLength);
            return false;
        }

        return body_len >= content_length;
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

/// @brief std::formatter for HttpResponse -- compact one-line summary.
///
/// Shows status code, body size, and a truncated body preview.
/// Full body is omitted to keep log output manageable.
template <>
struct std::formatter<eph::net::HttpResponse> : std::formatter<std::string> {
    auto format(const eph::net::HttpResponse& r, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("HttpResponse(status={}, body={}B)",
                r.status_code, r.body.size()),
            ctx);
    }
};

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
