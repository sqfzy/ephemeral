#pragma once

/// @file transport.hpp
/// Generic WebSocket transport over any TcpTransport backend.
///
/// Provides a single entry point for establishing a WSS connection
/// and sending/receiving data with minimal latency.
///
/// Architecture:
///   Control thread (handshake):
///     TCP connect -> TLS 1.3 handshake -> WebSocket Upgrade
///   Data plane (hot path):
///     app send() -> SPSC queue -> TX thread -> WS frame -> TLS encrypt ->
///     TCP send
///
/// Thread model:
///   - Application thread: calls send()/recv(), non-blocking
///   - TX thread: busy-poll SPSC queue, build packets, send
///   - RX thread: busy-poll TCP rx, decrypt, parse WS frames, push to recv queue

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>


#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/utils/alignment.hpp"
#include "eph/containers/bounded_queue.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/record.hpp"
#include "eph/net/http.hpp"
#include "eph/net/tcp_concept.hpp"
#include "eph/net/tls_record.hpp"
#include "eph/net/tls_session.hpp"
#include "eph/net/transport_types.hpp"
#include "eph/net/websocket.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// Internal message types for SPSC queue
// ---------------------------------------------------------------------------

namespace detail {

/// Message passed from application thread to TX thread via SPSC queue.
/// Fixed-size to satisfy TrivialData constraint.
template <size_t MaxPayload>
struct alignas(eph::utils::CACHE_LINE_SIZE) TxMessage {
    uint8_t  data[MaxPayload]{};
    uint16_t len = 0;
    uint8_t  opcode = ws::opcode::kBinary;

