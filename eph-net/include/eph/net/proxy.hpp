#pragma once

/// @file proxy.hpp
/// SOCKS5 and HTTP CONNECT proxy support for SocketTransport.
///
/// Provides:
///   - ProxyConfig: proxy server address, type, and optional auth
///   - socks5_handshake(): RFC 1928 + RFC 1929 on an established TCP connection
///   - http_connect_handshake(): RFC 7231 §4.3.6 CONNECT tunnel
///   - make_proxied_factory(): convenience TcpFactory builder for Transport::create()
///
/// Design: proxy handshakes execute in the TcpFactory lambda (connect path only),
/// not on the hot data path.  When no proxy is used, this header is never included
/// and zero code is generated.
///
/// Security:
///   - SOCKS5 uses domain name type (0x03) for remote DNS resolution — no local
///     DNS leak through the proxy.
///   - ProxyConfig::password is never included in log output or error messages.
///   - Proxy type must be explicitly specified; no port-based guessing.

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/detail/base64.hpp"
#include "eph/core/detail/string_checks.hpp"
#include "eph/net/socket_transport.hpp"

namespace eph::net::proxy {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/// @brief Proxy protocol type.
enum class ProxyType : uint8_t {
    kSocks5,       ///< SOCKS5 (RFC 1928)
    kHttpConnect,  ///< HTTP CONNECT (RFC 7231 §4.3.6)
};

/// @brief Convert a ProxyType value to a human-readable name.
/// @param t  The proxy type to convert.
/// @return Null-terminated string view such as "SOCKS5" or "HTTP_CONNECT".
[[nodiscard]] constexpr std::string_view proxy_type_name(ProxyType t) noexcept {
    switch (t) {
    case ProxyType::kSocks5:      return "SOCKS5";
    case ProxyType::kHttpConnect: return "HTTP_CONNECT";
    }
    return "UNKNOWN";
}

/// @brief Proxy server configuration.
///
/// @note password is never logged or included in error messages.
struct ProxyConfig {
    std::string host;
    uint16_t    port = 1080;
    ProxyType   type = ProxyType::kSocks5;

    /// @brief Optional SOCKS5 username/password auth (RFC 1929).
    /// Ignored for HTTP CONNECT (use extra_headers for Proxy-Authorization).
    std::string username{};
    /// @brief Authentication password. Never logged or included in error messages.
    std::string password{};

    /// @brief Timeout for the proxy handshake (not the TCP connect to the proxy).
    std::chrono::milliseconds timeout{5000};

    /// @brief Serialize as a proxy URL string (inverse of parse_proxy_url).
    ///
    /// Format: scheme://[user:pass@]host:port
    /// Password is included in the URL — handle with care.
    ///
    /// @return URL string like "socks5://user:pass@host:port"
    [[nodiscard]] std::string to_url() const {
        std::string_view scheme = (type == ProxyType::kSocks5) ? "socks5" : "http";
        if (username.empty()) {
            return std::format("{}://{}:{}", scheme, host, port);
        }
        if (password.empty()) {
            return std::format("{}://{}@{}:{}", scheme, username, host, port);
        }
        return std::format("{}://{}:{}@{}:{}", scheme, username, password, host, port);
    }

    /// @brief Validate configuration, returning an error description or empty string on success.
    /// @return Empty string_view on success; otherwise a human-readable error description.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (host.empty())
            return "proxy host must not be empty";
        // Reject control characters in proxy hostname. For HTTP CONNECT
        // proxies, the host appears in the CONNECT request line; control
        // characters could enable request smuggling.
        if (eph::core::detail::contains_control_chars(host))
            return "proxy host contains control characters";
        if (port == 0)
            return "proxy port must be > 0";
        if (timeout.count() <= 0)
            return "proxy timeout must be positive";
        if (type == ProxyType::kSocks5 && username.size() > 255)
            return "SOCKS5 username exceeds 255 bytes";
        if (type == ProxyType::kSocks5 && password.size() > 255)
            return "SOCKS5 password exceeds 255 bytes";
        return {};
    }

