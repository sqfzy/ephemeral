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
    /// Invoked when a received data message is dropped because the RX queue
    /// is full. Enables applications to detect backpressure and speed up
    /// consumption. Called at most once per 1000 drops to avoid log flooding.
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

    /// Median RTT in microseconds (convenience for human-readable output).
    [[nodiscard]] double p50_us() const noexcept {
        return static_cast<double>(p50_ns) / 1e3;
    }
    /// 99th percentile RTT in microseconds.
    [[nodiscard]] double p99_us() const noexcept {
        return static_cast<double>(p99_ns) / 1e3;
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
            count,
            static_cast<double>(min_ns) / 1e3,
            p50_us(), p99_us(),
            static_cast<double>(p999_ns) / 1e3,
            static_cast<double>(max_ns) / 1e3,
            mean_us());
    }
};

/// Per-thread stats -- TX thread and RX thread each own their own counters.
/// Merged at query time to avoid atomic contention on the hot path.
struct ThreadStats {
    uint64_t packets       = 0;
    uint64_t bytes         = 0;
    uint64_t text_packets  = 0;  ///< Text frame count (subset of packets)
    uint64_t text_bytes    = 0;  ///< Text frame bytes (subset of bytes)
    uint64_t dropped       = 0;
    uint64_t crypto_errors = 0;
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
            "  TX: {} packets ({:.0f}/s), {} bytes ({:.0f} B/s), "
            "text: {} pkts/{} B, {} dropped, {} encrypt errors\n"
            "  RX: {} packets ({:.0f}/s), {} bytes ({:.0f} B/s), "
            "text: {} pkts/{} B, {} dropped, {} decrypt errors\n"
            "  Queue full: {}\n"
            "  WebSocket: {} pings received, {} pongs sent, {} pong timeouts\n"
            "  Reconnections: {}",
            uptime_s,
            tx_packets, tx_pps(), tx_bytes, tx_bps(),
            tx_text_packets, tx_text_bytes, tx_dropped, encrypt_errors,
            rx_packets, rx_pps(), rx_bytes, rx_bps(),
            rx_text_packets, rx_text_bytes, rx_dropped, decrypt_errors,
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
