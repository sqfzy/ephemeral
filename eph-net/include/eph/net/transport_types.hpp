#pragma once

/// @file transport_types.hpp
/// Public types for the Transport layer — enums, config, stats, formatters.
///
/// This header is deliberately lightweight: it depends only on the standard
/// library and tcp_concept.hpp, so downstream code (including DPDK backends)
/// can use these types without pulling in TLS, WebSocket, or SPSC queue
/// headers.

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "eph/net/detail/json_escape.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// Connection error types
// ---------------------------------------------------------------------------

/// Categorizes connection failures so callers can programmatically distinguish
/// between different failure modes (e.g., retry on transient TCP errors but
/// abort on TLS certificate rejection).
enum class ConnectionError : uint8_t {
    kInvalidConfig,      ///< TransportConfig validation failed
    kFactoryFailed,      ///< TcpFactory returned an error
    kTcpNotEstablished,  ///< Factory returned a non-established TCP session
    kTlsSessionFailed,   ///< TLS session creation failed
    kTlsHandshakeFailed, ///< TLS handshake failed (cert verification, protocol mismatch)
    kTlsKeyExportFailed, ///< TLS AEAD key export failed
    kWsUpgradeFailed,    ///< WebSocket HTTP upgrade failed (parse error, timeout)
    kWsUpgradeRejected,  ///< Server rejected upgrade (non-101 status code)
    kWsAcceptInvalid,    ///< Sec-WebSocket-Accept validation failed
};

/// Human-readable name for a ConnectionError.
constexpr const char* connection_error_name(ConnectionError e) noexcept {
    switch (e) {
        case ConnectionError::kInvalidConfig:      return "INVALID_CONFIG";
        case ConnectionError::kFactoryFailed:      return "FACTORY_FAILED";
        case ConnectionError::kTcpNotEstablished:  return "TCP_NOT_ESTABLISHED";
        case ConnectionError::kTlsSessionFailed:   return "TLS_SESSION_FAILED";
        case ConnectionError::kTlsHandshakeFailed: return "TLS_HANDSHAKE_FAILED";
        case ConnectionError::kTlsKeyExportFailed: return "TLS_KEY_EXPORT_FAILED";
        case ConnectionError::kWsUpgradeFailed:    return "WS_UPGRADE_FAILED";
        case ConnectionError::kWsUpgradeRejected:  return "WS_UPGRADE_REJECTED";
        case ConnectionError::kWsAcceptInvalid:    return "WS_ACCEPT_INVALID";
    }
    return "UNKNOWN";
}

/// Structured connection error with typed category and detail message.
/// Replaces opaque error strings from Transport::create(), enabling
/// callers to match on error category for retry/abort decisions.
struct ConnectionErrorInfo {
    ConnectionError code;       ///< Error category (for programmatic matching)
    std::string     detail;     ///< Human-readable detail (for logging)

    /// Full error message combining category name and detail.
    [[nodiscard]] std::string message() const {
        return std::format("[{}] {}", connection_error_name(code), detail);
    }

    /// JSON-formatted error info for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{\"code\":\"{}\",\"detail\":\"{}\",\"http_status\":{}}}",
            detail::json_escape(connection_error_name(code)),
            detail::json_escape(detail),
            http_status);
    }

    /// HTTP status code from server rejection (only valid when code == kWsUpgradeRejected).
    int http_status = 0;
};

// ---------------------------------------------------------------------------
// Send result
// ---------------------------------------------------------------------------

/// Result type for Transport::send() and related methods.
/// Replaces raw errno return codes with a type-safe enum that
/// enables exhaustive switch checking at compile time.
enum class SendError : int8_t {
    kOk             =  0,   ///< Message enqueued successfully
    kMessageTooLarge = -1,  ///< Payload exceeds MaxPayload
    kNotConnected    = -2,  ///< Transport not running
    kQueueFull       = -3,  ///< TX queue is full (transient backpressure)
    kInvalidUtf8     = -4,  ///< Text frame payload is not valid UTF-8 (RFC 6455 §5.6)
    kInvalidCloseCode = -5, ///< Close status code is not valid per RFC 6455 §7.4
    kNullData         = -6, ///< data pointer is null but len > 0
};

/// Return a human-readable name for a SendError.
constexpr const char* send_error_name(SendError e) noexcept {
    switch (e) {
        case SendError::kOk:              return "OK";
        case SendError::kMessageTooLarge: return "MESSAGE_TOO_LARGE";
        case SendError::kNotConnected:    return "NOT_CONNECTED";
        case SendError::kQueueFull:       return "QUEUE_FULL";
        case SendError::kInvalidUtf8:     return "INVALID_UTF8";
        case SendError::kInvalidCloseCode: return "INVALID_CLOSE_CODE";
        case SendError::kNullData:         return "NULL_DATA";
    }
    return "UNKNOWN";
}

/// Check if a SendError indicates success (enables `if (!send(...))` pattern).
constexpr bool operator!(SendError e) noexcept {
    return e != SendError::kOk;
}

// ---------------------------------------------------------------------------
// Connection lifecycle events and state
// ---------------------------------------------------------------------------

/// Connection lifecycle events reported via the on_state_change callback.
enum class TransportEvent : uint8_t {
    kConnected,       ///< Initial connection or reconnection succeeded
    kDisconnected,    ///< Connection lost (before reconnect attempt)
    kReconnecting,    ///< Reconnect attempt starting (attempt number in detail)
    kStopped,         ///< Transport stopped (graceful or exhausted retries)
};

/// Current connection state (pollable via Transport::state()).
enum class TransportState : uint8_t {
    kConnected,       ///< Connection established and data can flow
    kReconnecting,    ///< Connection lost, reconnection in progress
    kStopped,         ///< Transport stopped (call stop() or exhausted retries)
};