    /// @brief Multi-line formatted dump for logging/debugging.
    ///
    /// Password is redacted to prevent accidental credential leakage in logs.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "ProxyConfig:\n"
            "  type: {}\n"
            "  target: {}:{}\n"
            "  timeout: {}ms\n"
            "  auth: {}",
            proxy_type_name(type), host, port,
            timeout.count(),
            username.empty() ? std::string("none")
                             : std::format("user='{}' pass=<redacted>", username));
    }

    /// @brief JSON-formatted config for monitoring system integration.
    ///
    /// Password is always redacted in JSON output.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"type\":\"{}\",\"host\":\"{}\",\"port\":{},"
            "\"timeout_ms\":{},\"username\":\"{}\",\"has_password\":{}}}",
            proxy_type_name(type), host, port,
            timeout.count(), username,
            password.empty() ? "false" : "true");
    }

    /// @brief Equality comparison. Password is compared for completeness
    /// (both configs must represent the same proxy endpoint with the same credentials).
    [[nodiscard]] friend bool operator==(const ProxyConfig&,
                                         const ProxyConfig&) = default;

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate() which blocks construction, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        if (timeout.count() < 1000)
            w.emplace_back(std::format(
                "proxy timeout={}ms is short — handshake may fail on "
                "high-latency networks", timeout.count()));
        if (type == ProxyType::kSocks5 && !username.empty() && password.empty())
            w.emplace_back("SOCKS5 username set without password — server "
                           "may reject authentication");
        if (type == ProxyType::kHttpConnect && !username.empty())
            w.emplace_back("HTTP CONNECT with credentials uses Basic auth "
                           "(base64, not encrypted) — ensure TLS is used");
        return w;
    }
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail {

/// @brief Lazily-initialized logger for the proxy subsystem.
/// @return Pointer to the "net.proxy" spdlog logger.
inline spdlog::logger* proxy_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.proxy");
        if (!lg) lg = spdlog::stdout_color_mt("net.proxy");
        return lg;
    }();
    return l.get();
}

/// @brief Buffered I/O helper for proxy handshakes.
///
/// poll_rx() may deliver more bytes than requested in a single callback.
/// This helper buffers excess bytes so subsequent read_exact() calls
/// consume them without another poll_rx() round-trip.
struct HandshakeIO {
    SocketTransport& tcp;
    std::vector<uint8_t> buf;  // leftover bytes from previous poll_rx

    /// @brief Construct from a connected SocketTransport.
    /// @param t  Reference to the TCP transport (must be connected).
    explicit HandshakeIO(SocketTransport& t) : tcp(t) { buf.reserve(256); }

    /// @brief Read exactly @p needed bytes into @p out, blocking until complete or timeout.
    /// @param out      Destination buffer (must have capacity >= needed).
    /// @param needed   Number of bytes to read.
    /// @param timeout  Maximum time to wait for the remaining bytes.
    /// @return void on success, error string on timeout or I/O failure.
    std::expected<void, std::string>
    read_exact(uint8_t* out, size_t needed, std::chrono::milliseconds timeout) {
        size_t filled = 0;
        auto deadline = std::chrono::steady_clock::now() + timeout;

        // Drain leftover buffer first
        if (!buf.empty()) {
            size_t from_buf = std::min(buf.size(), needed);
            std::memcpy(out, buf.data(), from_buf);
            buf.erase(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(from_buf));
            filled = from_buf;
        }

        while (filled < needed) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                return std::unexpected(std::format(
                    "proxy handshake read timeout (got {}/{} bytes)",
                    filled, needed));
            }

            auto result = tcp.poll_rx_for([&](const uint8_t* data, uint16_t len) {
                size_t want = needed - filled;
                size_t take = std::min(static_cast<size_t>(len), want);
                std::memcpy(out + filled, data, take);
                filled += take;
                // Buffer excess bytes for subsequent reads
                if (static_cast<size_t>(len) > take) {
                    buf.insert(buf.end(), data + take, data + len);
                }
            }, remaining);

            if (!result) {
                return std::unexpected(std::format(
                    "proxy handshake read failed: {}", result.error()));
            }
        }
        return {};
    }

    /// @brief Send all bytes, blocking until complete.
    /// @param data  Pointer to the bytes to send.
    /// @param len   Number of bytes to send.
    /// @return void on success, error string on I/O failure.
    std::expected<void, std::string>
    send_all(const uint8_t* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            auto result = tcp.send(data + sent, len - sent);
            if (!result) {
                return std::unexpected(std::format(
                    "proxy handshake send failed: {}", result.error()));
            }
            sent += *result;
        }
        return {};
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// SOCKS5 handshake (RFC 1928 + RFC 1929)
// ---------------------------------------------------------------------------