    static_assert(MaxPayload > 0, "MaxPayload must be > 0");
    static_assert(MaxPayload <= tls_const::kMaxRecordPayload,
                  "MaxPayload exceeds TLS max record size");
};

/// Message passed from RX processing to application via SPSC queue.
template <size_t MaxPayload>
struct alignas(eph::utils::CACHE_LINE_SIZE) RxMessage {
    uint8_t  data[MaxPayload]{};
    uint16_t len = 0;
    uint8_t  opcode = ws::opcode::kBinary;
};

inline std::shared_ptr<spdlog::logger> transport_logger() {
    static auto l = [] {
        auto lg = spdlog::get("net.transport");
        if (!lg) lg = spdlog::stdout_color_mt("net.transport");
        // Inherit level from spdlog global default
        return lg;
    }();
    return l;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Transport -- public API
// ---------------------------------------------------------------------------

/// Generic WebSocket transport with TLS 1.3 encryption.
///
/// Template parameters:
///   TcpImpl    -- a type satisfying the TcpTransport concept
///   MaxPayload -- maximum application payload size per message
///   QueueDepth -- SPSC queue capacity (must be power of 2)
///
/// Usage:
///   auto factory = [&]() -> std::expected<std::unique_ptr<MyTcp>, std::string> {
///       auto tcp = std::make_unique<MyTcp>(my_config);
///       auto r = tcp->connect(std::chrono::milliseconds{3000});
///       if (!r) return std::unexpected(r.error());
///       return tcp;
///   };
///   auto result = Transport<MyTcp>::create(std::move(factory), config);
///   if (!result) { /* handle error */ }
///   auto& transport = *result;     // unique_ptr<Transport>
///   transport->send(data, len);    // Non-blocking
///   transport->recv([](auto* data, auto len) { ... });
template <TcpTransport TcpImpl, size_t MaxPayload = 512, size_t QueueDepth = 1024>
class Transport {
    static_assert(TcpTransport<TcpImpl>,
                  "TcpImpl must satisfy TcpTransport concept");
    static_assert(MaxPayload > 0, "MaxPayload must be > 0");
    static_assert(MaxPayload <= tls_const::kMaxRecordPayload,
                  "MaxPayload exceeds TLS max record size (16384)");
    static_assert(std::has_single_bit(QueueDepth),
                  "QueueDepth must be power of 2");

    using TxMsg = detail::TxMessage<MaxPayload>;
    using RxMsg = detail::RxMessage<MaxPayload>;
    using TxQueue = eph::containers::BoundedQueue<TxMsg, QueueDepth>;
    using RxQueue = eph::containers::BoundedQueue<RxMsg, QueueDepth>;

public:
    /// Factory callable: creates a new, already-connected TcpImpl instance.
    /// Called during initial connect and on each reconnection attempt.
    using TcpFactory = std::function<
        std::expected<std::unique_ptr<TcpImpl>, std::string>()>;

    static constexpr size_t max_payload() noexcept { return MaxPayload; }
    static constexpr size_t queue_depth() noexcept { return QueueDepth; }

    /// Create and connect a transport (TCP + TLS + WebSocket handshake).
    /// This is a blocking call -- performs the full handshake sequence.
    /// Returns unique_ptr because Transport owns threads and is non-movable.
    ///
    /// On failure, returns ConnectionErrorInfo with a typed error code
    /// for programmatic handling and a detail string for logging.
    static std::expected<std::unique_ptr<Transport>, ConnectionErrorInfo>
    create(TcpFactory tcp_factory, const TransportConfig& config) {
        auto log = detail::transport_logger();

        if (!tcp_factory) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kInvalidConfig, "tcp_factory is null"});
        }
        if (auto err = config.validate(); !err.empty()) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kInvalidConfig, std::string(err)});
        }

        SPDLOG_LOGGER_INFO(log,
            "Creating transport: {}:{}{}", config.remote_host,
            config.remote_port, config.ws_path);

        auto t = std::unique_ptr<Transport>(new Transport());
        t->config_      = config;
        t->tcp_factory_ = std::move(tcp_factory);

        auto conn_result = t->do_connect();
        if (!conn_result) {
            SPDLOG_LOGGER_ERROR(log, "Initial connect failed: {}",
                                conn_result.error().message());
            return std::unexpected(conn_result.error());
        }

        // Pre-allocate WS fragmentation buffer to avoid heap allocation
        // on the RX hot path when reassembling multi-frame messages.
        t->ws_frag_buf_.reserve(MaxPayload);

        t->created_at_ = std::chrono::steady_clock::now();
        t->running_.store(true, std::memory_order_release);
        t->notify_state(TransportEvent::kConnected, config.remote_host);

        // Start worker threads (capture raw pointer -- Transport outlives threads)
        auto* tp = t.get();
        t->tx_thread_ = std::thread([tp] { tp->tx_loop(); });
        t->rx_thread_ = std::thread([tp] { tp->rx_loop(); });

        SPDLOG_LOGGER_INFO(log, "Transport ready: {}", config.remote_host);

        return t;
    }

    ~Transport() {
        stop();
    }

    Transport(const Transport&)            = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&)                 = delete;
    Transport& operator=(Transport&&)      = delete;

    // -----------------------------------------------------------------------
    // Send API (application thread)
    // -----------------------------------------------------------------------

    /// Send data as a WebSocket frame (non-blocking).
    ///
    /// @param data     Payload data
    /// @param len      Payload length (must be <= MaxPayload)
    /// @param opcode   WebSocket opcode (default: binary)
    /// @return SendError::kOk on success, or a specific error code
    SendError send(const void* data, size_t len,
                   uint8_t opcode = ws::opcode::kBinary) noexcept {
        // RFC 6455 §5.6: text frames must contain valid UTF-8
        if (opcode == ws::opcode::kText &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return enqueue_tx(data, len, opcode);
    }

    /// Send data from a span (convenience overload).
    SendError send(std::span<const uint8_t> data,
                   uint8_t opcode = ws::opcode::kBinary) noexcept {
        return send(data.data(), data.size(), opcode);
    }

    /// Send data as a WebSocket binary frame (convenience, explicit intent).
    SendError send_binary(const void* data, size_t len) noexcept {
        return send(data, len, ws::opcode::kBinary);
    }

    /// Send data as a WebSocket text frame (convenience for JSON APIs).
    /// Validates UTF-8 encoding per RFC 6455 §5.6. Returns kInvalidUtf8
    /// if the payload is not valid UTF-8.
    SendError send_text(const void* data, size_t len) noexcept {
        if (!ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return enqueue_tx(data, len, ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket text frame (convenience for JSON APIs).
    /// Validates UTF-8 encoding per RFC 6455 §5.6.
    SendError send_text(std::string_view sv) noexcept {
        if (!ws::is_valid_utf8(sv)) {
            return SendError::kInvalidUtf8;
        }
        return enqueue_tx(sv.data(), sv.size(), ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket binary frame.
    SendError send_binary(std::string_view sv) noexcept {
        return send(sv.data(), sv.size(), ws::opcode::kBinary);
    }

    /// Send data with timeout — waits up to `timeout` for TX queue space.
    ///
    /// Unlike send() which returns kQueueFull immediately, this variant
    /// spins briefly waiting for the TX thread to drain the queue.
    /// Useful for backpressure-aware applications that prefer a short
    /// wait over implementing their own retry loop.
    ///
    /// @param data     Payload data
    /// @param len      Payload length (must be <= MaxPayload)
    /// @param timeout  Maximum time to wait for queue space
    /// @param opcode   WebSocket opcode (default: binary)
    /// @return SendError::kOk on success, kQueueFull on timeout
    template <typename Rep, typename Period>
    SendError send_for(const void* data, size_t len,
                       std::chrono::duration<Rep, Period> timeout,
                       uint8_t opcode = ws::opcode::kBinary) noexcept {
        // RFC 6455 §5.6: text frames must contain valid UTF-8
        if (opcode == ws::opcode::kText &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return enqueue_tx_for(data, len, timeout, opcode);
    }

    /// Send data from a span with timeout (convenience overload).
    template <typename Rep, typename Period>
    SendError send_for(std::span<const uint8_t> data,
                       std::chrono::duration<Rep, Period> timeout,
                       uint8_t opcode = ws::opcode::kBinary) noexcept {
        return send_for(data.data(), data.size(), timeout, opcode);
    }

    /// Send a WebSocket text frame with timeout and UTF-8 validation.
    /// Combines send_text()'s UTF-8 check with send_for()'s backpressure wait.
    template <typename Rep, typename Period>
    SendError send_text_for(const void* data, size_t len,
                            std::chrono::duration<Rep, Period> timeout) noexcept {
        if (!ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return enqueue_tx_for(data, len, timeout, ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket text frame with timeout.
    template <typename Rep, typename Period>
    SendError send_text_for(std::string_view sv,
                            std::chrono::duration<Rep, Period> timeout) noexcept {
        if (!ws::is_valid_utf8(sv)) {
            return SendError::kInvalidUtf8;
        }
        return enqueue_tx_for(sv.data(), sv.size(), timeout, ws::opcode::kText);
    }

    /// Send a WebSocket binary frame with timeout (convenience, explicit intent).
    template <typename Rep, typename Period>
    SendError send_binary_for(const void* data, size_t len,
                              std::chrono::duration<Rep, Period> timeout) noexcept {
        return send_for(data, len, timeout, ws::opcode::kBinary);
    }

    /// Send a span as a WebSocket binary frame with timeout.
    template <typename Rep, typename Period>
    SendError send_binary_for(std::span<const uint8_t> data,
                              std::chrono::duration<Rep, Period> timeout) noexcept {
        return send_for(data.data(), data.size(), timeout, ws::opcode::kBinary);
    }

    /// Send a WebSocket Close frame with a custom status code and reason.
    ///
    /// This enqueues the Close frame for transmission by the TX thread.
    /// The transport continues running until the server echoes the Close
    /// frame or stop() is called. For immediate shutdown, use stop().
    ///
    /// @param status_code  Close reason code (e.g., ws::close_code::kGoingAway)
    /// @param reason       Optional human-readable reason (max 123 bytes, truncated if longer)
    /// @return SendError::kOk on success, or a specific error code
    SendError send_close(uint16_t status_code,
                         std::string_view reason = {}) noexcept {
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;
        if (!ws::is_valid_close_code(status_code)) return SendError::kInvalidCloseCode;
        // RFC 6455 §7.1.6: close reason must be valid UTF-8
        if (!reason.empty() && !ws::is_valid_utf8(reason)) return SendError::kInvalidUtf8;

        // Close payload: 2-byte status code + optional reason (max 123 chars per RFC 6455 §5.5)
        size_t reason_len = std::min(reason.size(), size_t{123});
        uint16_t payload_len = static_cast<uint16_t>(2 + reason_len);

        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

        bool ok = tx_queue_.try_produce([&](TxMsg& msg) {
            msg.data[0] = static_cast<uint8_t>(status_code >> 8);
            msg.data[1] = static_cast<uint8_t>(status_code & 0xFF);
            if (reason_len > 0) {
                std::memcpy(msg.data + 2, reason.data(), reason_len);
            }
            msg.len = payload_len;
            msg.opcode = ws::opcode::kClose;
        });

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            return SendError::kQueueFull;
        }
        return SendError::kOk;
    }

    /// Send a WebSocket Ping frame to probe connection liveness.
    ///
    /// Optionally includes application payload (max 125 bytes per RFC 6455 §5.5).
    /// The server MUST reply with a Pong frame echoing the same payload.
    /// Use this for custom heartbeat strategies beyond the built-in ping_interval.
    ///
    /// @param payload      Optional ping payload data (nullptr for empty ping)
    /// @param payload_len  Payload length (must be <= 125, truncated if larger)
    /// @return SendError::kOk on success, or a specific error code
    SendError send_ping(const void* payload = nullptr,
                        size_t payload_len = 0) noexcept {
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;

        // RFC 6455 §5.5: control frame payload MUST NOT exceed 125 bytes
        payload_len = std::min(payload_len, size_t{125});
        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

        bool ok = tx_queue_.try_produce([&](TxMsg& msg) {
            if (payload && payload_len > 0) {
                std::memcpy(msg.data, payload, payload_len);
            }
            msg.len = static_cast<uint16_t>(payload_len);
            msg.opcode = ws::opcode::kPing;
        });

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            return SendError::kQueueFull;
        }
        return SendError::kOk;
    }

    /// Batch-send multiple messages (all-or-nothing semantics).
    ///
    /// Enqueues all messages with a single atomic tail update, amortizing
    /// the per-message atomic store overhead. All messages share the same opcode.
    /// Zero heap allocations — writes directly into pre-reserved queue slots.
    ///
    /// @param payloads  Array of {data, len} pairs
    /// @param count     Number of messages
    /// @param opcode    WebSocket opcode for all messages
    /// @return SendError::kOk on success, or a specific error code
    SendError send_n(const std::span<const uint8_t>* payloads, size_t count,
                     uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;

        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxPayload) return SendError::kMessageTooLarge;
        }

        // Write directly into queue slots — no temporary array needed.
        bool ok = tx_queue_.try_produce_n(count,
            [&](TxMsg& slot, size_t i) {
                std::memcpy(slot.data, payloads[i].data(), payloads[i].size());
                slot.len = static_cast<uint16_t>(payloads[i].size());
                slot.opcode = opcode;
            });

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            return SendError::kQueueFull;
        }
        return SendError::kOk;
    }

    // -----------------------------------------------------------------------
    // Receive API (application thread)
    // -----------------------------------------------------------------------

    /// Try to receive a message (non-blocking).
    /// @param callback  Called with (data_ptr, len) if a message is available.
    /// @return true if a message was consumed, false if queue empty.
    /// @warning The data pointer passed to callback is only valid for the
    ///          duration of the callback invocation. Copy the data if you
    ///          need it after the callback returns -- the underlying SPSC
    ///          queue slot will be reused.
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    bool recv(F&& callback) {
        return rx_queue_.try_consume([&](RxMsg& msg) {
            std::invoke(std::forward<F>(callback), msg.data, msg.len);
        });
    }

    /// Try to receive a message with opcode (non-blocking).
    /// @param callback  Called with (data_ptr, len, opcode) if a message is available.
    ///                  opcode is one of ws::opcode::kBinary, ws::opcode::kText, etc.
    /// @return true if a message was consumed, false if queue empty.
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t, uint8_t>
    bool recv(F&& callback) {
        return rx_queue_.try_consume([&](RxMsg& msg) {
            std::invoke(std::forward<F>(callback), msg.data, msg.len, msg.opcode);
        });
    }

    /// Try to receive a message as a copied byte vector (non-blocking).
    /// Returns the payload bytes, or nullopt if the queue is empty.
    /// Prefer the callback variant for zero-copy hot paths.
    [[nodiscard]] std::optional<std::vector<uint8_t>> try_recv() {
        std::optional<std::vector<uint8_t>> result;
        rx_queue_.try_consume([&](RxMsg& msg) {
            result.emplace(msg.data, msg.data + msg.len);
        });
        return result;
    }

    /// Received message with opcode metadata.
    struct ReceivedMessage {
        std::vector<uint8_t> data;
        uint8_t opcode = ws::opcode::kBinary;

        /// Check if this is a text message.
        [[nodiscard]] bool is_text() const noexcept {
            return opcode == ws::opcode::kText;
        }
        /// Check if this is a binary message.
        [[nodiscard]] bool is_binary() const noexcept {
            return opcode == ws::opcode::kBinary;
        }
        /// Check if this is a close frame.
        /// Close frames are delivered to the RX queue when the server sends
        /// a WebSocket Close frame. Use close_code() and close_reason() to
        /// extract the status code and reason string from the payload.
        [[nodiscard]] bool is_close() const noexcept {
            return opcode == ws::opcode::kClose;
        }
        /// Return the payload as a string_view (valid only for text messages).
        [[nodiscard]] std::string_view text() const noexcept {
            return {reinterpret_cast<const char*>(data.data()), data.size()};
        }
        /// Extract the close status code from a close frame payload.
        /// Returns 0 if the payload is too short (< 2 bytes) or not a close frame.
        /// Common codes: 1000 (Normal), 1001 (GoingAway), 1002 (ProtocolError).
        [[nodiscard]] uint16_t close_code() const noexcept {
            if (opcode != ws::opcode::kClose || data.size() < 2) return 0;
            return static_cast<uint16_t>((data[0] << 8) | data[1]);
        }
        /// Extract the close reason string from a close frame payload.
        /// Returns empty string_view if not a close frame or no reason present.
        [[nodiscard]] std::string_view close_reason() const noexcept {
            if (opcode != ws::opcode::kClose || data.size() <= 2) return {};
            return {reinterpret_cast<const char*>(data.data() + 2),
                    data.size() - 2};
        }
    };

    /// Try to receive a message with opcode info (non-blocking).
    /// Returns payload + opcode, or nullopt if the queue is empty.
    [[nodiscard]] std::optional<ReceivedMessage> try_recv_msg() {
        std::optional<ReceivedMessage> result;
        rx_queue_.try_consume([&](RxMsg& msg) {
            result.emplace(ReceivedMessage{
                .data = std::vector<uint8_t>(msg.data, msg.data + msg.len),
                .opcode = msg.opcode,
            });
        });
        return result;
    }

    /// Batch-receive up to max_count messages (non-blocking, best-effort).
    ///
    /// Calls callback with (data_ptr, len) for each available message,
    /// consuming up to max_count from the RX queue in a single drain loop.
    ///
    /// @param callback    Called with (data_ptr, len) for each message
    /// @param max_count   Maximum number of messages to consume
    /// @return Number of messages actually consumed
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    size_t recv_n(F&& callback, size_t max_count) {
        return rx_queue_.try_consume_n(max_count,
            [&](RxMsg& msg, [[maybe_unused]] size_t idx) {
                std::invoke(std::forward<F>(callback), msg.data, msg.len);
            });
    }

    /// Batch-receive up to max_count messages with opcode (non-blocking).
    ///
    /// Uses try_consume_n for amortized atomic operations (single head
    /// update for the entire batch, matching send_n's try_produce_n).
    ///
    /// @param callback    Called with (data_ptr, len, opcode) for each message
    /// @param max_count   Maximum number of messages to consume
    /// @return Number of messages actually consumed
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t, uint8_t>
    size_t recv_n(F&& callback, size_t max_count) {
        return rx_queue_.try_consume_n(max_count,
            [&](RxMsg& msg, [[maybe_unused]] size_t idx) {
                std::invoke(std::forward<F>(callback),
                            msg.data, msg.len, msg.opcode);
            });
    }

    /// Drain all available messages (non-blocking).
    /// Equivalent to recv_n(callback, queue_depth()).
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    size_t drain_recv(F&& callback) {
        return recv_n(std::forward<F>(callback), QueueDepth);
    }

    /// Blocking receive with timeout — waits for a message to arrive.
    ///
    /// Suitable for non-DPDK (socket) transports where busy-polling
    /// the RX queue is wasteful. Yields the thread between polls to
    /// keep CPU usage low while maintaining sub-millisecond wake-up.
    ///
    /// @param callback  Called with (data_ptr, len) when a message arrives
    /// @param timeout   Maximum time to wait for a message
    /// @return true if a message was consumed, false on timeout or stop
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t>
    bool wait_recv(F&& callback, std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (running_.load(std::memory_order_acquire)) {
            bool got = rx_queue_.try_consume([&](RxMsg& msg) {
                std::invoke(std::forward<F>(callback), msg.data, msg.len);
            });
            if (got) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return false;
    }

    /// Blocking receive with opcode and timeout.
    template <typename F>
        requires std::invocable<F, const uint8_t*, uint16_t, uint8_t>
    bool wait_recv(F&& callback, std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (running_.load(std::memory_order_acquire)) {
            bool got = rx_queue_.try_consume([&](RxMsg& msg) {
                std::invoke(std::forward<F>(callback),
                            msg.data, msg.len, msg.opcode);
            });
            if (got) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return false;
    }

    /// Blocking receive returning a ReceivedMessage with opcode and timeout.
    /// Returns nullopt on timeout or transport stopped.
    [[nodiscard]] std::optional<ReceivedMessage> wait_recv_msg(
            std::chrono::milliseconds timeout) {
        std::optional<ReceivedMessage> result;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (running_.load(std::memory_order_acquire)) {
            bool got = rx_queue_.try_consume([&](RxMsg& msg) {
                result.emplace(ReceivedMessage{
                    .data = std::vector<uint8_t>(msg.data, msg.data + msg.len),
                    .opcode = msg.opcode,
                });
            });
            if (got) return result;
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::this_thread::yield();
        }
        return std::nullopt;
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Initiate a graceful WebSocket close handshake (RFC 6455 §7.1.1).
    ///
    /// Sends a Close frame with the given status code and waits for the
    /// server to echo a Close response, up to the specified timeout.
    /// After the server responds (or timeout expires), calls stop().
    ///
    /// This is the RFC-compliant way to shut down: the client sends Close,
    /// the server echoes Close, then both sides close the TCP connection.
    ///
    /// @param status_code  Close reason code (default: 1000 Normal Closure)
    /// @param reason       Optional human-readable close reason (max 123 bytes)
    /// @param timeout      Maximum time to wait for server Close response
    /// @return true if server responded with Close before timeout
    bool close_gracefully(
            uint16_t status_code = ws::close_code::kNormal,
            std::string_view reason = "client shutdown",
            std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) noexcept {
        auto err = send_close(status_code, reason);
        if (err != SendError::kOk) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "close_gracefully: send_close failed: {}",
                send_error_name(err));
            stop();
            return false;
        }

        // Wait for the server Close response (RX thread sets closing_=true)
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (running_.load(std::memory_order_acquire) &&
               !closing_.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "close_gracefully: timed out waiting for server Close "
                    "response ({}ms)", timeout.count());
                stop();
                return false;
            }
            std::this_thread::yield();
        }

        // Server responded with Close — stop cleanly
        stop();
        return true;
    }

    /// Stop the transport gracefully. Sends WebSocket Close frame.
    ///
    /// Thread safety: waits for TX/RX threads to exit BEFORE touching
    /// crypto_ or tcp_, avoiding data races on shared state.
    void stop() noexcept {
        bool was_running = running_.exchange(false, std::memory_order_acq_rel);

        auto log = detail::transport_logger();
        SPDLOG_LOGGER_INFO(log, "Stopping transport");

        // Join worker threads FIRST — ensures no concurrent access to
        // crypto_/tcp_ from TX/RX threads when we send the Close frame.
        if (tx_thread_.joinable()) tx_thread_.join();
        if (rx_thread_.joinable()) rx_thread_.join();

        // Send WebSocket Close frame after threads have exited (no race)
        if (was_running && tcp_ && tcp_->is_established() &&
            (config_.use_tls ? crypto_ != nullptr : true)) {
            // Close frame: header (max 14) + payload (max 125) = 139 bytes,
            // plus 1 byte for encrypt()'s temporary content type append.
            uint8_t close_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
            size_t close_len = ws::build_close_frame(
                close_buf, ws::close_code::kNormal, "client shutdown");

            if (config_.use_tls) {
                // TLS output: record header + ciphertext + content type + auth tag
                uint8_t tls_buf[TlsRecordCrypto::encrypted_size(
                    ws::kMaxFrameHeaderLen + 125)]{};
                uint16_t tls_len = crypto_->encrypt(
                    close_buf, static_cast<uint16_t>(close_len), tls_buf);
                if (tls_len > 0) {
                    tcp_->send(tls_buf, tls_len);
                }
            } else {
                // Plain WS: send close frame directly over TCP
                tcp_->send(close_buf, close_len);
            }
        }

        // Close TCP connection
        if (tcp_ && tcp_->is_established()) {
            tcp_->close();
        }

        notify_state(TransportEvent::kStopped);
        SPDLOG_LOGGER_INFO(log, "Transport stopped");
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// Query the current connection state (lock-free, safe from any thread).
    [[nodiscard]] TransportState state() const noexcept {
        if (!running_.load(std::memory_order_acquire))
            return TransportState::kStopped;
        if (reconnecting_.load(std::memory_order_acquire))
            return TransportState::kReconnecting;
        return TransportState::kConnected;
    }

    /// Check if the transport is connected and data can flow.
    [[nodiscard]] bool is_connected() const noexcept {
        return state() == TransportState::kConnected;
    }

    // -----------------------------------------------------------------------
    // Queue occupancy (backpressure monitoring)
    // -----------------------------------------------------------------------

    /// Approximate number of messages pending in the TX queue.
    /// Useful for detecting backpressure before send() returns -EAGAIN.
    /// @note Result is approximate — the producer and consumer may
    ///       advance between the size() read and the caller's use.
    [[nodiscard]] size_t tx_queue_size() const noexcept {
        return tx_queue_.size();
    }

    /// Approximate number of messages available in the RX queue.
    [[nodiscard]] size_t rx_queue_size() const noexcept {
        return rx_queue_.size();
    }

    /// TX queue occupancy as a fraction [0.0, 1.0].
    [[nodiscard]] double tx_queue_fill_ratio() const noexcept {
        return static_cast<double>(tx_queue_.size()) /
               static_cast<double>(QueueDepth);
    }

    /// RX queue occupancy as a fraction [0.0, 1.0].
    [[nodiscard]] double rx_queue_fill_ratio() const noexcept {
        return static_cast<double>(rx_queue_.size()) /
               static_cast<double>(QueueDepth);
    }

    /// Reset all statistics counters to zero.
    /// Useful for windowed measurement: call stats(), then reset_stats().
    /// @warning Not thread-safe with stats() — call from one thread only
    ///          (typically the application thread between measurement windows).
    void reset_stats() noexcept {
        tx_stats_ = {};
        rx_stats_ = {};
        queue_full_count_.store(0, std::memory_order_relaxed);
        ws_pings_received_.store(0, std::memory_order_relaxed);
        ws_pongs_sent_.store(0, std::memory_order_relaxed);
        pong_timeouts_.store(0, std::memory_order_relaxed);
        reconnect_count_.store(0, std::memory_order_relaxed);
        rtt_histogram_.reset();
    }

    /// Negotiated TLS version string (e.g. "TLSv1.3"), or "none" if not connected.
    [[nodiscard]] std::string_view tls_version() const noexcept {
        return tls_version_;
    }

    /// Negotiated cipher suite name (e.g. "TLS_AES_256_GCM_SHA384"), or "none".
    [[nodiscard]] std::string_view cipher_name() const noexcept {
        return cipher_name_;
    }

    /// Negotiated WebSocket subprotocol (from server's Sec-WebSocket-Protocol
    /// response header), or empty string if none was negotiated.
    [[nodiscard]] std::string_view ws_subprotocol() const noexcept {
        return ws_subprotocol_;
    }

    /// Resolved remote IP address (e.g. "10.0.0.1") from the last connection.
    /// Available if the underlying TcpImpl exposes a resolved_ip() method.
    /// Returns empty string if not available or not yet connected.
    [[nodiscard]] std::string_view remote_ip() const noexcept {
        return remote_ip_;
    }

    /// Snapshot of round-trip time statistics from ping/pong measurements.
    ///
    /// Requires TSC to be initialized (TSC::init() called before Transport::create).
    /// If TSC is not initialized or no pings have been exchanged, all fields are zero.
    ///
    /// @note The histogram is owned by the RX thread. This method reads it
    ///       from the application thread, which is safe because HdrHistogram
    ///       reads are non-destructive and the RX thread only appends samples.
    ///       In the worst case, a concurrent write may cause a slightly stale
    ///       read — acceptable for monitoring purposes.
    [[nodiscard]] RttStats rtt_stats() const noexcept {
        if (rtt_histogram_.get_total_count() == 0) return {};
        return RttStats{
            .count   = rtt_histogram_.get_total_count(),
            .min_ns  = rtt_histogram_.get_min_value(),
            .max_ns  = rtt_histogram_.get_max_value(),
            .mean_ns = rtt_histogram_.get_mean(),
            .p50_ns  = rtt_histogram_.get_value_at_percentile(50.0),
            .p99_ns  = rtt_histogram_.get_value_at_percentile(99.0),
            .p999_ns = rtt_histogram_.get_value_at_percentile(99.9),
        };
    }

    [[nodiscard]] TransportStats stats() const noexcept {
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - created_at_).count();
        return TransportStats{
            .tx_packets        = tx_stats_.packets,
            .tx_bytes          = tx_stats_.bytes,
            .tx_text_packets   = tx_stats_.text_packets,
            .tx_text_bytes     = tx_stats_.text_bytes,
            .tx_dropped        = tx_stats_.dropped,
            .rx_packets        = rx_stats_.packets,
            .rx_bytes          = rx_stats_.bytes,
            .rx_text_packets   = rx_stats_.text_packets,
            .rx_text_bytes     = rx_stats_.text_bytes,
            .rx_dropped        = rx_stats_.dropped,
            .encrypt_errors    = tx_stats_.crypto_errors,
            .decrypt_errors    = rx_stats_.crypto_errors,
            .queue_full_count  = queue_full_count_.load(std::memory_order_relaxed),
            .ws_pings_received = ws_pings_received_.load(std::memory_order_relaxed),
            .ws_pongs_sent     = ws_pongs_sent_.load(std::memory_order_relaxed),
            .pong_timeouts     = pong_timeouts_.load(std::memory_order_relaxed),
            .reconnect_count   = reconnect_count_.load(std::memory_order_relaxed),
            .uptime_ns         = static_cast<uint64_t>(uptime > 0 ? uptime : 0),
            .handshake_ns      = last_handshake_ns_,
            .remote_ip         = remote_ip_,
        };
    }

private:
    Transport() = default;

    // -----------------------------------------------------------------------
    // Internal enqueue helpers (no UTF-8 validation — caller is responsible)
    // -----------------------------------------------------------------------

    /// Enqueue a message to TX queue without UTF-8 validation.
    /// Shared implementation for send() and send_text() to avoid
    /// double-validating UTF-8 on the hot path.
    SendError enqueue_tx(const void* data, size_t len,
                         uint8_t opcode) noexcept {
        if (len > MaxPayload) return SendError::kMessageTooLarge;
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;

        bool ok = tx_queue_.try_produce([&](TxMsg& msg) {
            std::memcpy(msg.data, data, len);
            msg.len = static_cast<uint16_t>(len);
            msg.opcode = opcode;
        });

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            return SendError::kQueueFull;
        }
        return SendError::kOk;
    }

    /// Enqueue with timeout, no UTF-8 validation.
    template <typename Rep, typename Period>
    SendError enqueue_tx_for(const void* data, size_t len,
                             std::chrono::duration<Rep, Period> timeout,
                             uint8_t opcode) noexcept {
        if (len > MaxPayload) return SendError::kMessageTooLarge;
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;

        bool ok = tx_queue_.try_produce_for([&](TxMsg& msg) {
            std::memcpy(msg.data, data, len);
            msg.len = static_cast<uint16_t>(len);
            msg.opcode = opcode;
        }, timeout);

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            return SendError::kQueueFull;
        }
        return SendError::kOk;
    }

    TransportConfig                        config_;
    TcpFactory                             tcp_factory_;
    std::unique_ptr<TcpImpl>               tcp_;
    std::unique_ptr<TlsSession<TcpImpl>>   tls_;   // Only used during create(), not on hot path

    // Connection metadata captured after each successful handshake
    std::string                            tls_version_{"none"};
    std::string                            cipher_name_{"none"};
    std::string                            ws_subprotocol_{};
    std::string                            remote_ip_{};
    std::unique_ptr<TlsRecordCrypto>       crypto_;

    // Uptime tracking
    std::chrono::steady_clock::time_point  created_at_{};

    TxQueue                                tx_queue_{};
    RxQueue                                rx_queue_{};

    std::atomic<bool>                      running_{false};
    // RX sets reconnecting_=true before modifying crypto_/tcp_;
    // TX spins while this flag is set to avoid data races.
    std::atomic<bool>                      reconnecting_{false};
    // RX sets closing_=true when a server Close frame is received.
    // TX drains the queue (sending the Close response) before exiting.
    std::atomic<bool>                      closing_{false};
    std::thread                            tx_thread_;
    std::thread                            rx_thread_;

    // Per-thread stats to avoid cross-core atomic contention
    ThreadStats                            tx_stats_{};
    ThreadStats                            rx_stats_{};
    // App-thread-only counters (no contention -- only send() writes these)
    std::atomic<uint64_t>                  queue_full_count_{0};
    std::atomic<uint64_t>                  ws_pings_received_{0};
    std::atomic<uint64_t>                  ws_pongs_sent_{0};
    std::atomic<uint64_t>                  reconnect_count_{0};
    std::atomic<uint64_t>                  pong_timeouts_{0};
    uint64_t                               last_handshake_ns_{0};

    // Pong timeout tracking (TX thread writes ping time, RX thread writes pong time).
    // Using atomics with relaxed ordering — occasional stale reads are acceptable
    // since pong timeout detection is a best-effort liveness check, not a
    // precision requirement.
    using SteadyTimePoint = std::chrono::steady_clock::time_point;
    std::atomic<int64_t>                   last_pong_ns_{0};      // RX writes, TX reads
    bool                                   ping_awaiting_pong_{false}; // TX-thread-local

    // RTT measurement: TX thread writes TSC timestamp when sending ping,
    // RX thread reads it when pong arrives and records the delta in the
    // histogram.  The histogram is owned exclusively by the RX thread.
    std::atomic<uint64_t>                  last_ping_tsc_{0};     // TX writes, RX reads
    eph::utils::HdrHistogram               rtt_histogram_{
        100,          // lowest: 100 ns (~0.1 us)
        10'000'000'000ULL, // highest: 10 s (covers even slow WAN)
        3             // 3 significant digits
    };

    // WebSocket fragmentation reassembly buffer (RX thread only).
    // Accumulates continuation frames until FIN=1.
    std::vector<uint8_t>                   ws_frag_buf_;
    uint8_t                                ws_frag_opcode_ = 0;

    // -----------------------------------------------------------------------
    // State change notification
    // -----------------------------------------------------------------------

    void notify_state(TransportEvent event, std::string_view detail = {}) noexcept {
        if (config_.on_state_change) {
            try {
                config_.on_state_change(event, detail);
            } catch (...) {
                // Callback must not throw, but guard defensively
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "on_state_change callback threw an exception");
            }
        }
    }


    // -----------------------------------------------------------------------
    // Connection establishment (reused by create() and reconnect)
    // -----------------------------------------------------------------------

    /// Full connection sequence: TCP (via factory) -> [TLS] -> WS Upgrade -> [key export].
    /// TLS phases are skipped when config_.use_tls is false (plain ws://).
    /// On success, tcp_ (and optionally tls_, crypto_) are populated and ready.
    /// On failure, previous state is cleaned up.
    std::expected<void, ConnectionErrorInfo> do_connect() {
        auto log = detail::transport_logger();
        auto connect_start = std::chrono::steady_clock::now();

        // Phase 1: Create TCP session via factory (factory handles connect)
        auto tcp_result = tcp_factory_();
        if (!tcp_result) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kFactoryFailed,
                std::format("TCP factory failed: {}", tcp_result.error())});
        }
        tcp_ = std::move(*tcp_result);

        if (!tcp_->is_established()) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kTcpNotEstablished,
                "TCP factory returned non-established session"});
        }

        if (config_.use_tls) {
            // Phase 2: TLS handshake
            TlsConfig tls_cfg{
                .hostname = config_.remote_host,
                .ca_cert_path = config_.ca_cert_path,
                .verify_peer = config_.verify_peer,
                .handshake_timeout = config_.tls_timeout,
            };

            auto tls_result = TlsSession<TcpImpl>::create(*tcp_, tls_cfg);
            if (!tls_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsSessionFailed,
                    std::format("TLS session failed: {}", tls_result.error())});
            }
            tls_ = std::make_unique<TlsSession<TcpImpl>>(std::move(*tls_result));

            auto hs_result = tls_->handshake();
            if (!hs_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsHandshakeFailed,
                    std::format("TLS handshake failed: {}", hs_result.error())});
            }
        }

        // Phase 3: WebSocket upgrade (over TLS or plain TCP)
        auto ws_result = do_ws_upgrade();
        if (!ws_result) {
            return std::unexpected(ws_result.error());
        }

        if (config_.use_tls) {
            // Phase 4: Extract keys for AEAD hot path
            auto hot_state = tls_->extract_hot_state();
            if (!hot_state) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsKeyExportFailed,
                    std::format("TLS key export failed: {}", hot_state.error())});
            }

            size_t key_len = tls_->cipher_key_len();
            auto crypto = TlsRecordCrypto::create(*hot_state, key_len);
            if (!crypto) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsKeyExportFailed,
                    std::format("TLS AEAD init failed: {}", crypto.error())});
            }
            crypto_ = std::make_unique<TlsRecordCrypto>(std::move(*crypto));

            // Capture TLS connection metadata
            tls_version_ = tls_->tls_version();
            cipher_name_ = tls_->cipher_name();
        } else {
            tls_version_ = "none";
            cipher_name_ = "none";
        }

        // Extract resolved IP if the TCP backend exposes it
        if constexpr (requires { tcp_->resolved_ip(); }) {
            remote_ip_ = std::string(tcp_->resolved_ip());
        }

        // Initialize pong timestamp to "now" so pong timeout doesn't fire
        // before the first ping/pong exchange completes.
        last_pong_ns_.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);
        ping_awaiting_pong_ = false;

        // Record handshake duration
        auto connect_end = std::chrono::steady_clock::now();
        last_handshake_ns_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                connect_end - connect_start).count());

        SPDLOG_LOGGER_INFO(log,
            "Connected: {} ({}, handshake: {:.1f}ms)",
            config_.remote_host,
            config_.use_tls
                ? std::format("TLS: {}, cipher: {}", tls_version_, cipher_name_)
                : std::string("plain WS"),
            static_cast<double>(last_handshake_ns_) / 1e6);
        return {};
    }

    /// Attempt reconnection with exponential backoff and jitter.
    /// Discards old SPSC queue data. Called from RX thread when disconnect
    /// is detected. Returns true if reconnection succeeded.
    ///
    /// Backoff schedule: base * 2^(attempt-1), capped at max_reconnect_backoff.
    /// Each delay is jittered by ±25% to avoid thundering herd.
    bool do_reconnect() {
        auto log = detail::transport_logger();
        int max_attempts = config_.max_reconnect_attempts;
        if (max_attempts <= 0) {
            SPDLOG_LOGGER_ERROR(log, "Auto-reconnect disabled, stopping");
            return false;
        }

        notify_state(TransportEvent::kDisconnected, config_.remote_host);

        // Record disconnect time for downtime measurement
        auto disconnect_time = std::chrono::steady_clock::now();

        // Signal TX thread to pause: it must not touch crypto_/tcp_
        // while we are reconnecting.
        reconnecting_.store(true, std::memory_order_release);

        // Discard stale queue data and fragment buffer
        tx_queue_.clear();
        ws_frag_buf_.clear();
        closing_.store(false, std::memory_order_release);

        // Compute backoff cap: explicit max, or 16x base as default
        auto base_ms = config_.reconnect_interval.count();
        auto max_backoff_ms = config_.max_reconnect_backoff.count();
        if (max_backoff_ms <= 0) {
            max_backoff_ms = base_ms * 16;
        }

        // Thread-local RNG for jitter (seeded from hardware entropy)
        thread_local std::mt19937 rng{std::random_device{}()};

        auto current_delay_ms = base_ms;

        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            // Apply ±25% jitter to current delay
            auto jitter_lo = current_delay_ms * 3 / 4;  // 75%
            auto jitter_hi = current_delay_ms * 5 / 4;  // 125%
            std::uniform_int_distribution<int64_t> dist(
                std::max(jitter_lo, int64_t{1}), std::max(jitter_hi, int64_t{1}));
            auto actual_delay_ms = dist(rng);

            SPDLOG_LOGGER_INFO(log,
                "Reconnect attempt {}/{} in {}ms (backoff: {}ms)",
                attempt, max_attempts, actual_delay_ms, current_delay_ms);

            notify_state(TransportEvent::kReconnecting,
                std::format("{}/{}", attempt, max_attempts));

            std::this_thread::sleep_for(
                std::chrono::milliseconds{actual_delay_ms});

            // Clean up old connection state
            crypto_.reset();
            tls_.reset();
            tcp_.reset();

            auto result = do_connect();
            if (result) {
                auto total = reconnect_count_.fetch_add(1, std::memory_order_relaxed) + 1;
                reconnecting_.store(false, std::memory_order_release);
                notify_state(TransportEvent::kConnected,
                    std::format("reconnect attempt {}", attempt));

                auto downtime_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - disconnect_time).count());

                SPDLOG_LOGGER_INFO(log,
                    "Reconnected successfully on attempt {} (downtime: {:.1f}ms)",
                    attempt, static_cast<double>(downtime_ns) / 1e6);

                // Notify application — ideal for replaying subscriptions
                if (config_.on_reconnected) {
                    try {
                        config_.on_reconnected(attempt, downtime_ns,
                            static_cast<uint64_t>(total));
                    } catch (...) {
                        SPDLOG_LOGGER_WARN(log,
                            "on_reconnected callback threw an exception");
                    }
                }
                return true;
            }

            SPDLOG_LOGGER_WARN(log,
                "Reconnect attempt {} failed: {}",
                attempt, result.error().message());

            // Let application decide whether to continue retrying.
            // Useful for aborting on non-transient errors (e.g., TLS
            // certificate rejection, HTTP 403).
            if (config_.on_reconnect_attempt) {
                try {
                    auto err_msg = result.error().message();
                    bool should_continue = config_.on_reconnect_attempt(
                        attempt, max_attempts, err_msg);
                    if (!should_continue) {
                        SPDLOG_LOGGER_INFO(log,
                            "Reconnect aborted by on_reconnect_attempt "
                            "callback after attempt {}", attempt);
                        break;
                    }
                } catch (...) {
                    SPDLOG_LOGGER_WARN(log,
                        "on_reconnect_attempt callback threw an exception");
                }
            }

            // Exponential backoff: double delay, capped at max
            current_delay_ms = std::min(current_delay_ms * 2, max_backoff_ms);
        }

        reconnecting_.store(false, std::memory_order_release);
        SPDLOG_LOGGER_ERROR(log,
            "All {} reconnect attempts exhausted", max_attempts);
        return false;
    }

    // -----------------------------------------------------------------------
    // WebSocket upgrade (Phase 3 of handshake)
    // -----------------------------------------------------------------------

    std::expected<void, ConnectionErrorInfo> do_ws_upgrade() {
        auto log = detail::transport_logger();

        // Generate WebSocket key
        auto ws_key_result = http::generate_ws_key();
        if (!ws_key_result) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kWsUpgradeFailed, ws_key_result.error()});
        }
        std::string ws_key = std::move(*ws_key_result);

        // Build upgrade request
        std::string host = config_.remote_host;
        if (config_.remote_port != 443) {
            host += ":" + std::to_string(config_.remote_port);
        }

        // Build extra headers including subprotocol if configured
        std::string headers = config_.extra_headers;
        if (!config_.ws_subprotocol.empty()) {
            headers += std::format("Sec-WebSocket-Protocol: {}\r\n",
                                   config_.ws_subprotocol);
        }

        std::string request = http::build_upgrade_request(
            host, config_.ws_path, ws_key, headers);

        SPDLOG_LOGGER_DEBUG(log, "Sending WebSocket upgrade request ({})",
            config_.use_tls ? "TLS" : "plain TCP");

        // Send upgrade request through TLS or plain TCP
        if (config_.use_tls) {
            auto write_result = tls_->handshake_write(request.data(),
                                                        static_cast<int>(request.size()));
            if (!write_result || *write_result <= 0) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kWsUpgradeFailed,
                    "Failed to send WebSocket upgrade request"});
            }
        } else {
            auto write_result = tcp_->send(request.data(), request.size());
            if (!write_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kWsUpgradeFailed,
                    std::format("Failed to send WebSocket upgrade request: {}",
                                write_result.error())});
            }
        }

        // Read upgrade response (with timeout).
        // Cap buffer at 64KB to prevent unbounded allocation from
        // a misbehaving server sending oversized HTTP responses.
        static constexpr size_t kMaxUpgradeResponseSize = 65536;
        std::vector<uint8_t> response_buf;
        response_buf.reserve(4096);

        auto deadline = std::chrono::steady_clock::now() + config_.ws_timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            uint8_t buf[4096];
            int bytes_read = 0;

            if (config_.use_tls) {
                auto read_result = tls_->handshake_read(buf, sizeof(buf));
                if (!read_result) {
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        std::format("Failed to read upgrade response: {}",
                                    read_result.error())});
                }
                bytes_read = *read_result;
            } else {
                // Plain TCP: poll_rx with a short timeout to avoid busy-spin
                auto rx_result = tcp_->poll_rx(
                    [&](const uint8_t* data, uint16_t len) {
                        bytes_read = len;
                        std::memcpy(buf, data, std::min(static_cast<size_t>(len),
                                                         sizeof(buf)));
                    });
                if (!rx_result) {
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        std::format("Failed to read upgrade response: {}",
                                    rx_result.error())});
                }
            }

            if (bytes_read > 0) {
                if (response_buf.size() + static_cast<size_t>(bytes_read) > kMaxUpgradeResponseSize) {
                    SPDLOG_LOGGER_ERROR(log,
                        "WebSocket upgrade response exceeds {}B limit",
                        kMaxUpgradeResponseSize);
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        "WebSocket upgrade response too large"});
                }
                response_buf.insert(response_buf.end(),
                                    buf, buf + bytes_read);
            }

            // Check if we have a complete HTTP response
            auto response_str = std::string_view(
                reinterpret_cast<const char*>(response_buf.data()),
                response_buf.size());

            if (response_str.find("\r\n\r\n") != std::string_view::npos) {
                // Parse the response
                auto parsed = http::parse_upgrade_response(
                    reinterpret_cast<const char*>(response_buf.data()),
                    response_buf.size());

                if (!parsed) {
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        std::format("Failed to parse upgrade response: {}",
                                    parsed.error())});
                }

                if (parsed->status_code != 101) {
                    SPDLOG_LOGGER_ERROR(log,
                        "WebSocket upgrade rejected: status={}",
                        parsed->status_code);
                    return std::unexpected(ConnectionErrorInfo{
                        .code = ConnectionError::kWsUpgradeRejected,
                        .detail = std::format("WebSocket upgrade rejected (status {})",
                                    parsed->status_code),
                        .http_status = parsed->status_code});
                }

                if (!parsed->has_upgrade || !parsed->has_connection_upgrade) {
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        "Missing Upgrade/Connection headers in response"});
                }

                // Validate Sec-WebSocket-Accept
                if (!http::validate_ws_accept(ws_key,
                                               parsed->sec_ws_accept)) {
                    SPDLOG_LOGGER_ERROR(log,
                        "Sec-WebSocket-Accept validation failed");
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsAcceptInvalid,
                        "Sec-WebSocket-Accept validation failed"});
                }

                // Store negotiated subprotocol for user queries
                ws_subprotocol_ = std::move(parsed->sec_ws_protocol);

                SPDLOG_LOGGER_INFO(log, "WebSocket upgrade successful{}",
                    ws_subprotocol_.empty() ? ""
                        : std::format(" (subprotocol: {})", ws_subprotocol_));
                return {};
            }
        }

        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kWsUpgradeFailed,
            "WebSocket upgrade response timeout"});
    }

    // -----------------------------------------------------------------------
    // TX worker loop (runs on dedicated thread)
    // -----------------------------------------------------------------------

    void tx_loop() {
        eph::utils::set_thread_affinity(config_.tx_cpu, "TX");
        auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "TX loop started");

        // WS encode buffer: header + payload + 1 byte for TLS content type append
        constexpr size_t kWsBufSize =
            ws::kMaxFrameHeaderLen + MaxPayload + 1;
        // TLS output buffer: sized for the actual max WS frame (without the +1 temp byte)
        constexpr size_t kMaxWsFrame =
            ws::kMaxFrameHeaderLen + MaxPayload;
        constexpr size_t kTlsBufSize =
            TlsRecordCrypto::encrypted_size(
                static_cast<uint16_t>(kMaxWsFrame));

        // Batch buffers for drain loop (sized from config)
        const int kMaxBatch = config_.tx_burst_size;
        auto batch    = std::make_unique<TxMsg[]>(kMaxBatch);
        auto tls_bufs_storage = std::make_unique<uint8_t[]>(
            static_cast<size_t>(kMaxBatch) * kTlsBufSize);
        auto tls_lens = std::make_unique<uint16_t[]>(kMaxBatch);

        // Single WS encode buffer reused per message
        uint8_t ws_buf[kWsBufSize];

        ws::FrameTemplate ws_tmpl = ws::FrameTemplate::for_binary();

        auto last_ping = std::chrono::steady_clock::now();

        while (running_.load(std::memory_order_acquire)) {
            // Spin-wait while RX thread is reconnecting to avoid
            // touching crypto_/tcp_ which are being replaced.
            // Also re-check running_ so stop() can terminate the TX thread
            // even during a prolonged reconnection attempt.
            if (reconnecting_.load(std::memory_order_acquire)) [[unlikely]] {
                if (!running_.load(std::memory_order_acquire)) break;
                eph::utils::cpu_relax();
                continue;
            }

            // -- WebSocket ping / pong timeout (periodic keepalive, owned by TX thread) --
            if (config_.ping_interval.count() > 0) {
                auto now = std::chrono::steady_clock::now();

                // Check pong timeout before sending next ping.
                // If we sent a ping and haven't received a pong within pong_timeout,
                // the peer is considered dead — trigger reconnect via running_=false.
                if (config_.pong_timeout.count() > 0 && ping_awaiting_pong_) {
                    auto last_pong_tp = SteadyTimePoint{
                        std::chrono::nanoseconds{
                            last_pong_ns_.load(std::memory_order_relaxed)}};
                    if (now - last_pong_tp > config_.pong_timeout) {
                        pong_timeouts_.fetch_add(1, std::memory_order_relaxed);
                        SPDLOG_LOGGER_WARN(log,
                            "Pong timeout: no pong received within {}s, "
                            "triggering reconnect",
                            config_.pong_timeout.count());
                        // Signal RX thread to reconnect by resetting TCP.
                        // RX will detect the broken connection and handle reconnect.
                        tcp_->reset();
                        ping_awaiting_pong_ = false;
                        continue;
                    }
                }

                if (now - last_ping >= config_.ping_interval) {
                    if (send_ws_ping(ws_buf, tls_bufs_storage.get())) {
                        ping_awaiting_pong_ = true;
                    }
                    last_ping = now;
                }
            }

            // Drain: consume as many messages as available, up to kMaxBatch.
            // Uses try_consume_n for amortized atomic operations (single
            // head update for the entire batch vs one per message).
            int n = static_cast<int>(tx_queue_.try_consume_n(
                static_cast<size_t>(kMaxBatch),
                [&](TxMsg& msg, [[maybe_unused]] size_t idx) {
                    batch[idx] = msg;
                }));

            if (n == 0) {
                // If RX signaled a graceful close and the queue is now
                // empty, the Close response has been sent — exit.
                if (closing_.load(std::memory_order_acquire)) [[unlikely]] {
                    SPDLOG_LOGGER_DEBUG(log,
                        "TX: closing_ set and queue drained, exiting");
                    running_.store(false, std::memory_order_release);
                    break;
                }
                eph::utils::cpu_relax();
                continue;
            }

            // WS encode -> [TLS encrypt] -> TCP send for each message in batch
            for (int i = 0; i < n; ++i) {
                size_t ws_len;

                // Use precomputed template for the common case (binary),
                // fall back to encode_frame for other opcodes (text, pong)
                // to ensure the correct opcode is written into the frame.
                if (batch[i].opcode == ws::opcode::kBinary) {
                    ws_len = ws_tmpl.encode(
                        ws_buf, batch[i].data, batch[i].len);
                } else {
                    ws_len = ws::encode_frame(
                        ws_buf, batch[i].opcode,
                        batch[i].data, batch[i].len);
                }

                if (config_.use_tls) {
                    uint8_t* tls_buf_i = tls_bufs_storage.get() +
                        static_cast<size_t>(i) * kTlsBufSize;
                    tls_lens[i] = crypto_->encrypt(
                        ws_buf, static_cast<uint16_t>(ws_len),
                        tls_buf_i);

                    if (tls_lens[i] == 0) {
                        tx_stats_.crypto_errors++;
                    }
                } else {
                    // Plain WS: send WS frame directly over TCP
                    auto result = tcp_->send(ws_buf, ws_len);
                    if (!result) {
                        tx_stats_.dropped++;
                        SPDLOG_LOGGER_WARN(log,
                            "TCP send failed (dropped): {}", result.error());
                    } else {
                        tx_stats_.packets++;
                        tx_stats_.bytes += batch[i].len;
                        if (batch[i].opcode == ws::opcode::kText) {
                            tx_stats_.text_packets++;
                            tx_stats_.text_bytes += batch[i].len;
                        }
                    }
                }
            }

            // Send all encrypted packets through TCP (TLS mode only)
            if (config_.use_tls) {
                for (int i = 0; i < n; ++i) {
                    if (tls_lens[i] == 0) continue;

                    uint8_t* tls_buf_i = tls_bufs_storage.get() +
                        static_cast<size_t>(i) * kTlsBufSize;
                    auto result = tcp_->send(tls_buf_i, tls_lens[i]);
                    if (!result) {
                        tx_stats_.dropped++;
                        SPDLOG_LOGGER_WARN(log,
                            "TCP send failed (dropped): {}", result.error());
                    } else {
                        tx_stats_.packets++;
                        tx_stats_.bytes += batch[i].len;
                        if (batch[i].opcode == ws::opcode::kText) {
                            tx_stats_.text_packets++;
                            tx_stats_.text_bytes += batch[i].len;
                        }
                    }
                }
            }
        }

        SPDLOG_LOGGER_DEBUG(log, "TX loop exited");
    }

    // -----------------------------------------------------------------------
    // RX worker loop (runs on dedicated thread)
    // -----------------------------------------------------------------------

    void rx_loop() {
        eph::utils::set_thread_affinity(config_.rx_cpu, "RX");
        auto log = detail::transport_logger();
        SPDLOG_LOGGER_DEBUG(log, "RX loop started");

        // Fixed-size RX buffers -- no heap allocation on hot path.
        // TLS reassembly: accumulates raw TCP bytes until complete TLS records form.
        // Sized for 2x max TLS record to handle partial records at boundary.
        static constexpr size_t kReassemblyBufSize =
            2 * (tls_const::kMaxRecordPayload + tls_record::kRecordHeaderLen +
                 tls_record::kAuthTagLen + 1);
        auto decrypt_buf = std::make_unique<uint8_t[]>(
            tls_const::kMaxRecordPayload + 256);
        auto reassembly_storage = std::make_unique<uint8_t[]>(kReassemblyBufSize);
        size_t reassembly_len = 0;

        // WS reassembly: accumulates decrypted bytes when a WebSocket frame
        // spans multiple TLS records. Without this, partial WS frames at
        // TLS record boundaries would be silently discarded.
        static constexpr size_t kWsReassemblyBufSize =
            ws::kMaxFrameHeaderLen + MaxPayload + 256;
        auto ws_reassembly_storage = std::make_unique<uint8_t[]>(kWsReassemblyBufSize);
        size_t ws_reassembly_len = 0;

        while (running_.load(std::memory_order_acquire)) {
            // After a server Close frame, stop receiving — TX will
            // drain the Close response and set running_=false.
            if (closing_.load(std::memory_order_acquire)) [[unlikely]] {
                eph::utils::cpu_relax();
                continue;
            }

            // -- Receive data via poll_rx --
            bool reconnect_needed = false;
            auto rx_result = tcp_->poll_rx(
                [&](const uint8_t* data, uint16_t len) {
                    if (config_.use_tls) {
                        // TLS mode: accumulate into TLS reassembly buffer
                        if (reassembly_len + len <= kReassemblyBufSize) {
                            std::memcpy(reassembly_storage.get() + reassembly_len,
                                        data, len);
                            reassembly_len += len;
                        } else {
                            SPDLOG_LOGGER_ERROR(log,
                                "RX reassembly buffer overflow ({} + {} > {}), "
                                "triggering reconnect",
                                reassembly_len, len, kReassemblyBufSize);
                            reassembly_len = 0;
                            ws_reassembly_len = 0;
                            reconnect_needed = true;
                        }
                    } else {
                        // Plain WS mode: accumulate into WS reassembly buffer
                        if (ws_reassembly_len + len <= kWsReassemblyBufSize) {
                            std::memcpy(ws_reassembly_storage.get() + ws_reassembly_len,
                                        data, len);
                            ws_reassembly_len += len;
                        } else {
                            SPDLOG_LOGGER_ERROR(log,
                                "WS reassembly buffer overflow ({} + {} > {}), "
                                "triggering reconnect",
                                ws_reassembly_len, len, kWsReassemblyBufSize);
                            ws_reassembly_len = 0;
                            reconnect_needed = true;
                        }
                    }
                });

            // Reassembly buffer overflow -> reconnect
            if (reconnect_needed) {
                if (!do_reconnect()) {
                    running_.store(false, std::memory_order_release);
                    break;
                }
                continue;
            }

            if (!rx_result) {
                SPDLOG_LOGGER_WARN(log, "TCP rx error: {}",
                                   rx_result.error());

                // -- Auto-reconnect (fixed interval, discard old messages) --
                reassembly_len = 0;
                ws_reassembly_len = 0;
                if (do_reconnect()) {
                    continue; // Resume RX loop with new connection
                }

                // Reconnect exhausted -- stop transport
                running_.store(false, std::memory_order_release);
                break;
            }

            // No data received this poll iteration
            if (*rx_result == 0) continue;

            // Plain WS mode: process WS frames directly from TCP data
            if (!config_.use_tls) {
                // Use ws_reassembly buffer to handle partial WS frames
                size_t ws_consumed = process_ws_data(
                    ws_reassembly_storage.get(), ws_reassembly_len);

                // Save unconsumed WS bytes for next TCP chunk
                size_t ws_remaining = ws_reassembly_len - ws_consumed;
                if (ws_remaining > 0 && ws_consumed > 0) {
                    std::memmove(ws_reassembly_storage.get(),
                                 ws_reassembly_storage.get() + ws_consumed,
                                 ws_remaining);
                }
                ws_reassembly_len = ws_remaining;
                continue;
            }

            // Decrypt complete TLS records from reassembly buffer
            size_t consumed = 0;
            while (reassembly_len - consumed >=
                   tls_record::kRecordHeaderLen + tls_record::kAuthTagLen) {
                const uint8_t* rec_ptr = reassembly_storage.get() + consumed;

                uint8_t content_type;
                uint16_t payload_len;
                if (!tls_record::parse_record_header(
                        rec_ptr, content_type, payload_len)) {
                    break;
                }

                size_t record_total = tls_record::kRecordHeaderLen + payload_len;
                if (reassembly_len - consumed < record_total) break;

                uint16_t decrypted_len;
                bool ok = crypto_->decrypt(
                    rec_ptr,
                    static_cast<uint16_t>(record_total),
                    decrypt_buf.get(), decrypted_len);

                if (!ok) {
                    rx_stats_.crypto_errors++;
                    SPDLOG_LOGGER_WARN(log,
                        "TLS decrypt failed -- triggering reconnect");
                    // Corrupted record -> link unreliable, reconnect.
                    // Reset reassembly state and skip compact logic below.
                    reassembly_len = 0;
                    ws_reassembly_len = 0;
                    consumed = 0;
                    if (!do_reconnect()) {
                        running_.store(false, std::memory_order_release);
                    }
                    break; // Resume with fresh connection or exit outer loop
                }

                // Prepend any leftover WS bytes from the previous TLS record.
                // This handles WS frames that span TLS record boundaries.
                const uint8_t* ws_data;
                size_t ws_data_len;
                if (ws_reassembly_len > 0) {
                    // Append new decrypted data after existing WS leftovers
                    if (ws_reassembly_len + decrypted_len <= kWsReassemblyBufSize) {
                        std::memcpy(ws_reassembly_storage.get() + ws_reassembly_len,
                                    decrypt_buf.get(), decrypted_len);
                        ws_reassembly_len += decrypted_len;
                    } else {
                        SPDLOG_LOGGER_WARN(log,
                            "WS reassembly buffer overflow ({} + {} > {}), "
                            "discarding partial frame",
                            ws_reassembly_len, decrypted_len,
                            kWsReassemblyBufSize);
                        ws_reassembly_len = 0;
                        consumed += record_total;
                        continue;
                    }
                    ws_data = ws_reassembly_storage.get();
                    ws_data_len = ws_reassembly_len;
                } else {
                    ws_data = decrypt_buf.get();
                    ws_data_len = decrypted_len;
                }

                size_t ws_consumed = process_ws_data(ws_data, ws_data_len);

                // Save unconsumed WS bytes for next TLS record
                size_t ws_remaining = ws_data_len - ws_consumed;
                if (ws_remaining > 0) {
                    if (ws_data == decrypt_buf.get()) {
                        // First time: copy leftovers into WS reassembly buffer
                        std::memcpy(ws_reassembly_storage.get(),
                                    decrypt_buf.get() + ws_consumed, ws_remaining);
                    } else {
                        // Already in WS reassembly buffer: compact to front
                        std::memmove(ws_reassembly_storage.get(),
                                     ws_reassembly_storage.get() + ws_consumed,
                                     ws_remaining);
                    }
                    ws_reassembly_len = ws_remaining;
                } else {
                    ws_reassembly_len = 0;
                }

                consumed += record_total;
            }

            // Compact: move unconsumed data to front (memmove, not erase)
            if (consumed > 0) {
                reassembly_len -= consumed;
                if (reassembly_len > 0) {
                    std::memmove(reassembly_storage.get(),
                                 reassembly_storage.get() + consumed,
                                 reassembly_len);
                }
            }
        }

        SPDLOG_LOGGER_DEBUG(log, "RX loop exited");
    }

    /// Send a WebSocket ping frame (called from TX thread only).
    /// Uses caller-provided buffers to avoid extra stack allocations.
    /// Records TSC timestamp for RTT measurement by the RX thread.
    bool send_ws_ping(uint8_t* ws_buf, uint8_t* tls_buf) noexcept {
        size_t ping_len = ws::build_ping_frame(ws_buf);

        // Record TSC just before TCP send for tightest RTT measurement
        last_ping_tsc_.store(eph::utils::TSC::now(), std::memory_order_relaxed);

        if (config_.use_tls) {
            uint16_t tls_len = crypto_->encrypt(
                ws_buf, static_cast<uint16_t>(ping_len), tls_buf);
            if (tls_len == 0) return false;

            auto result = tcp_->send(tls_buf, tls_len);
            if (!result) {
                SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                    "WS ping send failed: {}", result.error());
                return false;
            }
        } else {
            auto result = tcp_->send(ws_buf, ping_len);
            if (!result) {
                SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                    "WS ping send failed: {}", result.error());
                return false;
            }
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // WebSocket frame processing
    // -----------------------------------------------------------------------

    /// Process decrypted WebSocket data. Returns the number of bytes
    /// consumed. Unconsumed bytes (partial frames) must be preserved
    /// by the caller and prepended to the next chunk of decrypted data.
    size_t process_ws_data(const uint8_t* data, size_t len) {
        auto log = detail::transport_logger();
        size_t offset = 0;

        while (offset < len) {
            auto frame = ws::decode_frame(data + offset, len - offset);
            if (!frame) {
                if (frame.error() == "incomplete") break;
                SPDLOG_LOGGER_WARN(log, "WS frame decode error: {}",
                                   frame.error());
                break;
            }

            offset += frame->total_len;
            rx_stats_.packets++;

            if (frame->is_ping()) {
                ws_pings_received_.fetch_add(1, std::memory_order_relaxed);
                if (config_.on_ping) {
                    try {
                        config_.on_ping(frame->payload,
                                        static_cast<uint16_t>(frame->payload_len));
                    } catch (...) {
                        SPDLOG_LOGGER_WARN(log,
                            "on_ping callback threw an exception");
                    }
                }
                handle_ping(*frame);
                continue;
            }

            if (frame->is_close()) {
                uint16_t code = frame->close_status_code();
                // Extract close reason from payload (after 2-byte status code)
                std::string_view close_reason;
                if (frame->payload && frame->payload_len > 2) {
                    close_reason = std::string_view(
                        reinterpret_cast<const char*>(frame->payload + 2),
                        frame->payload_len - 2);
                }
                SPDLOG_LOGGER_INFO(log,
                    "Received WS Close frame: code={} reason=\"{}\"",
                    code, close_reason);
                // Notify application of close reason before responding
                if (config_.on_close) {
                    try {
                        config_.on_close(code, close_reason);
                    } catch (...) {
                        SPDLOG_LOGGER_WARN(log,
                            "on_close callback threw an exception");
                    }
                }
                // Deliver close frame to RX queue so polling-mode users
                // can detect server-initiated close via try_recv_msg().
                // The close payload (2-byte code + optional reason) is
                // accessible via ReceivedMessage::close_code()/close_reason().
                if (frame->payload && frame->payload_len > 0 &&
                    frame->payload_len <= MaxPayload) {
                    rx_queue_.try_produce([&](RxMsg& msg) {
                        std::memcpy(msg.data, frame->payload,
                                    frame->payload_len);
                        msg.len = static_cast<uint16_t>(frame->payload_len);
                        msg.opcode = ws::opcode::kClose;
                    });
                }
                // RFC 6455 §5.5.1: respond with a Close frame echoing
                // the status code before shutting down.
                handle_close(code);
                // Signal TX to drain the Close response before exiting.
                // TX checks closing_ and sends remaining queue items.
                closing_.store(true, std::memory_order_release);
                break;
            }

            if (frame->is_pong()) {
                // Record pong arrival for timeout detection (TX thread reads this).
                last_pong_ns_.store(
                    std::chrono::steady_clock::now().time_since_epoch().count(),
                    std::memory_order_relaxed);

                // RTT measurement: compute delta from the ping TSC timestamp.
                // Only record if TSC is initialized and we have a valid ping timestamp.
                uint64_t ping_tsc = last_ping_tsc_.load(std::memory_order_relaxed);
                if (ping_tsc > 0 && eph::utils::TSC::is_initialized()) {
                    uint64_t pong_tsc = eph::utils::TSC::now();
                    if (pong_tsc > ping_tsc) {
                        auto rtt_ns = eph::utils::TSC::to_ns(pong_tsc - ping_tsc);
                        if (rtt_ns) {
                            rtt_histogram_.record(
                                static_cast<uint64_t>(*rtt_ns));
                        }
                    }
                    // Clear ping TSC so we don't double-record on spurious pongs
                    last_ping_tsc_.store(0, std::memory_order_relaxed);
                }

                if (config_.on_pong) {
                    try {
                        config_.on_pong(frame->payload,
                                        static_cast<uint16_t>(frame->payload_len));
                    } catch (...) {
                        SPDLOG_LOGGER_WARN(detail::transport_logger(),
                            "on_pong callback threw an exception");
                    }
                }
                continue;
            }

            // Data frame handling with fragmentation reassembly.
            // RFC 6455 §5.4: first fragment has opcode != 0, FIN=0;
            // continuation fragments have opcode=0; final fragment has FIN=1.
            if (!frame->is_data()) continue;

            // Unmask payload in-place if needed (server frames are usually
            // unmasked, but handle masked frames for robustness).
            // payload pointer is const; we'll unmask during copy below.

            if (frame->opcode != ws::opcode::kContinuation) {
                // Start of a new message (possibly the only frame if FIN=1)
                if (!ws_frag_buf_.empty()) {
                    SPDLOG_LOGGER_WARN(log,
                        "New WS message started while previous fragment "
                        "incomplete, discarding {} buffered bytes",
                        ws_frag_buf_.size());
                    ws_frag_buf_.clear();
                }
                ws_frag_opcode_ = frame->opcode;
            }

            // Append payload to fragment buffer (or process directly if
            // single-frame message).
            bool is_final = frame->fin;
            bool is_single_frame = (frame->opcode != ws::opcode::kContinuation
                                    && is_final);

            if (is_single_frame && frame->payload_len <= MaxPayload) {
                // Fast path: complete single-frame message, no buffering
                deliver_data_frame(*frame);
            } else if (is_single_frame) {
                // Single oversized frame
                rx_stats_.dropped++;
                SPDLOG_LOGGER_WARN(log,
                    "Dropping oversized WS frame: payload_len={}, "
                    "max={}, opcode=0x{:02x}",
                    frame->payload_len, MaxPayload, frame->opcode);
            } else {
                // Fragmented message: accumulate
                size_t new_size = ws_frag_buf_.size() + frame->payload_len;
                if (new_size > MaxPayload) {
                    rx_stats_.dropped++;
                    SPDLOG_LOGGER_WARN(log,
                        "Dropping oversized fragmented WS message: "
                        "accumulated={}, max={}", new_size, MaxPayload);
                    ws_frag_buf_.clear();
                    continue;
                }

                if (frame->payload && frame->payload_len > 0) {
                    size_t old_size = ws_frag_buf_.size();
                    ws_frag_buf_.resize(new_size);
                    std::memcpy(ws_frag_buf_.data() + old_size,
                                frame->payload, frame->payload_len);
                    if (frame->masked) {
                        ws::apply_mask(
                            ws_frag_buf_.data() + old_size,
                            frame->payload_len, frame->mask_key);
                    }
                }

                if (is_final) {
                    // Reassembly complete — deliver
                    if (!ws_frag_buf_.empty()) {
                        deliver_message(
                            ws_frag_buf_.data(),
                            static_cast<uint16_t>(ws_frag_buf_.size()),
                            ws_frag_opcode_);
                    }
                    ws_frag_buf_.clear();
                }
            }
        }
        return offset;
    }

    /// Deliver a decoded payload to either the on_message callback or the RX queue.
    /// Text frames are validated for UTF-8 compliance (RFC 6455 §5.6);
    /// invalid frames are dropped with a warning.
    void deliver_message(const uint8_t* data, uint16_t len, uint8_t opcode) {
        // RFC 6455 §5.6: text frames must contain valid UTF-8
        if (opcode == ws::opcode::kText && !ws::is_valid_utf8(data, len)) {
            rx_stats_.dropped++;
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Dropping text frame with invalid UTF-8 (len={})", len);
            return;
        }
        auto update_rx_stats = [&] {
            rx_stats_.bytes += len;
            if (opcode == ws::opcode::kText) {
                rx_stats_.text_packets++;
                rx_stats_.text_bytes += len;
            }
        };

        if (config_.on_message) {
            try {
                config_.on_message(data, len, opcode);
            } catch (...) {
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "on_message callback threw an exception");
            }
            update_rx_stats();
            return;
        }

        bool ok = rx_queue_.try_produce([&](RxMsg& msg) {
            std::memcpy(msg.data, data, len);
            msg.len = len;
            msg.opcode = opcode;
        });

        if (ok) {
            update_rx_stats();
        } else {
            rx_stats_.dropped++;
            if (rx_stats_.dropped % 1000 == 1) {
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "RX queue full, dropping data frame "
                    "(total dropped: {})", rx_stats_.dropped);
            }
            if (config_.on_rx_drop) {
                try {
                    config_.on_rx_drop(rx_stats_.dropped);
                } catch (...) {
                    // Callback must not throw
                }
            }
        }
    }

    /// Deliver a complete single-frame data message.
    void deliver_data_frame(const ws::DecodedFrame& frame) {
        if (frame.payload_len == 0) return;

        // For masked frames, unmask into a temp buffer before delivery
        if (frame.masked) {
            uint8_t tmp[MaxPayload];
            std::memcpy(tmp, frame.payload, frame.payload_len);
            ws::apply_mask(tmp, frame.payload_len, frame.mask_key);
            deliver_message(tmp, static_cast<uint16_t>(frame.payload_len),
                            frame.opcode);
        } else {
            deliver_message(frame.payload, static_cast<uint16_t>(frame.payload_len),
                            frame.opcode);
        }
    }

    /// Enqueue pong response into TX queue so the TX thread sends it.
    /// This avoids data races: only TX thread touches crypto_->encrypt().
    void handle_ping(const ws::DecodedFrame& ping_frame) {
        // Ping payload is at most 125 bytes (RFC 6455 §5.5).
        // Enqueue the unmasked payload with kPong opcode; TX thread
        // will encode the WS frame and encrypt it.
        size_t pong_payload_len = std::min(
            static_cast<size_t>(ping_frame.payload_len),
            static_cast<size_t>(MaxPayload));

        bool ok = tx_queue_.try_produce([&](TxMsg& msg) {
            if (ping_frame.payload && pong_payload_len > 0) {
                std::memcpy(msg.data, ping_frame.payload, pong_payload_len);
                if (ping_frame.masked) {
                    ws::apply_mask(msg.data, pong_payload_len,
                                   ping_frame.mask_key);
                }
            }
            msg.len = static_cast<uint16_t>(pong_payload_len);
            msg.opcode = ws::opcode::kPong;
        });

        if (ok) {
            ws_pongs_sent_.fetch_add(1, std::memory_order_relaxed);
        } else {
            SPDLOG_LOGGER_DEBUG(detail::transport_logger(),
                "TX queue full, dropping pong response");
        }
    }

    /// Enqueue a Close frame response into the TX queue.
    /// Called from RX thread when a server Close frame is received.
    void handle_close(uint16_t status_code) {
        // Encode the 2-byte status code as payload; TX thread will wrap
        // it in a WS Close frame via encode_frame(kClose, ...).
        tx_queue_.try_produce([&](TxMsg& msg) {
            msg.data[0] = static_cast<uint8_t>(status_code >> 8);
            msg.data[1] = static_cast<uint8_t>(status_code & 0xFF);
            msg.len = 2;
            msg.opcode = ws::opcode::kClose;
        });
    }
};

} // namespace eph::net