/// Return a human-readable name for a TransportEvent.
constexpr const char* transport_event_name(TransportEvent e) noexcept {
    switch (e) {
        case TransportEvent::kConnected:    return "CONNECTED";
        case TransportEvent::kDisconnected: return "DISCONNECTED";
        case TransportEvent::kReconnecting: return "RECONNECTING";
        case TransportEvent::kStopped:      return "STOPPED";
    }
    return "UNKNOWN";
}

/// Return a human-readable name for a TransportState.
constexpr const char* transport_state_name(TransportState s) noexcept {
    switch (s) {
        case TransportState::kConnected:    return "CONNECTED";
        case TransportState::kReconnecting: return "RECONNECTING";
        case TransportState::kStopped:      return "STOPPED";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Callback type
// ---------------------------------------------------------------------------

/// Callback type for connection state changes.
/// @param event  The lifecycle event
/// @param detail Context string (e.g., error message, attempt count)
/// @warning Called from RX thread (or stop() caller). Must be non-blocking.
using TransportStateCallback =
    std::function<void(TransportEvent event, std::string_view detail)>;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct TransportConfig {
    // Connection target
    std::string remote_host{};      // Hostname for TLS SNI and HTTP Host
    uint16_t    remote_port = 443;  // Remote TCP port
    std::string ws_path     = "/";  // WebSocket upgrade path
    std::string ws_subprotocol{};   // WebSocket subprotocol (Sec-WebSocket-Protocol)
    std::string extra_headers{};    // Additional HTTP headers for upgrade

    // TLS
    bool        use_tls     = true; // false = plain ws:// (no TLS handshake/encryption)
    std::string ca_cert_path{};     // CA cert file, empty = system default
    bool        verify_peer = true;

    // Mutual TLS (mTLS) — client certificate authentication.
    // Both must be set together; empty = no client certificate.
    std::string client_cert_path{}; // Client certificate file (PEM)
    std::string client_key_path{};  // Client private key file (PEM)

    // Timeouts
    std::chrono::milliseconds tcp_timeout{3000};
    std::chrono::milliseconds tls_timeout{5000};
    std::chrono::milliseconds ws_timeout{3000};

    // Performance
    uint16_t tx_burst_size = 32;    // Max messages per TX drain batch
    uint16_t rx_burst_size = 32;    // Max packets per RX poll

    // Reconnection (exponential backoff with jitter, discard old messages)
    std::chrono::milliseconds reconnect_interval{100}; // Base interval (first retry)
    std::chrono::milliseconds max_reconnect_backoff{0}; // Max backoff cap (0 = 16x base)
    int max_reconnect_attempts = 10;                    // 0 = disable auto-reconnect

    // WebSocket ping (sent by TX thread at configured interval)
    std::chrono::seconds ping_interval{30};  // 0 = disable ping
    // Pong timeout: if no pong is received within this duration after a ping,
    // the connection is considered dead and a reconnect is triggered.
    // 0 = disable pong timeout detection (default for backward compatibility).
    std::chrono::seconds pong_timeout{0};

    // CPU affinity for worker threads (-1 = no pinning)
    int tx_cpu = -1;
    int rx_cpu = -1;

    // RX drop log throttling: log a warning every N drops (0 = disable logging).
    // The on_rx_drop callback is still invoked on every drop regardless.
    uint64_t drop_log_interval = 1000;

    // Connection state change callback (optional, called from worker threads)
    TransportStateCallback on_state_change{};

    /// Push-mode message callback (optional, called from RX thread).
    ///
    /// When set, incoming data messages are delivered directly to this
    /// callback from the RX thread instead of being enqueued to the
    /// rx_queue_. This eliminates the polling overhead of recv() and is
    /// ideal for event-driven architectures.
    ///
    /// @param data    Payload pointer (valid only during callback invocation)
    /// @param len     Payload length
    /// @param opcode  WebSocket opcode (kBinary, kText, etc.)
    ///
    /// @warning Called from the RX thread — must be non-blocking and
    ///          thread-safe with respect to the application thread.
    ///          Copy the data if you need it after the callback returns.
    std::function<void(const uint8_t* data, uint16_t len, uint8_t opcode)>
        on_message{};

    /// Close frame callback (optional, called from RX thread).
    ///
    /// When the server sends a WebSocket Close frame, this callback is
    /// invoked with the status code and reason before the transport begins
    /// its graceful shutdown sequence. Useful for logging, metrics, or
    /// deciding whether to reconnect.
    ///
    /// @param code    Close status code (e.g., 1000=Normal, 1001=GoingAway)
    /// @param reason  Human-readable reason (may be empty; valid only during callback)
    ///
    /// @warning Called from the RX thread — must be non-blocking.
    std::function<void(uint16_t code, std::string_view reason)> on_close{};

    /// Ping frame callback (optional, called from RX thread).
    ///
    /// Invoked when a WebSocket Ping frame is received from the server,
    /// before the automatic Pong response is enqueued. Useful for custom
    /// latency measurement or heartbeat monitoring.
    ///
    /// @param payload      Ping payload data (may be nullptr if empty)
    /// @param payload_len  Payload length (0–125 bytes per RFC 6455 §5.5)
    ///
    /// @warning Called from the RX thread — must be non-blocking.
    std::function<void(const uint8_t* payload, uint16_t payload_len)> on_ping{};

    /// Pong frame callback (optional, called from RX thread).
    ///
    /// Invoked when a WebSocket Pong frame is received (in response to a
    /// Ping sent by this transport or unsolicited). Enables round-trip
    /// latency measurement when paired with send_ping() timestamps.
    ///
    /// @param payload      Pong payload data (echo of the Ping payload)
    /// @param payload_len  Payload length
    ///
    /// @warning Called from the RX thread — must be non-blocking.
    std::function<void(const uint8_t* payload, uint16_t payload_len)> on_pong{};

    /// RX drop callback (optional, called from RX thread).
    ///
    /// Invoked on every RX queue drop (unlike the throttled log warning).
    /// Enables applications to detect backpressure and speed up consumption.
    ///
    /// @param total_dropped  Cumulative number of dropped messages
    ///
    /// @warning Called from the RX thread — must be non-blocking.
    std::function<void(uint64_t total_dropped)> on_rx_drop{};

    /// Reconnect attempt callback (optional, called from RX thread).
    ///
    /// Invoked after each failed reconnection attempt. Enables applications to
    /// observe reconnect failures (for metrics/alerting) and optionally abort
    /// the reconnect loop early — e.g., when the error indicates a non-transient
    /// failure such as TLS certificate rejection or HTTP 403.
    ///
    /// @param attempt       Current attempt number (1-based)
    /// @param max_attempts  Total configured attempts
    /// @param error         Error message from the failed attempt
    /// @return true to continue retrying, false to abort reconnection
    ///
    /// @warning Called from the RX thread — must be non-blocking.
    ///          If not set, all attempts proceed (equivalent to always returning true).
    std::function<bool(int attempt, int max_attempts, std::string_view error)>
        on_reconnect_attempt{};

    /// Reconnect success callback (optional, called from RX thread).
    ///
    /// Invoked after a successful reconnection. This is the ideal place to
    /// replay subscriptions, re-authenticate, or emit reconnection metrics.
    ///
    /// @param attempt       Number of attempts needed to reconnect (1-based)
    /// @param downtime_ns   Duration the connection was down (nanoseconds)
    /// @param total_reconnects  Cumulative reconnection count (lifetime)
    ///
    /// @warning Called from the RX thread — must be non-blocking.
    ///          The transport is fully connected when this fires; send() is safe.
    std::function<void(int attempt, uint64_t downtime_ns, uint64_t total_reconnects)>
        on_reconnected{};

    /// Multi-line formatted dump for logging/debugging.
    /// Callbacks are shown as set/unset (closures cannot be serialized).
    [[nodiscard]] std::string dump() const {
        return std::format(
            "TransportConfig:\n"
            "  target: {}:{}{}\n"
            "  subprotocol: {}\n"
            "  tls: use_tls={}, verify_peer={}, ca_cert={}, client_cert={}, client_key={}\n"
            "  timeouts: tcp={}ms, tls={}ms, ws={}ms\n"
            "  burst: tx={}, rx={}\n"
            "  reconnect: interval={}ms, max_backoff={}ms, max_attempts={}\n"
            "  ping: interval={}s, pong_timeout={}s\n"
            "  cpu: tx={}, rx={}\n"
            "  callbacks: on_state_change={}, on_message={}, on_close={}, "
            "on_ping={}, on_pong={}, on_rx_drop={}, on_reconnect_attempt={}, "
            "on_reconnected={}",
            remote_host, remote_port, ws_path,
            ws_subprotocol.empty() ? std::string_view("(none)") : std::string_view(ws_subprotocol),
            use_tls, verify_peer,
            ca_cert_path.empty() ? std::string_view("(system default)") : std::string_view(ca_cert_path),
            client_cert_path.empty() ? std::string_view("(none)") : std::string_view(client_cert_path),
            client_key_path.empty() ? std::string_view("(none)") : std::string_view(client_key_path),
            tcp_timeout.count(), tls_timeout.count(), ws_timeout.count(),
            tx_burst_size, rx_burst_size,
            reconnect_interval.count(), max_reconnect_backoff.count(),
            max_reconnect_attempts,
            ping_interval.count(), pong_timeout.count(),
            tx_cpu, rx_cpu,
            static_cast<bool>(on_state_change), static_cast<bool>(on_message),
            static_cast<bool>(on_close), static_cast<bool>(on_ping),
            static_cast<bool>(on_pong), static_cast<bool>(on_rx_drop),
            static_cast<bool>(on_reconnect_attempt),
            static_cast<bool>(on_reconnected));
    }

    /// JSON-formatted config for monitoring system integration.
    /// String fields are escaped per RFC 8259 §7 to prevent malformed output.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{"
            "\"remote_host\":\"{}\",\"remote_port\":{},\"ws_path\":\"{}\","
            "\"ws_subprotocol\":\"{}\","
            "\"use_tls\":{},\"verify_peer\":{},"
            "\"ca_cert_path\":\"{}\","
            "\"client_cert_path\":\"{}\",\"client_key_path\":\"{}\","
            "\"tcp_timeout_ms\":{},\"tls_timeout_ms\":{},\"ws_timeout_ms\":{},"
            "\"tx_burst_size\":{},\"rx_burst_size\":{},"
            "\"reconnect_interval_ms\":{},\"max_reconnect_backoff_ms\":{},"
            "\"max_reconnect_attempts\":{},"
            "\"ping_interval_s\":{},\"pong_timeout_s\":{},"
            "\"tx_cpu\":{},\"rx_cpu\":{},"
            "\"extra_headers\":\"{}\"}}",
            detail::json_escape(remote_host), remote_port,
            detail::json_escape(ws_path),
            detail::json_escape(ws_subprotocol),
            use_tls ? "true" : "false",
            verify_peer ? "true" : "false",
            detail::json_escape(ca_cert_path),
            detail::json_escape(client_cert_path),
            detail::json_escape(client_key_path),
            tcp_timeout.count(), tls_timeout.count(), ws_timeout.count(),
            tx_burst_size, rx_burst_size,
            reconnect_interval.count(), max_reconnect_backoff.count(),
            max_reconnect_attempts,
            ping_interval.count(), pong_timeout.count(),
            tx_cpu, rx_cpu,
            detail::json_escape(extra_headers));
    }

    /// Validate configuration, returning an error description or empty string on success.
    /// Call before Transport::create() to get early, actionable error messages.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (remote_host.empty())
            return "remote_host must not be empty";
        if (remote_port == 0)
            return "remote_port must be > 0";
        if (ws_path.empty())
            return "ws_path must not be empty";
        if (ws_path[0] != '/')
            return "ws_path must start with '/'";
        if (tx_burst_size == 0)
            return "tx_burst_size must be > 0";
        if (rx_burst_size == 0)
            return "rx_burst_size must be > 0";
        if (max_reconnect_attempts < 0)
            return "max_reconnect_attempts must be >= 0";
        if (client_cert_path.empty() != client_key_path.empty())
            return "client_cert_path and client_key_path must both be set or both empty";
        if (!client_cert_path.empty() && !use_tls)
            return "client certificates require use_tls=true";
        if (tcp_timeout.count() <= 0)
            return "tcp_timeout must be positive";
        if (tls_timeout.count() <= 0)
            return "tls_timeout must be positive";
        if (ws_timeout.count() <= 0)
            return "ws_timeout must be positive";
        if (max_reconnect_attempts > 0 && reconnect_interval.count() <= 0)
            return "reconnect_interval must be positive when auto-reconnect is enabled";
        if (ping_interval.count() < 0)
            return "ping_interval must be >= 0 (0 disables ping)";
        if (pong_timeout.count() < 0)
            return "pong_timeout must be >= 0 (0 disables pong timeout)";
        if (pong_timeout.count() > 0 && ping_interval.count() <= 0)
            return "pong_timeout requires ping_interval > 0";
        if (pong_timeout.count() > 0 && ping_interval.count() > 0 &&
            pong_timeout >= ping_interval)
            return "pong_timeout must be less than ping_interval "
                   "(otherwise timeout fires before next ping)";
        if (!extra_headers.empty()) {
            // HTTP headers must end with \r\n for correct framing
            if (extra_headers.size() < 2 ||
                extra_headers[extra_headers.size() - 2] != '\r' ||
                extra_headers[extra_headers.size() - 1] != '\n') {
                return "extra_headers must end with \\r\\n";
            }
        }
        // Sec-WebSocket-Protocol is interpolated directly into the HTTP
        // Upgrade request.  CR/LF in the value would allow HTTP header
        // injection (CWE-113), so reject them early.
        if (!ws_subprotocol.empty()) {
            for (char c : ws_subprotocol) {
                if (c == '\r' || c == '\n') {
                    return "ws_subprotocol must not contain CR or LF (header injection)";
                }
            }
        }
        return {};
    }

    /// Check for non-fatal contradictions or likely misconfigurations.
    /// Returns a list of warning messages (empty if no issues).
    /// Unlike validate() which blocks connection, these are advisory.
    [[nodiscard]] std::vector<std::string> warnings() const {
        std::vector<std::string> w;
        if (!use_tls && verify_peer)
            w.emplace_back("verify_peer=true has no effect when use_tls=false");
        if (!use_tls && !ca_cert_path.empty())
            w.emplace_back("ca_cert_path is set but use_tls=false — CA cert will be ignored");
        if (tx_burst_size > static_cast<uint16_t>(1024))
            w.emplace_back(std::format(
                "tx_burst_size={} is unusually large — may increase "
                "per-iteration latency variance", tx_burst_size));
        if (rx_burst_size > static_cast<uint16_t>(1024))
            w.emplace_back(std::format(
                "rx_burst_size={} is unusually large — may increase "
                "per-iteration latency variance", rx_burst_size));
        return w;
    }

    /// Parse a WebSocket URL into a TransportConfig.
    ///
    /// Supported URL forms:
    ///   wss://host/path            (port=443, use_tls=true)
    ///   ws://host/path             (port=80,  use_tls=false)
    ///   wss://host:port/path       (explicit port)
    ///   wss://host                 (path defaults to "/")
    ///   wss://host:port/path?query (query is included in ws_path)
    ///
    /// Only sets remote_host, remote_port, ws_path, and use_tls.
    /// All other config fields retain their default values and can be
    /// modified after construction.
    ///
    /// @param url  WebSocket URL string
    /// @return TransportConfig on success, or error description
    [[nodiscard]] static std::expected<TransportConfig, std::string>
    from_url(std::string_view url) {
        // Strip leading/trailing whitespace
        while (!url.empty() && (url.front() == ' ' || url.front() == '\t'))
            url.remove_prefix(1);
        while (!url.empty() && (url.back() == ' ' || url.back() == '\t'))
            url.remove_suffix(1);

        TransportConfig cfg;

        // Parse scheme
        if (url.starts_with("wss://")) {
            cfg.use_tls = true;
            cfg.remote_port = 443;
            url.remove_prefix(6);
        } else if (url.starts_with("ws://")) {
            cfg.use_tls = false;
            cfg.remote_port = 80;
            url.remove_prefix(5);
        } else {
            return std::unexpected("URL must start with ws:// or wss://");
        }

        if (url.empty()) {
            return std::unexpected("URL missing host");
        }

        // IPv6 bracket notation: wss://[::1]:port/path
        if (url.front() == '[') {
            size_t bracket_end = url.find(']');
            if (bracket_end == std::string_view::npos) {
                return std::unexpected("IPv6 address missing closing ']'");
            }
            cfg.remote_host = std::string(url.substr(1, bracket_end - 1));
            if (cfg.remote_host.empty()) {
                return std::unexpected("URL has empty IPv6 address");
            }
            url.remove_prefix(bracket_end + 1);
        } else {
            // Regular hostname: find boundary at first '/' or ':' or end
            size_t host_end = url.find_first_of(":/");
            if (host_end == std::string_view::npos) {
                cfg.remote_host = std::string(url);
                cfg.ws_path = "/";
                return cfg;
            }
            cfg.remote_host = std::string(url.substr(0, host_end));
            if (cfg.remote_host.empty()) {
                return std::unexpected("URL has empty host");
            }
            url.remove_prefix(host_end);
        }

        // Reject control characters in hostname (CWE-93 header injection)
        for (char c : cfg.remote_host) {
            if (c < 0x20 || c == 0x7f) {
                return std::unexpected(
                    "hostname contains control characters");
            }
        }

        // Parse optional port
        if (!url.empty() && url.front() == ':') {
            url.remove_prefix(1); // skip ':'
            size_t port_end = url.find('/');
            std::string_view port_str = (port_end == std::string_view::npos)
                ? url : url.substr(0, port_end);

            if (port_str.empty()) {
                return std::unexpected("URL has empty port after ':'");
            }

            // Parse as uint32_t first to detect overflow beyond uint16_t
            uint32_t port32 = 0;
            auto [ptr, ec] = std::from_chars(
                port_str.data(), port_str.data() + port_str.size(), port32);
            if (ec != std::errc{} || ptr != port_str.data() + port_str.size()) {
                return std::unexpected(
                    std::format("invalid port: '{}'", port_str));
            }
            if (port32 == 0 || port32 > 65535) {
                return std::unexpected(
                    std::format("port out of range: {}", port32));
            }
            cfg.remote_port = static_cast<uint16_t>(port32);

            if (port_end == std::string_view::npos) {
                cfg.ws_path = "/";
                return cfg;
            }
            url.remove_prefix(port_end);
        }

        // Remaining is path (+ optional query/fragment)
        if (url.empty()) {
            cfg.ws_path = "/";
        } else {
            cfg.ws_path = std::string(url);
        }

        return cfg;
    }

    /// Serialize the connection target as a WebSocket URL.
    ///
    /// Inverse of from_url(): reconstructs a URL from remote_host,
    /// remote_port, ws_path, and use_tls. Omits the port when it
    /// matches the scheme default (443 for wss, 80 for ws).
    ///
    /// @return URL string like "wss://host:port/path"
    [[nodiscard]] std::string to_url() const {
        std::string_view scheme = use_tls ? "wss" : "ws";
        uint16_t default_port = use_tls ? 443 : 80;
        // IPv6 addresses must be bracket-enclosed in URLs
        bool is_ipv6 = remote_host.find(':') != std::string::npos;
        std::string host_part = is_ipv6
            ? std::format("[{}]", remote_host)
            : remote_host;
        if (remote_port == default_port) {
            return std::format("{}://{}{}", scheme, host_part, ws_path);
        }
        return std::format("{}://{}:{}{}", scheme, host_part,
                           remote_port, ws_path);
    }
};