/// @brief Execute SOCKS5 handshake on an already-connected TCP socket.
///
/// The socket must be connected to the proxy server (ProxyConfig::host:port).
/// After successful return, the socket is tunneled to target_host:target_port
/// and ready for TLS/WS handshake.
///
/// Uses domain name type (ATYP=0x03) for target_host to ensure DNS resolution
/// happens on the proxy side, preventing DNS leaks.
///
/// @param tcp          Connected SocketTransport (to the proxy server)
/// @param target_host  Target hostname (sent as domain name, max 255 chars)
/// @param target_port  Target port
/// @param cfg          Proxy configuration (auth credentials, timeout)
/// @return void on success, error string on failure
[[nodiscard]] inline std::expected<void, std::string>
socks5_handshake(SocketTransport& tcp,
                 std::string_view target_host, uint16_t target_port,
                 const ProxyConfig& cfg) {
    [[maybe_unused]] auto log = detail::proxy_logger();
    detail::HandshakeIO io(tcp);

    if (target_host.size() > 255) {
        SPDLOG_LOGGER_WARN(log,
            "SOCKS5: target hostname exceeds 255 bytes (len={})", target_host.size());
        return std::unexpected("SOCKS5: target hostname exceeds 255 bytes");
    }

    bool has_auth = !cfg.username.empty();
    SPDLOG_LOGGER_DEBUG(log, "SOCKS5 handshake to {}:{} (auth={})",
                        target_host, target_port, has_auth);

    // ── Phase 1: Greeting ─────────────────────────────────────────────────
    // Client sends: VER(1) NMETHODS(1) METHODS(N)
    uint8_t greeting[4];
    size_t greeting_len;
    if (has_auth) {
        // Offer two methods: 0x00 = no auth, 0x02 = username/password (RFC 1929)
        greeting[0] = 0x05;  greeting[1] = 0x02;
        greeting[2] = 0x00;  greeting[3] = 0x02;
        greeting_len = 4;
    } else {
        // Offer one method: 0x00 = no authentication required
        greeting[0] = 0x05;  greeting[1] = 0x01;  greeting[2] = 0x00;
        greeting_len = 3;
    }

    if (auto r = io.send_all(greeting, greeting_len); !r) return std::unexpected(r.error());

    // Server responds: VER(1) METHOD(1)
    uint8_t server_choice[2];
    if (auto r = io.read_exact(server_choice, 2, cfg.timeout); !r) return std::unexpected(r.error());

    if (server_choice[0] != 0x05) {
        return std::unexpected(std::format(
            "SOCKS5: server version 0x{:02x}, expected 0x05", server_choice[0]));
    }

    uint8_t chosen_method = server_choice[1];
    SPDLOG_LOGGER_DEBUG(log, "SOCKS5 server chose method 0x{:02x}", chosen_method);

    // ── Phase 2: Authentication (if required) ─────────────────────────────
    if (chosen_method == 0x02) {
        if (cfg.username.size() > 255 || cfg.password.size() > 255) {
            SPDLOG_LOGGER_WARN(log,
                "SOCKS5: username/password exceeds 255 bytes (user={}, pass=<redacted>)",
                cfg.username.size());
            return std::unexpected("SOCKS5: username/password exceeds 255 bytes");
        }
        // RFC 1929: VER(1) ULEN(1) UNAME(ULEN) PLEN(1) PASSWD(PLEN)
        // Max: 1 + 1 + 255 + 1 + 255 = 513 bytes — fits comfortably on the stack.
        size_t auth_len = 3 + cfg.username.size() + cfg.password.size();
        uint8_t auth_buf[513];
        size_t off = 0;
        auth_buf[off++] = 0x01;
        auth_buf[off++] = static_cast<uint8_t>(cfg.username.size());
        std::memcpy(auth_buf + off, cfg.username.data(), cfg.username.size());
        off += cfg.username.size();
        auth_buf[off++] = static_cast<uint8_t>(cfg.password.size());
        std::memcpy(auth_buf + off, cfg.password.data(), cfg.password.size());

        if (auto r = io.send_all(auth_buf, auth_len); !r) return std::unexpected(r.error());

        uint8_t auth_resp[2];
        if (auto r = io.read_exact(auth_resp, 2, cfg.timeout); !r) return std::unexpected(r.error());

        if (auth_resp[1] != 0x00) {
            return std::unexpected("SOCKS5: authentication failed");
        }
        SPDLOG_LOGGER_DEBUG(log, "SOCKS5 authentication succeeded");
    } else if (chosen_method == 0x00) {
        // No auth required
    } else if (chosen_method == 0xFF) {  // 0xFF = no acceptable methods per RFC 1928

        return std::unexpected("SOCKS5: server rejected all auth methods");
    } else {
        return std::unexpected(std::format(
            "SOCKS5: unsupported auth method 0x{:02x}", chosen_method));
    }

    // ── Phase 3: Connect request ──────────────────────────────────────────
    // VER(1) CMD(1) RSV(1) ATYP(1) DST.ADDR(variable) DST.PORT(2)
    // Max: 4 + 1 + 255 + 2 = 262 bytes — stack-allocated to avoid heap overhead.
    size_t host_len = target_host.size();
    size_t req_len = 4 + 1 + host_len + 2;
    uint8_t req_buf[262];
    {
        size_t off = 0;
        req_buf[off++] = 0x05;  // VER
        req_buf[off++] = 0x01;  // CMD: CONNECT
        req_buf[off++] = 0x00;  // RSV
        req_buf[off++] = 0x03;  // ATYP: domain name (remote DNS — no leak)
        req_buf[off++] = static_cast<uint8_t>(host_len);
        std::memcpy(req_buf + off, target_host.data(), host_len);
        off += host_len;
        req_buf[off++] = static_cast<uint8_t>(target_port >> 8);
        req_buf[off++] = static_cast<uint8_t>(target_port & 0xFF);
    }

    if (auto r = io.send_all(req_buf, req_len); !r) return std::unexpected(r.error());

    // Server responds: VER(1) REP(1) RSV(1) ATYP(1) BND.ADDR(variable) BND.PORT(2)
    uint8_t resp_header[4];
    if (auto r = io.read_exact(resp_header, 4, cfg.timeout); !r) return std::unexpected(r.error());

    if (resp_header[0] != 0x05) {
        return std::unexpected(std::format(
            "SOCKS5: connect response version 0x{:02x}", resp_header[0]));
    }

    if (resp_header[1] != 0x00) {
        static constexpr const char* kSocks5Errors[] = {
            "succeeded", "general SOCKS server failure",
            "connection not allowed by ruleset", "network unreachable",
            "host unreachable", "connection refused",
            "TTL expired", "command not supported", "address type not supported",
        };
        const char* err_msg = (resp_header[1] < 9)
            ? kSocks5Errors[resp_header[1]] : "unknown error";
        return std::unexpected(std::format(
            "SOCKS5: connect failed: 0x{:02x} ({})", resp_header[1], err_msg));
    }

    // Drain BND.ADDR + BND.PORT based on ATYP
    size_t drain_len = 0;
    switch (resp_header[3]) {
        case 0x01: drain_len = 4 + 2; break;   // ATYP 0x01: IPv4 (4 bytes) + port (2 bytes)
        case 0x03: {                            // ATYP 0x03: Domain: length byte + domain + port
            uint8_t dlen;
            if (auto r = io.read_exact(&dlen, 1, cfg.timeout); !r) return std::unexpected(r.error());
            if (dlen > 16) {
                // Domain responses >16 bytes exceed our stack buffer.
                // Read and discard in chunks to drain the remaining bytes.
                uint8_t discard[128];
                size_t remaining = static_cast<size_t>(dlen) + 2; // domain + port
                while (remaining > 0) {
                    size_t chunk = std::min(remaining, sizeof(discard));
                    if (auto r = io.read_exact(discard, chunk, cfg.timeout); !r)
                        return std::unexpected(r.error());
                    remaining -= chunk;
                }
                SPDLOG_LOGGER_DEBUG(log, "SOCKS5 tunnel established to {}:{}", target_host, target_port);
                return {};
            }
            drain_len = dlen + 2;
            break;
        }
        case 0x04: drain_len = 16 + 2; break;  // ATYP 0x04: IPv6 (16 bytes) + port (2 bytes)
        default:
            return std::unexpected(std::format(
                "SOCKS5: unknown ATYP 0x{:02x}", resp_header[3]));
    }

    // Max drain: IPv6(16) + port(2) = 18 bytes — stack buffer suffices.
    uint8_t drain_buf[18];
    if (auto r = io.read_exact(drain_buf, drain_len, cfg.timeout); !r)
        return std::unexpected(r.error());

    SPDLOG_LOGGER_INFO(log, "SOCKS5 tunnel established to {}:{}", target_host, target_port);
    return {};
}

