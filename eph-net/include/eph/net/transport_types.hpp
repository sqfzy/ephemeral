#pragma once

/// @file transport_types.hpp
/// Public types for the Transport layer — enums, config, stats, formatters.
///
/// This header is deliberately lightweight: it depends only on the standard
/// library and tcp_concept.hpp, so downstream code (including DPDK backends)
/// can use these types without pulling in TLS, WebSocket, or SPSC queue
/// headers.

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace eph::net {

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
    std::string ca_cert_path{};     // CA cert file, empty = system default
    bool        verify_peer = true;

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

    /// Validate configuration, returning an error description or empty string on success.
    /// Call before Transport::create() to get early, actionable error messages.
    [[nodiscard]] constexpr std::string_view validate() const noexcept {
        if (remote_host.empty())
            return "remote_host must not be empty";
        if (remote_port == 0)
            return "remote_port must be > 0";
        if (ws_path.empty())
            return "ws_path must not be empty";
        if (tx_burst_size == 0)
            return "tx_burst_size must be > 0";
        if (rx_burst_size == 0)
            return "rx_burst_size must be > 0";
        if (max_reconnect_attempts < 0)
            return "max_reconnect_attempts must be >= 0";
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
        if (!extra_headers.empty()) {
            // HTTP headers must end with \r\n for correct framing
            if (extra_headers.size() < 2 ||
                extra_headers[extra_headers.size() - 2] != '\r' ||
                extra_headers[extra_headers.size() - 1] != '\n') {
                return "extra_headers must end with \\r\\n";
            }
        }
        return {};
    }
};

// ---------------------------------------------------------------------------
// Transport stats
// ---------------------------------------------------------------------------

/// Per-thread stats -- TX thread and RX thread each own their own counters.
/// Merged at query time to avoid atomic contention on the hot path.
struct ThreadStats {
    uint64_t packets       = 0;
    uint64_t bytes         = 0;
    uint64_t dropped       = 0;
    uint64_t crypto_errors = 0;
};

/// Aggregated transport statistics (returned by stats()).
struct TransportStats {
    uint64_t tx_packets        = 0;
    uint64_t tx_bytes          = 0;
    uint64_t tx_dropped        = 0;
    uint64_t rx_packets        = 0;
    uint64_t rx_bytes          = 0;
    uint64_t rx_dropped        = 0;
    uint64_t encrypt_errors    = 0;
    uint64_t decrypt_errors    = 0;
    uint64_t queue_full_count  = 0;
    uint64_t ws_pings_received = 0;
    uint64_t ws_pongs_sent     = 0;
    uint64_t pong_timeouts     = 0;
    uint64_t reconnect_count   = 0;
    uint64_t uptime_ns         = 0;  ///< Nanoseconds since Transport::create()
    uint64_t handshake_ns      = 0;  ///< Last TCP+TLS+WS handshake duration (ns)

    /// Last handshake duration in milliseconds (for human-readable logging).
    [[nodiscard]] double handshake_ms() const noexcept {
        return static_cast<double>(handshake_ns) / 1e6;
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

    /// Multi-line formatted dump for logging/debugging.
    [[nodiscard]] std::string dump() const {
        double uptime_s = static_cast<double>(uptime_ns) / 1e9;
        return std::format(
            "TransportStats (uptime: {:.1f}s):\n"
            "  TX: {} packets ({:.0f}/s), {} bytes ({:.0f} B/s), {} dropped, {} encrypt errors\n"
            "  RX: {} packets ({:.0f}/s), {} bytes ({:.0f} B/s), {} dropped, {} decrypt errors\n"
            "  Queue full: {}\n"
            "  WebSocket: {} pings received, {} pongs sent, {} pong timeouts\n"
            "  Reconnections: {}",
            uptime_s,
            tx_packets, tx_pps(), tx_bytes, tx_bps(), tx_dropped, encrypt_errors,
            rx_packets, rx_pps(), rx_bytes, rx_bps(), rx_dropped, decrypt_errors,
            queue_full_count,
            ws_pings_received, ws_pongs_sent, pong_timeouts,
            reconnect_count);
    }
};

} // namespace eph::net

// ─────────────────────────────────────────────────────────────────────────────
// std::formatter specializations
// ─────────────────────────────────────────────────────────────────────────────

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
struct std::formatter<eph::net::TransportStats> : std::formatter<std::string> {
    auto format(const eph::net::TransportStats& s, auto& ctx) const {
        return std::formatter<std::string>::format(
            std::format(
                "TX: {}pkts/{}B (dropped:{}, encrypt_err:{}) | "
                "RX: {}pkts/{}B (dropped:{}, decrypt_err:{}) | "
                "queue_full:{} ping:{} pong:{} pong_timeout:{} reconnect:{}",
                s.tx_packets, s.tx_bytes, s.tx_dropped, s.encrypt_errors,
                s.rx_packets, s.rx_bytes, s.rx_dropped, s.decrypt_errors,
                s.queue_full_count, s.ws_pings_received,
                s.ws_pongs_sent, s.pong_timeouts, s.reconnect_count),
            ctx);
    }
};