// ---------------------------------------------------------------------------
// Connection info (aggregated metadata from the current connection)
// ---------------------------------------------------------------------------

/// Snapshot of connection metadata, aggregating information that would
/// otherwise require multiple getter calls (tls_version, cipher_name,
/// remote_ip, ws_subprotocol). Useful for logging/monitoring dashboards.
struct ConnectionInfo {
    std::string tls_version;       ///< e.g. "TLSv1.3" or "none"
    std::string cipher_name;       ///< e.g. "TLS_AES_256_GCM_SHA384" or "none"
    std::string ws_subprotocol;    ///< Negotiated subprotocol, or empty
    std::string remote_ip;         ///< Resolved IP, or empty
    uint16_t    remote_port = 0;   ///< Remote port from config
    bool        use_tls = true;    ///< Whether TLS is enabled

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        return std::format(
            "ConnectionInfo:\n"
            "  remote: {}:{}\n"
            "  tls: {} ({})\n"
            "  subprotocol: {}",
            remote_ip.empty() ? "unknown" : remote_ip, remote_port,
            tls_version, cipher_name,
            ws_subprotocol.empty() ? "(none)" : ws_subprotocol);
    }

    /// JSON-formatted connection info.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{"
            "\"tls_version\":\"{}\",\"cipher_name\":\"{}\","
            "\"ws_subprotocol\":\"{}\",\"remote_ip\":\"{}\","
            "\"remote_port\":{},\"use_tls\":{}}}",
            detail::json_escape(tls_version),
            detail::json_escape(cipher_name),
            detail::json_escape(ws_subprotocol),
            detail::json_escape(remote_ip),
            remote_port,
            use_tls ? "true" : "false");
    }
};