// ---------------------------------------------------------------------------
// HTTP CONNECT handshake (RFC 7231 §4.3.6)
// ---------------------------------------------------------------------------

/// @brief Execute HTTP CONNECT handshake on an already-connected TCP socket.
///
/// After successful return, the socket is tunneled to target_host:target_port.
///
/// @param tcp          Connected SocketTransport (to the proxy server)
/// @param target_host  Target hostname
/// @param target_port  Target port
/// @param cfg          Proxy configuration (timeout; auth via username/password → Basic auth)
/// @return void on success, error string on failure
[[nodiscard]] inline std::expected<void, std::string>
http_connect_handshake(SocketTransport& tcp,
                       std::string_view target_host, uint16_t target_port,
                       const ProxyConfig& cfg) {
    [[maybe_unused]] auto log = detail::proxy_logger();
    SPDLOG_LOGGER_DEBUG(log, "HTTP CONNECT to {}:{}", target_host, target_port);

    // Build CONNECT request
    std::string request = std::format(
        "CONNECT {}:{} HTTP/1.1\r\n"
        "Host: {}:{}\r\n",
        target_host, target_port,
        target_host, target_port);

    // Basic auth if credentials provided
    if (!cfg.username.empty()) {
        auto plain = std::format("{}:{}", cfg.username, cfg.password);
        auto encoded = eph::core::detail::base64_encode(plain);
        request += std::format("Proxy-Authorization: Basic {}\r\n", encoded);
    }

    request += "\r\n";

    detail::HandshakeIO io(tcp);
    if (auto r = io.send_all(
            reinterpret_cast<const uint8_t*>(request.data()), request.size()); !r)
        return std::unexpected(r.error());

    // Read response — look for "HTTP/1.x 200" and "\r\n\r\n"
    std::string response;
    response.reserve(512);
    auto deadline = std::chrono::steady_clock::now() + cfg.timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;

        // 8 KiB cap prevents unbounded buffering from a misbehaving proxy
        constexpr size_t kMaxConnectResponse = 8192;
        auto result = tcp.poll_rx_for([&](const uint8_t* data, uint16_t len) {
            size_t take = std::min(static_cast<size_t>(len),
                                   kMaxConnectResponse - std::min(response.size(), kMaxConnectResponse));
            if (take > 0)
                response.append(reinterpret_cast<const char*>(data), take);
        }, remaining);

        if (!result) {
            return std::unexpected(std::format(
                "HTTP CONNECT read failed: {}", result.error()));
        }

        // Check for complete response (header ends with \r\n\r\n)
        if (response.find("\r\n\r\n") != std::string::npos) {
            break;
        }

        if (response.size() > 8192) {
            SPDLOG_LOGGER_WARN(log,
                "HTTP CONNECT: response too large ({}B > 8KB) from {}:{}",
                response.size(), target_host, target_port);
            return std::unexpected("HTTP CONNECT: response too large (>8KB)");
        }
    }

    if (response.find("\r\n\r\n") == std::string::npos) {
        SPDLOG_LOGGER_WARN(log,
            "HTTP CONNECT: handshake timeout ({}ms) to {}:{}, received {}B",
            cfg.timeout.count(), target_host, target_port, response.size());
        return std::unexpected("HTTP CONNECT: response timeout");
    }

    // Parse status line: "HTTP/1.x NNN reason\r\n"
    auto first_space = response.find(' ');
    if (first_space == std::string::npos || first_space + 3 >= response.size()) {
        return std::unexpected(std::format(
            "HTTP CONNECT: malformed response: {:.40}", response));
    }

    int status_code = 0;
    auto status_str = response.substr(first_space + 1, 3);
    auto [ptr, ec] = std::from_chars(
        status_str.data(), status_str.data() + status_str.size(), status_code);
    if (ec != std::errc{} || ptr != status_str.data() + status_str.size()) {
        return std::unexpected(std::format(
            "HTTP CONNECT: invalid status code: {}", status_str));
    }

    if (status_code != 200) {
        // Extract reason phrase for better error message
        auto reason_start = first_space + 5;  // skip "NNN "
        auto reason_end = response.find("\r\n");
        std::string_view reason = (reason_start < reason_end)
            ? std::string_view(response).substr(reason_start, reason_end - reason_start)
            : "unknown";
        return std::unexpected(std::format(
            "HTTP CONNECT: proxy returned {} {}", status_code, reason));
    }

    // Warn if the proxy sent tunneled data after the response headers
    // in the same TCP segment — those bytes are consumed from the socket
    // and would be lost.  In practice proxies don't do this, but log it
    // so a future debugger knows to look here if TLS handshakes fail.
    auto hdr_end = response.find("\r\n\r\n");
    if (hdr_end + 4 < response.size()) {
        SPDLOG_LOGGER_WARN(log,
            "HTTP CONNECT: {} leftover bytes after response headers "
            "(tunneled data may be lost)",
            response.size() - hdr_end - 4);
    }

    SPDLOG_LOGGER_INFO(log, "HTTP CONNECT tunnel established to {}:{}",
                       target_host, target_port);
    return {};
}