// ---------------------------------------------------------------------------
// Transport stats
// ---------------------------------------------------------------------------

/// Round-trip time statistics from WebSocket ping/pong measurements.
/// All values are in nanoseconds. Zero count means no RTT data collected.
struct RttStats {
    uint64_t count  = 0;    ///< Number of RTT samples recorded
    uint64_t min_ns = 0;    ///< Minimum RTT (ns)
    uint64_t max_ns = 0;    ///< Maximum RTT (ns)
    double   mean_ns = 0.0; ///< Mean RTT (ns)
    uint64_t p50_ns = 0;    ///< Median RTT (ns)
    uint64_t p99_ns = 0;    ///< 99th percentile RTT (ns)
    uint64_t p999_ns = 0;   ///< 99.9th percentile RTT (ns)

    /// Minimum RTT in microseconds.
    [[nodiscard]] double min_us() const noexcept {
        return static_cast<double>(min_ns) / 1e3;
    }
    /// Maximum RTT in microseconds.
    [[nodiscard]] double max_us() const noexcept {
        return static_cast<double>(max_ns) / 1e3;
    }
    /// Median RTT in microseconds (convenience for human-readable output).
    [[nodiscard]] double p50_us() const noexcept {
        return static_cast<double>(p50_ns) / 1e3;
    }
    /// 99th percentile RTT in microseconds.
    [[nodiscard]] double p99_us() const noexcept {
        return static_cast<double>(p99_ns) / 1e3;
    }
    /// 99.9th percentile RTT in microseconds.
    [[nodiscard]] double p999_us() const noexcept {
        return static_cast<double>(p999_ns) / 1e3;
    }
    /// Mean RTT in microseconds.
    [[nodiscard]] double mean_us() const noexcept {
        return mean_ns / 1e3;
    }
    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        if (count == 0) return "RttStats: no samples";
        return std::format(
            "RttStats ({} samples):\n"
            "  min: {:.1f}us, p50: {:.1f}us, p99: {:.1f}us, "
            "p999: {:.1f}us, max: {:.1f}us, mean: {:.1f}us",
            count, min_us(), p50_us(), p99_us(),
            p999_us(), max_us(), mean_us());
    }

    /// JSON-formatted RTT stats for monitoring system integration.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{"
            "\"count\":{},\"min_ns\":{},\"max_ns\":{},"
            "\"mean_ns\":{:.1f},\"p50_ns\":{},\"p99_ns\":{},\"p999_ns\":{},"
            "\"p50_us\":{:.3f},\"p99_us\":{:.3f},\"mean_us\":{:.3f}}}",
            count, min_ns, max_ns,
            mean_ns, p50_ns, p99_ns, p999_ns,
            p50_us(), p99_us(), mean_us());
    }
};

/// Per-thread stats — TX thread and RX thread each own their own counters.
/// Merged at query time via stats().
///
/// Fields use std::atomic with relaxed ordering to avoid undefined behavior
/// when the application thread reads stats while worker threads write them.
/// On x86_64, relaxed atomics compile to plain loads/stores (zero overhead).
struct ThreadStats {
    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> text_packets{0};  ///< Text frame count (subset of packets)
    std::atomic<uint64_t> text_bytes{0};    ///< Text frame bytes (subset of bytes)
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> crypto_errors{0};

    /// Reset all counters to zero (call from one thread only).
    void reset() noexcept {
        packets.store(0, std::memory_order_relaxed);
        bytes.store(0, std::memory_order_relaxed);
        text_packets.store(0, std::memory_order_relaxed);
        text_bytes.store(0, std::memory_order_relaxed);
        dropped.store(0, std::memory_order_relaxed);
        crypto_errors.store(0, std::memory_order_relaxed);
    }
};

/// Aggregated transport statistics (returned by stats()).
struct TransportStats {
    uint64_t tx_packets        = 0;
    uint64_t tx_bytes          = 0;
    uint64_t tx_text_packets   = 0;  ///< Text frames sent (subset of tx_packets)
    uint64_t tx_text_bytes     = 0;  ///< Text frame bytes sent (subset of tx_bytes)
    uint64_t tx_dropped        = 0;
    uint64_t rx_packets        = 0;
    uint64_t rx_bytes          = 0;
    uint64_t rx_text_packets   = 0;  ///< Text frames received (subset of rx_packets)
    uint64_t rx_text_bytes     = 0;  ///< Text frame bytes received (subset of rx_bytes)
    uint64_t rx_dropped        = 0;
    uint64_t encrypt_errors    = 0;
    uint64_t decrypt_errors    = 0;
    uint64_t queue_full_count  = 0;
    uint64_t ws_pings_received = 0;
    uint64_t ws_pongs_sent     = 0;
    uint64_t pong_timeouts     = 0;
    uint64_t reconnect_count   = 0;
    size_t   tx_queue_hwm      = 0;  ///< Peak TX queue occupancy since last reset
    size_t   rx_queue_hwm      = 0;  ///< Peak RX queue occupancy since last reset
    uint64_t uptime_ns         = 0;  ///< Nanoseconds since Transport::create()
    uint64_t handshake_ns      = 0;  ///< Last TCP+TLS+WS handshake duration (ns)
    uint64_t tcp_connect_ns    = 0;  ///< Last TCP connect (factory) duration (ns)
    uint64_t tls_handshake_ns  = 0;  ///< Last TLS handshake duration (ns), 0 if no TLS
    uint64_t ws_upgrade_ns     = 0;  ///< Last WebSocket upgrade duration (ns)
    std::string remote_ip{};         ///< Resolved remote IP of current connection
    RttStats    rtt{};               ///< Round-trip time statistics from ping/pong
    uint64_t tls_write_seq     = 0;  ///< Current TLS write sequence number
    uint64_t tls_read_seq      = 0;  ///< Current TLS read sequence number
    uint64_t tls_seq_limit     = 0;  ///< TLS sequence limit (kMaxSequenceNumber)