// ---------------------------------------------------------------------------
// Convenience factory builder
// ---------------------------------------------------------------------------

/// @brief Build a TcpFactory that connects through a proxy.
///
/// The returned factory:
///   1. Creates a SocketTransport connected to proxy_cfg.host:proxy_cfg.port
///   2. Executes the proxy handshake (SOCKS5 or HTTP CONNECT)
///   3. Returns the tunneled socket ready for TLS/WS handshake
///
/// Usage:
/// @code
///   auto factory = proxy::make_proxied_factory(
///       sock_cfg, proxy_cfg, "stream.binance.com", 9443);
///   auto result = Transport<SocketTransport>::create(
///       std::move(factory), transport_cfg);
/// @endcode
///
/// @param base_sock_cfg  Base SocketConfig (tcp_nodelay, keepalive, etc.)
///                       host/port will be overridden with proxy address.
/// @param proxy_cfg      Proxy server configuration
/// @param target_host    Ultimate target hostname (for proxy CONNECT)
/// @param target_port    Ultimate target port
/// @return TcpFactory suitable for Transport::create()
inline std::function<std::expected<std::unique_ptr<SocketTransport>, std::string>()>
make_proxied_factory(const SocketConfig& base_sock_cfg,
                     const ProxyConfig& proxy_cfg,
                     std::string_view target_host, uint16_t target_port) {
    // Fail-fast: validate proxy config before capturing into the factory closure
    if (auto err = proxy_cfg.validate(); !err.empty()) {
        [[maybe_unused]] auto log = detail::proxy_logger();
        SPDLOG_LOGGER_ERROR(log, "ProxyConfig validation failed: {}", err);
        // Return a factory that always fails with the validation error
        return [msg = std::string(err)]()
            -> std::expected<std::unique_ptr<SocketTransport>, std::string> {
            return std::unexpected(std::format("ProxyConfig invalid: {}", msg));
        };
    }

    // Capture by value for reconnection safety (factory is called multiple times)
    return [sock_cfg = base_sock_cfg, proxy = proxy_cfg,
            host = std::string(target_host), port = target_port]()
        -> std::expected<std::unique_ptr<SocketTransport>, std::string> {

        // Override host/port to connect to proxy server
        SocketConfig proxy_sock = sock_cfg;
        proxy_sock.host = proxy.host;
        proxy_sock.port = proxy.port;

        auto tcp = std::make_unique<SocketTransport>(proxy_sock);
        auto connect_r = tcp->connect(proxy.timeout);
        if (!connect_r) {
            return std::unexpected(std::format(
                "proxy connect failed ({}:{}): {}",
                proxy.host, proxy.port, connect_r.error()));
        }

        // Execute proxy handshake based on type
        std::expected<void, std::string> hs_result;
        switch (proxy.type) {
            case ProxyType::kSocks5:
                hs_result = socks5_handshake(*tcp, host, port, proxy);
                break;
            case ProxyType::kHttpConnect:
                hs_result = http_connect_handshake(*tcp, host, port, proxy);
                break;
        }

        if (!hs_result) {
            return std::unexpected(hs_result.error());
        }

        return tcp;
    };
}

// ---------------------------------------------------------------------------
// Proxy URL parser
// ---------------------------------------------------------------------------

/// @brief Parse a proxy URL string into a ProxyConfig.
///
/// Supported formats:
///   socks5://host:port
///   socks5://user:pass@host:port
///   http://host:port
///   http://user:pass@host:port
///
/// @param url  Proxy URL string
/// @return ProxyConfig on success, error string on failure
[[nodiscard]] inline std::expected<ProxyConfig, std::string>
parse_proxy_url(std::string_view url) {
    ProxyConfig cfg;

    // Determine type from scheme
    if (url.starts_with("socks5://")) {
        cfg.type = ProxyType::kSocks5;
        url.remove_prefix(9);
    } else if (url.starts_with("http://")) {
        cfg.type = ProxyType::kHttpConnect;
        url.remove_prefix(7);
    } else {
        return std::unexpected(std::format(
            "unsupported proxy scheme (expected socks5:// or http://): {}", url));
    }

    // Check for user:pass@ — use rfind to split on the LAST '@' so that
    // passwords containing '@' are handled correctly (hostnames never
    // contain '@', so the last '@' always separates auth from host).
    auto at_pos = url.rfind('@');
    if (at_pos != std::string_view::npos) {
        auto auth = url.substr(0, at_pos);
        auto colon = auth.find(':');
        if (colon != std::string_view::npos) {
            cfg.username = std::string(auth.substr(0, colon));
            cfg.password = std::string(auth.substr(colon + 1));
        } else {
            cfg.username = std::string(auth);
        }
        url.remove_prefix(at_pos + 1);
    }

    // Parse host:port
    auto colon = url.rfind(':');
    if (colon == std::string_view::npos) {
        return std::unexpected(std::format(
            "proxy URL missing port: {}", url));
    }

    cfg.host = std::string(url.substr(0, colon));
    auto port_str = url.substr(colon + 1);
    if (port_str.empty()) {
        return std::unexpected("proxy URL missing port number after ':'");
    }
    uint32_t port_val = 0;
    auto [ptr, ec] = std::from_chars(port_str.data(),
                                     port_str.data() + port_str.size(),
                                     port_val);
    if (ec != std::errc{} || ptr != port_str.data() + port_str.size()) {
        return std::unexpected(std::format(
            "proxy URL invalid port: {}", port_str));
    }
    if (port_val == 0 || port_val > 65535) {
        return std::unexpected(std::format(
            "proxy URL port out of range: {}", port_val));
    }
    cfg.port = static_cast<uint16_t>(port_val);

    return cfg;
}

} // namespace eph::net::proxy

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specialization for ProxyType
// ─────────────────────────────────────────────────────────────────────────────

/// @brief std::formatter specialization for ProxyType.
///
/// Formats as "SOCKS5" or "HTTP_CONNECT".
template <>
struct std::formatter<eph::net::proxy::ProxyType> : std::formatter<std::string_view> {
    auto format(eph::net::proxy::ProxyType t, auto& ctx) const {
        return std::formatter<std::string_view>::format(
            eph::net::proxy::proxy_type_name(t), ctx);
    }
};

/// @brief std::formatter specialization for ProxyConfig -- compact one-line summary.
///
/// Password is never included in formatted output to prevent credential leakage.
template <>
struct std::formatter<eph::net::proxy::ProxyConfig> : std::formatter<std::string> {
    auto format(const eph::net::proxy::ProxyConfig& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("ProxyConfig({}://{}:{}, timeout={}ms)",
                eph::net::proxy::proxy_type_name(c.type),
                c.host, c.port, c.timeout.count()),
            ctx);
    }
};