    /// Last handshake duration in milliseconds (for human-readable logging).
    [[nodiscard]] double handshake_ms() const noexcept {
        return static_cast<double>(handshake_ns) / 1e6;
    }

    /// TLS write sequence usage as a fraction [0.0, 1.0].
    /// Useful for monitoring: values approaching 1.0 indicate an imminent
    /// reconnect for key refresh.
    [[nodiscard]] double tls_write_seq_usage() const noexcept {
        return tls_seq_limit > 0
            ? static_cast<double>(tls_write_seq) / static_cast<double>(tls_seq_limit)
            : 0.0;
    }

    /// TLS read sequence usage as a fraction [0.0, 1.0].
    [[nodiscard]] double tls_read_seq_usage() const noexcept {
        return tls_seq_limit > 0
            ? static_cast<double>(tls_read_seq) / static_cast<double>(tls_seq_limit)
            : 0.0;
    }

    // -----------------------------------------------------------------------
    // Rate helpers — compute averages over uptime for monitoring dashboards.
    // Return 0.0 if uptime_ns == 0 to avoid division by zero.
    // -----------------------------------------------------------------------

    /// TX packets per second (average over uptime).
    [[nodiscard]] double tx_pps() const noexcept {
        return uptime_ns > 0
            ? static_cast<double>(tx_packets) * 1e9 / static_cast<double>(uptime_ns)
            : 0.0;
    }

    /// RX packets per second (average over uptime).
    [[nodiscard]] double rx_pps() const noexcept {
        return uptime_ns > 0
            ? static_cast<double>(rx_packets) * 1e9 / static_cast<double>(uptime_ns)
            : 0.0;
    }

    /// TX bytes per second (average over uptime).
    [[nodiscard]] double tx_bps() const noexcept {
        return uptime_ns > 0
            ? static_cast<double>(tx_bytes) * 1e9 / static_cast<double>(uptime_ns)
            : 0.0;
    }

    /// RX bytes per second (average over uptime).
    [[nodiscard]] double rx_bps() const noexcept {
        return uptime_ns > 0
            ? static_cast<double>(rx_bytes) * 1e9 / static_cast<double>(uptime_ns)
            : 0.0;
    }

    /// Uptime as a chrono duration.
    [[nodiscard]] std::chrono::nanoseconds uptime() const noexcept {
        return std::chrono::nanoseconds{uptime_ns};
    }

    /// Compute the delta between two snapshots for windowed metrics.
    ///
    /// Usage:
    ///   auto s1 = transport.stats();
    ///   // ... wait ...
    ///   auto s2 = transport.stats();
    ///   auto delta = s2 - s1;  // delta.tx_pps() gives window-average rate
    ///
    /// Counter fields (packets, bytes, errors) are subtracted.
    /// HWM fields take the later snapshot's value.
    /// Connection metadata (remote_ip, TLS seq, RTT, handshake timings)
    /// comes from the later snapshot (rhs is 'this', lhs is 'earlier').
    [[nodiscard]] TransportStats operator-(const TransportStats& earlier) const noexcept {
        TransportStats d;
        d.tx_packets        = tx_packets        - earlier.tx_packets;
        d.tx_bytes          = tx_bytes          - earlier.tx_bytes;
        d.tx_text_packets   = tx_text_packets   - earlier.tx_text_packets;
        d.tx_text_bytes     = tx_text_bytes     - earlier.tx_text_bytes;
        d.tx_dropped        = tx_dropped        - earlier.tx_dropped;
        d.rx_packets        = rx_packets        - earlier.rx_packets;
        d.rx_bytes          = rx_bytes          - earlier.rx_bytes;
        d.rx_text_packets   = rx_text_packets   - earlier.rx_text_packets;
        d.rx_text_bytes     = rx_text_bytes     - earlier.rx_text_bytes;
        d.rx_dropped        = rx_dropped        - earlier.rx_dropped;
        d.encrypt_errors    = encrypt_errors    - earlier.encrypt_errors;
        d.decrypt_errors    = decrypt_errors    - earlier.decrypt_errors;
        d.queue_full_count  = queue_full_count  - earlier.queue_full_count;
        d.ws_pings_received = ws_pings_received - earlier.ws_pings_received;
        d.ws_pongs_sent     = ws_pongs_sent     - earlier.ws_pongs_sent;
        d.pong_timeouts     = pong_timeouts     - earlier.pong_timeouts;
        d.reconnect_count   = reconnect_count   - earlier.reconnect_count;
        // HWM: take current snapshot values (not delta)
        d.tx_queue_hwm      = tx_queue_hwm;
        d.rx_queue_hwm      = rx_queue_hwm;
        // Uptime delta = window duration
        d.uptime_ns         = uptime_ns         - earlier.uptime_ns;
        // Connection metadata from current snapshot
        d.handshake_ns      = handshake_ns;
        d.tcp_connect_ns    = tcp_connect_ns;
        d.tls_handshake_ns  = tls_handshake_ns;
        d.ws_upgrade_ns     = ws_upgrade_ns;
        d.remote_ip         = remote_ip;
        d.rtt               = rtt;
        d.tls_write_seq     = tls_write_seq;
        d.tls_read_seq      = tls_read_seq;
        d.tls_seq_limit     = tls_seq_limit;
        return d;
    }

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        double uptime_s = static_cast<double>(uptime_ns) / 1e9;
        return std::format(
            "TransportStats (uptime: {:.1f}s, remote: {}):\n"
            "  TX: {} packets ({:.0f}/s), {} bytes ({:.0f} B/s), "
            "text: {} pkts/{} B, {} dropped, {} encrypt errors\n"
            "  RX: {} packets ({:.0f}/s), {} bytes ({:.0f} B/s), "
            "text: {} pkts/{} B, {} dropped, {} decrypt errors\n"
            "  Queue full: {}, TX HWM: {}, RX HWM: {}\n"
            "  WebSocket: {} pings received, {} pongs sent, {} pong timeouts\n"
            "  Reconnections: {}, handshake: {:.1f}ms "
            "(tcp: {:.1f}ms, tls: {:.1f}ms, ws: {:.1f}ms)\n"
            "  TLS seq: write={}/{} ({:.1f}%), read={}/{} ({:.1f}%)\n"
            "  {}",
            uptime_s, remote_ip.empty() ? "unknown" : remote_ip,
            tx_packets, tx_pps(), tx_bytes, tx_bps(),
            tx_text_packets, tx_text_bytes, tx_dropped, encrypt_errors,
            rx_packets, rx_pps(), rx_bytes, rx_bps(),
            rx_text_packets, rx_text_bytes, rx_dropped, decrypt_errors,
            queue_full_count, tx_queue_hwm, rx_queue_hwm,
            ws_pings_received, ws_pongs_sent, pong_timeouts,
            reconnect_count, handshake_ms(),
            static_cast<double>(tcp_connect_ns) / 1e6,
            static_cast<double>(tls_handshake_ns) / 1e6,
            static_cast<double>(ws_upgrade_ns) / 1e6,
            tls_write_seq, tls_seq_limit, tls_write_seq_usage() * 100.0,
            tls_read_seq, tls_seq_limit, tls_read_seq_usage() * 100.0,
            rtt.dump());
    }

    /// JSON-formatted stats for monitoring system integration.
    /// No external JSON library dependency — hand-rolled for zero overhead.
    /// String fields are escaped per RFC 8259 §7.
    [[nodiscard]] std::string to_json() const {
        return std::format(
            "{{"
            "\"tx_packets\":{},\"tx_bytes\":{},\"tx_text_packets\":{},"
            "\"tx_text_bytes\":{},\"tx_dropped\":{},"
            "\"rx_packets\":{},\"rx_bytes\":{},\"rx_text_packets\":{},"
            "\"rx_text_bytes\":{},\"rx_dropped\":{},"
            "\"encrypt_errors\":{},\"decrypt_errors\":{},"
            "\"queue_full_count\":{},\"tx_queue_hwm\":{},\"rx_queue_hwm\":{},"
            "\"ws_pings_received\":{},"
            "\"ws_pongs_sent\":{},\"pong_timeouts\":{},"
            "\"reconnect_count\":{},\"uptime_ns\":{},"
            "\"handshake_ns\":{},\"tcp_connect_ns\":{},\"tls_handshake_ns\":{},"
            "\"ws_upgrade_ns\":{},\"handshake_ms\":{:.3f},"
            "\"tx_pps\":{:.1f},\"rx_pps\":{:.1f},"
            "\"tx_bps\":{:.1f},\"rx_bps\":{:.1f},"
            "\"remote_ip\":\"{}\","
            "\"tls_write_seq\":{},\"tls_read_seq\":{},\"tls_seq_limit\":{},"
            "\"tls_write_seq_usage\":{:.6f},\"tls_read_seq_usage\":{:.6f},"
            "\"rtt\":{}}}",
            tx_packets, tx_bytes, tx_text_packets,
            tx_text_bytes, tx_dropped,
            rx_packets, rx_bytes, rx_text_packets,
            rx_text_bytes, rx_dropped,
            encrypt_errors, decrypt_errors,
            queue_full_count, tx_queue_hwm, rx_queue_hwm,
            ws_pings_received,
            ws_pongs_sent, pong_timeouts,
            reconnect_count, uptime_ns,
            handshake_ns, tcp_connect_ns, tls_handshake_ns,
            ws_upgrade_ns, handshake_ms(),
            tx_pps(), rx_pps(),
            tx_bps(), rx_bps(),
            detail::json_escape(remote_ip),
            tls_write_seq, tls_read_seq, tls_seq_limit,
            tls_write_seq_usage(), tls_read_seq_usage(),
            rtt.to_json());
    }
};

} // namespace eph::net

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations
// ─────────────────────────────────────────────────────────────────────────────

template <>
struct std::formatter<eph::net::RttStats> : std::formatter<std::string> {
    auto format(const eph::net::RttStats& r, auto& ctx) const {
        if (r.count == 0) {
            return std::formatter<std::string>::format("RTT: no samples", ctx);
        }
        return std::formatter<std::string>::format(
            std::format("RTT(n={}): p50={:.0f}us p99={:.0f}us max={:.0f}us",
                r.count, r.p50_us(), r.p99_us(),
                static_cast<double>(r.max_ns) / 1e3),
            ctx);
    }
};

template <>
struct std::formatter<eph::net::SendError> : std::formatter<const char*> {
    auto format(eph::net::SendError e, auto& ctx) const {
        return std::formatter<const char*>::format(
            eph::net::send_error_name(e), ctx);
    }
};

template <>
struct std::formatter<eph::net::TransportEvent> : std::formatter<const char*> {
    auto format(eph::net::TransportEvent e, auto& ctx) const {
        return std::formatter<const char*>::format(
            eph::net::transport_event_name(e), ctx);
    }
};

template <>
struct std::formatter<eph::net::TransportState> : std::formatter<const char*> {
    auto format(eph::net::TransportState s, auto& ctx) const {
        return std::formatter<const char*>::format(
            eph::net::transport_state_name(s), ctx);
    }
};

template <>
struct std::formatter<eph::net::ConnectionError> : std::formatter<const char*> {
    auto format(eph::net::ConnectionError e, auto& ctx) const {
        return std::formatter<const char*>::format(
            eph::net::connection_error_name(e), ctx);
    }
};

template <>
struct std::formatter<eph::net::ConnectionErrorInfo> : std::formatter<std::string> {
    auto format(const eph::net::ConnectionErrorInfo& e, auto& ctx) const {
        return std::formatter<std::string>::format(e.message(), ctx);
    }
};

template <>
struct std::formatter<eph::net::ConnectionInfo> : std::formatter<std::string> {
    auto format(const eph::net::ConnectionInfo& c, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format("{}:{} tls={} version={} cipher={} subproto={}",
                c.remote_ip.empty() ? "unknown" : c.remote_ip,
                c.remote_port,
                c.use_tls ? "true" : "false",
                c.tls_version.empty() ? "none" : c.tls_version,
                c.cipher_name.empty() ? "none" : c.cipher_name,
                c.ws_subprotocol.empty() ? "(none)" : c.ws_subprotocol),
            ctx);
    }
};

template <>
struct std::formatter<eph::net::TransportStats> : std::formatter<std::string> {
    auto format(const eph::net::TransportStats& s, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format(
                "TX: {}pkts/{}B (dropped:{}, encrypt_err:{}) | "
                "RX: {}pkts/{}B (dropped:{}, decrypt_err:{}) | "
                "queue_full:{} ping:{} pong:{} pong_timeout:{} reconnect:{}"
                " tls_seq:{}/{}({:.0f}%) remote:{}",
                s.tx_packets, s.tx_bytes, s.tx_dropped, s.encrypt_errors,
                s.rx_packets, s.rx_bytes, s.rx_dropped, s.decrypt_errors,
                s.queue_full_count, s.ws_pings_received,
                s.ws_pongs_sent, s.pong_timeouts, s.reconnect_count,
                s.tls_write_seq, s.tls_seq_limit,
                s.tls_write_seq_usage() * 100.0,
                s.remote_ip.empty() ? "unknown" : s.remote_ip),
            ctx);
    }
};
