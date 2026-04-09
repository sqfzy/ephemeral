#pragma once

/// @file transport.hpp
/// Generic WebSocket transport over any TcpTransport backend (threaded variant).
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
///
/// Composition:
///   Transport composes four value members:
///     TransportCore<TcpImpl>  -- shared connection state (TCP, TLS, config)
///     TxWorker<...>           -- TX thread, TX queue, TX stats
///     RxWorker<...>           -- RX thread, RX queue, RX stats
///     ReconnectPolicy         -- exponential backoff reconnect strategy

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/containers/bounded_queue.hpp"
#include "eph/containers/evicting_queue.hpp"
#include "eph/core/framer_concept.hpp"
#include "eph/core/tcp_concept.hpp"
#include "eph/transport/detail/transport_core.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/transport/detail/tx_worker.hpp"
#include "eph/transport/detail/rx_worker.hpp"
#include "eph/transport/reconnect_policy.hpp"
#include "eph/transport/detail/tls_record.hpp"
#include "eph/transport/detail/websocket.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/transport/detail/message_types.hpp"

namespace eph::net {

// kEnableTimestamps is defined once in transport_types.hpp (included above).

// ---------------------------------------------------------------------------
// Transport -- public API (threaded variant, composed from workers)
// ---------------------------------------------------------------------------

/// Generic WebSocket transport with TLS 1.3 encryption (threaded variant).
///
/// This is the default operating mode: dedicated TX and RX threads with
/// SPSC queues for decoupled, non-blocking send/recv from the application
/// thread.
///
/// Template parameters:
///   TcpImpl    -- a type satisfying the TcpTransport concept
///   Framer     -- message framer (default: WsFramer for WebSocket)
///   MaxPayload -- maximum application payload size per message
///   QueueDepth -- SPSC queue capacity (must be power of 2)
///   RxQueueTmpl -- RX queue template (BoundedQueue or EvictingQueue)
///   LastOnlyDeliver -- when true, only the last WS data frame per batch
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
/// @tparam LastOnlyDeliver  When true, only the last WS data frame per
///   process_ws_data() call is delivered; intermediate frames are decoded
///   but skipped.  Useful for single-symbol streams where only the latest
///   value matters.  For multi-symbol combined streams, set to false so
///   every message reaches the application.
template <TcpTransport TcpImpl, MessageFramer Framer = WsFramer,
          size_t MaxPayload = 512, size_t QueueDepth = 1024,
          template <typename, size_t> class RxQueueTmpl =
              eph::containers::BoundedQueue,
          bool LastOnlyDeliver = false>
class Transport {
    static_assert(TcpTransport<TcpImpl>,
                  "TcpImpl must satisfy TcpTransport concept");
    static_assert(MessageFramer<Framer>,
                  "Framer must satisfy MessageFramer concept");
    static_assert(MaxPayload > 0, "MaxPayload must be > 0");
    static_assert(MaxPayload <= tls_const::kMaxRecordPayload,
                  "MaxPayload exceeds TLS max record size (16384)");
    static_assert(std::has_single_bit(QueueDepth),
                  "QueueDepth must be power of 2");

    /// True when using WebSocket framing (enables WS handshake, ping/pong,
    /// close handshake, and fragmentation reassembly).
    static constexpr bool kIsWebSocket = std::is_same_v<Framer, WsFramer>;

    /// True when the RX queue uses evicting (latest-value) semantics.
    static constexpr bool kRxEvicting =
        std::same_as<RxQueueTmpl<int, 2>,
                     eph::containers::EvictingQueue<int, 2>>;

    /// Controls whether only the last WS data frame per batch is delivered.
    /// Independent of queue type — EvictingQueue can deliver all frames
    /// (multi-symbol) or only the last (single-symbol).
    static constexpr bool kLastOnlyDeliver = LastOnlyDeliver;

    // -- Fixed capability constants (threaded variant always has everything) --
    static constexpr bool kHasTxThread = true;
    static constexpr bool kHasRxThread = true;
    static constexpr bool kHasTxQueue  = true;
    static constexpr bool kHasRxQueue  = true;

    // -- Component type aliases -----------------------------------------------
    using TxWorkerT = TxWorker<TcpImpl, Framer, MaxPayload, QueueDepth>;
    using RxWorkerT = RxWorker<TcpImpl, Framer, MaxPayload, QueueDepth,
                               RxQueueTmpl, LastOnlyDeliver>;
    using TxMsg = detail::TxMessage<MaxPayload>;
    using RxMsg = detail::RxMessage<MaxPayload>;

public:
    /// Factory callable: creates a new, already-connected TcpImpl instance.
    /// Called during initial connect and on each reconnection attempt.
    using TcpFactory = std::function<
        std::expected<std::unique_ptr<TcpImpl>, std::string>()>;

    /// Received message with opcode metadata (delegates to RxWorker's definition).
    using ReceivedMessage = typename RxWorkerT::ReceivedMessage;

    static constexpr size_t max_payload() noexcept { return MaxPayload; }
    static constexpr size_t queue_depth() noexcept { return QueueDepth; }
    static constexpr bool   timestamps_enabled() noexcept { return kEnableTimestamps; }

    // =======================================================================
    // Factory
    // =======================================================================

    /// Create and connect a transport (TCP + TLS + WebSocket handshake).
    /// This is a blocking call -- performs the full handshake sequence.
    /// Returns unique_ptr because Transport owns threads and is non-movable.
    ///
    /// On failure, returns ConnectionErrorInfo with a typed error code
    /// for programmatic handling and a detail string for logging.
    [[nodiscard]] static std::expected<std::unique_ptr<Transport>, ConnectionErrorInfo>
    create(TcpFactory tcp_factory, const TransportConfig& config) {
        [[maybe_unused]] auto log = detail::transport_logger();

        if (!tcp_factory) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kInvalidConfig, "tcp_factory is null"});
        }
        if (auto err = config.validate(); !err.empty()) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kInvalidConfig, std::string(err)});
        }

        // Log non-fatal config warnings before proceeding
        for (const auto& w : config.warnings()) {
            SPDLOG_LOGGER_WARN(log, "Config warning: {}", w);
        }

        SPDLOG_LOGGER_INFO(log,
            "Creating transport: {}:{}{}",
            config.remote_host, config.remote_port, config.ws_path);

        // Construct Transport with all components
        auto t = std::unique_ptr<Transport>(new Transport(
            std::move(tcp_factory), config));

        // Phase 1: TCP + TLS + WS handshake via TransportCore
        auto conn_result = t->core_.do_connect();
        if (!conn_result) {
            SPDLOG_LOGGER_ERROR(log, "Initial connect failed: {}",
                                conn_result.error().message());
            return std::unexpected(conn_result.error());
        }

        // Phase 2: WebSocket upgrade (only for WsFramer)
        if constexpr (kIsWebSocket) {
            auto ws_result = t->core_.template do_ws_upgrade<Framer>();
            if (!ws_result) {
                SPDLOG_LOGGER_ERROR(log, "WS upgrade failed: {}",
                                    ws_result.error().message());
                return std::unexpected(ws_result.error());
            }
        }

        t->core_.created_at = std::chrono::steady_clock::now();
        t->core_.running.store(true, std::memory_order_release);
        t->core_.notify_state(TransportEvent::kConnected, config.remote_host);

        // Flush any pending TCP ACK accumulated during handshake.
        // Critical for deferred_start: without this, the server's TCP
        // window fills up and it stops sending data before RX threads start.
        if constexpr (requires { t->core_.tcp->flush_pending_ack(); }) {
            t->core_.tcp->flush_pending_ack();
        }

        // Hook: allow caller to configure session (e.g., shared RX ring)
        // after handshake but before threads start polling.
        if (config.on_connected_before_threads) {
            config.on_connected_before_threads();
        }

        // Deferred start: if requested, don't start threads now.
        // Caller must call start_threads() later.
        if (!config.deferred_start) {
            t->start_threads();
        }

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

    // =======================================================================
    // Send API (application thread) -> delegates to tx_
    // =======================================================================

    /// Send data as a WebSocket frame (non-blocking, best-effort).
    ///
    /// Semantics: kOk means the message was enqueued in the TX SPSC queue,
    /// NOT that it was sent on the wire. If the connection drops between
    /// enqueue and TX thread transmission, the message is silently lost.
    /// For at-least-once delivery guarantees, implement application-level
    /// acknowledgment on top of this API.
    ///
    /// @param data     Payload data
    /// @param len      Payload length (must be <= MaxPayload)
    /// @param opcode   WebSocket opcode (default: binary)
    /// @return SendError::kOk on enqueue success, or a specific error code
    [[nodiscard]] SendError send(const void* data, size_t len,
                   uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        // RFC 6455 §5.6: text frames must contain valid UTF-8
        if (opcode == ws::opcode::kText && !core_.config.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return tx_.enqueue(data, len, opcode);
    }

    /// Send data from a span (convenience overload).
    [[nodiscard]] SendError send(std::span<const uint8_t> data,
                   uint8_t opcode = ws::opcode::kBinary) noexcept {
        return send(data.data(), data.size(), opcode);
    }

    /// Send data as a WebSocket binary frame (convenience, explicit intent).
    [[nodiscard]] SendError send_binary(const void* data, size_t len) noexcept {
        return send(data, len, ws::opcode::kBinary);
    }

    /// Send data as a WebSocket text frame (convenience for JSON APIs).
    /// Validates UTF-8 encoding per RFC 6455 §5.6 unless
    /// TransportConfig::skip_utf8_validation is true. Returns kInvalidUtf8
    /// if validation is enabled and the payload is not valid UTF-8.
    [[nodiscard]] SendError send_text(const void* data, size_t len) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (!core_.config.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return tx_.enqueue(data, len, ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket text frame (convenience for JSON APIs).
    /// Validates UTF-8 encoding per RFC 6455 §5.6 unless
    /// TransportConfig::skip_utf8_validation is true.
    [[nodiscard]] SendError send_text(std::string_view sv) noexcept {
        if (!core_.config.skip_utf8_validation && !ws::is_valid_utf8(sv)) {
            return SendError::kInvalidUtf8;
        }
        return tx_.enqueue(sv.data(), sv.size(), ws::opcode::kText);
    }

    /// Send a text frame WITHOUT UTF-8 validation (unchecked).
    ///
    /// Use this when you know the payload is valid UTF-8 (e.g., ASCII-only
    /// JSON) and want to skip the validation overhead on the hot path.
    /// If the payload is not valid UTF-8, the remote peer may close the
    /// connection per RFC 6455 §5.6 — this is the caller's responsibility.
    [[nodiscard]] SendError send_text_unchecked(const void* data, size_t len) noexcept {
        return tx_.enqueue(data, len, ws::opcode::kText);
    }

    /// Send a string_view as an unchecked text frame (no UTF-8 validation).
    [[nodiscard]] SendError send_text_unchecked(std::string_view sv) noexcept {
        return tx_.enqueue(sv.data(), sv.size(), ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket binary frame.
    [[nodiscard]] SendError send_binary(std::string_view sv) noexcept {
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
    [[nodiscard]] SendError send_for(const void* data, size_t len,
                       std::chrono::duration<Rep, Period> timeout,
                       uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        // RFC 6455 §5.6: text frames must contain valid UTF-8
        if (opcode == ws::opcode::kText && !core_.config.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return tx_.enqueue_for(data, len, timeout, opcode);
    }

    /// Send data from a span with timeout (convenience overload).
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_for(std::span<const uint8_t> data,
                       std::chrono::duration<Rep, Period> timeout,
                       uint8_t opcode = ws::opcode::kBinary) noexcept {
        return send_for(data.data(), data.size(), timeout, opcode);
    }

    /// Send a WebSocket text frame with timeout and UTF-8 validation.
    /// Combines send_text()'s UTF-8 check with send_for()'s backpressure wait.
    /// Validation is skipped when TransportConfig::skip_utf8_validation is true.
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_text_for(const void* data, size_t len,
                            std::chrono::duration<Rep, Period> timeout) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (!core_.config.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return tx_.enqueue_for(data, len, timeout, ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket text frame with timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_text_for(std::string_view sv,
                            std::chrono::duration<Rep, Period> timeout) noexcept {
        if (!core_.config.skip_utf8_validation && !ws::is_valid_utf8(sv)) {
            return SendError::kInvalidUtf8;
        }
        return tx_.enqueue_for(sv.data(), sv.size(), timeout, ws::opcode::kText);
    }

    /// Send a WebSocket binary frame with timeout (convenience, explicit intent).
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_binary_for(const void* data, size_t len,
                              std::chrono::duration<Rep, Period> timeout) noexcept {
        return send_for(data, len, timeout, ws::opcode::kBinary);
    }

    /// Send a span as a WebSocket binary frame with timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_binary_for(std::span<const uint8_t> data,
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
    [[nodiscard]] SendError send_close(uint16_t status_code,
                         std::string_view reason = {}) noexcept {
        if (!core_.running.load(std::memory_order_acquire)) return SendError::kNotConnected;
        if (!ws::is_valid_close_code(status_code)) return SendError::kInvalidCloseCode;
        // RFC 6455 §7.1.6: close reason must be valid UTF-8
        if (!reason.empty() && !ws::is_valid_utf8(reason)) return SendError::kInvalidUtf8;

        // Close payload: 2-byte status code + optional reason (max 123 chars per RFC 6455 §5.5)
        size_t reason_len = std::min(reason.size(), size_t{123});
        if (reason_len < reason.size()) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Close reason truncated from {} to 123 bytes (RFC 6455 §5.5 limit)",
                reason.size());
        }
        uint16_t payload_len = static_cast<uint16_t>(2 + reason_len);

        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

        // Build close payload and enqueue via TX queue directly
        bool ok = tx_.queue().try_produce([&](TxMsg& msg) {
            msg.data[0] = static_cast<uint8_t>(status_code >> 8);
            msg.data[1] = static_cast<uint8_t>(status_code & 0xFF);
            if (reason_len > 0) {
                std::memcpy(msg.data.data() + 2, reason.data(), reason_len);
            }
            msg.len = payload_len;
            msg.opcode = ws::opcode::kClose;
            if constexpr (kEnableTimestamps) {
                msg.tsc = eph::utils::TSC::now();
            }
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
    [[nodiscard]] SendError send_ping(const void* payload = nullptr,
                        size_t payload_len = 0) noexcept {
        if (!core_.running.load(std::memory_order_acquire)) return SendError::kNotConnected;

        // RFC 6455 §5.5: control frame payload MUST NOT exceed 125 bytes
        size_t original_len = payload_len;
        payload_len = std::min(payload_len, size_t{125});
        if (original_len > 125) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Ping payload truncated from {} to 125 bytes (RFC 6455 §5.5 limit)",
                original_len);
        }
        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

        bool ok = tx_.queue().try_produce([&](TxMsg& msg) {
            if (payload && payload_len > 0) {
                std::memcpy(msg.data.data(), payload, payload_len);
            }
            msg.len = static_cast<uint16_t>(payload_len);
            msg.opcode = ws::opcode::kPing;
            if constexpr (kEnableTimestamps) {
                msg.tsc = eph::utils::TSC::now();
            }
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
    [[nodiscard]] SendError send_n(const std::span<const uint8_t>* payloads, size_t count,
                     uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (!core_.running.load(std::memory_order_acquire)) return SendError::kNotConnected;
        if (count > 0 && !payloads) [[unlikely]] return SendError::kNullData;

        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxPayload) return SendError::kMessageTooLarge;
            // RFC 6455 §5.6: text frames must contain valid UTF-8
            if (opcode == ws::opcode::kText && !core_.config.skip_utf8_validation &&
                !ws::is_valid_utf8(payloads[i].data(), payloads[i].size())) {
                return SendError::kInvalidUtf8;
            }
        }

        return tx_.enqueue_batch(payloads, count, opcode);
    }

    /// Batch-send with timeout — waits up to `timeout` for queue space.
    ///
    /// Like send_n() but uses try_produce_n_for() to spin briefly for
    /// queue capacity rather than returning kQueueFull immediately.
    ///
    /// @param payloads  Array of {data, len} pairs
    /// @param count     Number of messages
    /// @param timeout   Maximum time to wait for queue space
    /// @param opcode    WebSocket opcode for all messages
    /// @return SendError::kOk on success, or a specific error code
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_n_for(const std::span<const uint8_t>* payloads, size_t count,
                         std::chrono::duration<Rep, Period> timeout,
                         uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (!core_.running.load(std::memory_order_acquire)) return SendError::kNotConnected;
        if (count > 0 && !payloads) [[unlikely]] return SendError::kNullData;

        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxPayload) return SendError::kMessageTooLarge;
            // RFC 6455 §5.6: text frames must contain valid UTF-8
            if (opcode == ws::opcode::kText && !core_.config.skip_utf8_validation &&
                !ws::is_valid_utf8(payloads[i].data(), payloads[i].size())) {
                return SendError::kInvalidUtf8;
            }
        }

        return tx_.enqueue_batch_for(payloads, count, timeout, opcode);
    }

    // =======================================================================
    // Receive API (application thread) -> delegates to rx_
    //
    // Two receive modes coexist — choose one per application:
    //   - Pull (recv/recv_n/drain_recv): application polls the RX queue.
    //     Use this for latency-sensitive hot loops (DPDK, trading).
    //   - Push (on_message callback in TransportConfig): RX thread invokes
    //     the callback directly. Simpler but adds indirection. Use this
    //     for event-driven applications (socket backend, non-critical path).
    //
    // Both modes can be active simultaneously — on_message fires first in
    // the RX thread, then the message is enqueued for recv() consumption.
    // =======================================================================

    /// Try to receive a message (non-blocking).
    /// @param callback  Called with (data_ptr, len) if a message is available.
    /// @return true if a message was consumed, false if queue empty.
    /// @warning The data pointer passed to callback is only valid for the
    ///          duration of the callback invocation. Copy the data if you
    ///          need it after the callback returns -- the underlying SPSC
    ///          queue slot will be reused.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool recv(F&& callback) {
        return rx_.recv(std::forward<F>(callback));
    }

    /// Try to receive a message with opcode (non-blocking).
    /// @param callback  Called with (data_ptr, len, opcode) if a message is available.
    ///                  opcode is one of ws::opcode::kBinary, ws::opcode::kText, etc.
    /// @return true if a message was consumed, false if queue empty.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool recv(F&& callback) {
        return rx_.recv(std::forward<F>(callback));
    }

    /// Try to receive a message with opcode and arrival timestamp (non-blocking).
    /// @param callback  Called with (data_ptr, len, opcode, arrival_tsc).
    ///                  arrival_tsc is the raw TSC cycle count captured at
    ///                  rx_burst time (RX thread). Use TSC::to_ns(now - tsc)
    ///                  in the callback to compute per-frame RX latency.
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    /// @return true if a message was consumed, false if queue empty.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t, uint64_t>
    [[nodiscard]] bool recv(F&& callback) {
        return rx_.recv(std::forward<F>(callback));
    }

    /// Try to receive a message as a copied byte vector (non-blocking).
    /// Returns the payload bytes, or nullopt if the queue is empty.
    /// Prefer the callback variant for zero-copy hot paths.
    [[nodiscard]] std::optional<std::vector<uint8_t>> try_recv() {
        return rx_.try_recv();
    }

    /// Try to receive a message with opcode info (non-blocking).
    /// Returns payload + opcode, or nullopt if the queue is empty.
    [[nodiscard]] std::optional<ReceivedMessage> try_recv_msg() {
        return rx_.try_recv_msg();
    }

    // -----------------------------------------------------------------------
    // Peek API (application thread) — inspect without consuming
    //
    // Peek at the next RX message without removing it from the queue.
    // Useful for message-type routing: inspect the header, then decide
    // whether to consume with recv() or skip. Multiple peeks return the
    // same message until recv()/try_recv() advances the head.
    //
    // @note Reader-thread only (same thread that calls recv()).
    // -----------------------------------------------------------------------

    /// Peek at the next message without consuming (non-blocking).
    /// @param callback  Called with (data_ptr, len) if a message is available.
    /// @return true if a message was peeked, false if queue empty.
    /// @warning The data pointer is only valid during the callback invocation.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool recv_peek(F&& callback) {
        return rx_.recv_peek(std::forward<F>(callback));
    }

    /// Peek at the next message with opcode without consuming (non-blocking).
    /// @param callback  Called with (data_ptr, len, opcode) if a message is available.
    /// @return true if a message was peeked, false if queue empty.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool recv_peek(F&& callback) {
        return rx_.recv_peek(std::forward<F>(callback));
    }

    /// Peek at the next message as a copied ReceivedMessage (non-blocking).
    /// Returns nullopt if the queue is empty.
    [[nodiscard]] std::optional<ReceivedMessage> peek_recv_msg() {
        return rx_.peek_recv_msg();
    }

    /// Batch-receive up to max_count messages (non-blocking, best-effort).
    ///
    /// Calls callback with (data_ptr, len) for each available message,
    /// consuming up to max_count from the RX queue in a single drain loop.
    ///
    /// @note Not available when RxQueue is EvictingQueue (latest-value semantics).
    ///       Use recv() in a loop instead.
    ///
    /// @param callback    Called with (data_ptr, len) for each message
    /// @param max_count   Maximum number of messages to consume
    /// @return Number of messages actually consumed
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t> && !kRxEvicting)
    [[nodiscard]] size_t recv_n(F&& callback, size_t max_count) {
        return rx_.recv_n(std::forward<F>(callback), max_count);
    }

    /// Batch-receive up to max_count messages with opcode (non-blocking).
    ///
    /// Uses try_consume_n for amortized atomic operations (single head
    /// update for the entire batch, matching send_n's try_produce_n).
    ///
    /// @note Not available when RxQueue is EvictingQueue.
    ///
    /// @param callback    Called with (data_ptr, len, opcode) for each message
    /// @param max_count   Maximum number of messages to consume
    /// @return Number of messages actually consumed
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t, uint8_t> && !kRxEvicting)
    [[nodiscard]] size_t recv_n(F&& callback, size_t max_count) {
        return rx_.recv_n(std::forward<F>(callback), max_count);
    }

    /// Drain all available messages (non-blocking).
    /// Equivalent to recv_n(callback, queue_depth()).
    /// @note Not available when RxQueue is EvictingQueue.
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t> && !kRxEvicting)
    [[nodiscard]] size_t drain_recv(F&& callback) {
        return rx_.drain_recv(std::forward<F>(callback));
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
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool wait_recv(F&& callback, std::chrono::milliseconds timeout) {
        return rx_.wait_recv(std::forward<F>(callback), timeout);
    }

    /// Blocking receive with opcode and timeout.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool wait_recv(F&& callback, std::chrono::milliseconds timeout) {
        return rx_.wait_recv(std::forward<F>(callback), timeout);
    }

    /// Blocking receive returning a ReceivedMessage with opcode and timeout.
    /// Returns nullopt on timeout or transport stopped.
    [[nodiscard]] std::optional<ReceivedMessage> wait_recv_msg(
            std::chrono::milliseconds timeout) {
        return rx_.wait_recv_msg(timeout);
    }

    // =======================================================================
    // Lifecycle
    // =======================================================================

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
        // Bail early if the transport is not running — nothing to close.
        if (!core_.running.load(std::memory_order_acquire)) return false;
        // Store close code/reason so stop() can propagate them in the
        // final Close frame instead of using a hardcoded default.
        // Write code/reason BEFORE setting close_requested_ (release)
        // so that stop() sees consistent values after acquire load (M9).
        core_.pending_close_code = status_code;
        core_.pending_close_reason = std::string(reason);
        core_.close_requested.store(true, std::memory_order_release);

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
        while (core_.running.load(std::memory_order_acquire) &&
               !core_.closing.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "close_gracefully: timed out waiting for server Close "
                    "response ({}ms)", timeout.count());
                stop();
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        // Server responded with Close — stop cleanly
        stop();
        return true;
    }

    /// Start RX/TX worker threads. Only needed when TransportConfig::deferred_start
    /// is true. Must be called exactly once after create() returns.
    void start_threads() {
        tx_.start();
        rx_.start();
    }

    /// Stop the transport gracefully. Sends WebSocket Close frame.
    ///
    /// Thread safety: waits for TX/RX threads to exit BEFORE touching
    /// crypto_ or tcp_, avoiding data races on shared state.
    void stop() noexcept {
        bool was_running = core_.running.exchange(false, std::memory_order_acq_rel);

        [[maybe_unused]] auto log = detail::transport_logger();
        SPDLOG_LOGGER_INFO(log, "Stopping transport");

        // Wait for RX thread to exit any in-progress reconnect before joining.
        // Without this guard, do_reconnect_() may be accessing crypto/tcp
        // pointers when tx_.stop()/rx_.stop() attempt to join the threads,
        // causing a use-after-free race (P0-1 production hardening).
        if (was_running) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (core_.reconnecting.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() > deadline) {
                    SPDLOG_LOGGER_ERROR(log,
                        "stop: reconnect still in progress after 5s, "
                        "forcing TCP reset to unblock");
                    if (core_.tcp) core_.tcp->reset();
                    break;
                }
                eph::utils::cpu_relax();
            }
        }

        // Join worker threads — ensures no concurrent access to
        // crypto/tcp from TX/RX threads when we send the Close frame.
        tx_.stop();
        rx_.stop();

        // Send WebSocket Close frame after threads have exited (no race)
        // Only applicable when using WsFramer — other framers have no
        // close handshake at the framing layer.
        if constexpr (kIsWebSocket) {
            if (was_running && core_.tcp && core_.tcp->is_established() &&
                (core_.config.use_tls ? core_.crypto != nullptr : true)) {
                // Acquire-load close_requested to synchronize with the
                // release-store in close_gracefully(), ensuring we see
                // the code/reason written by the app thread (M9).
                uint16_t close_code = ws::close_code::kNormal;
                std::string_view close_reason = "client shutdown";
                if (core_.close_requested.load(std::memory_order_acquire)) {
                    close_code = core_.pending_close_code;
                    if (!core_.pending_close_reason.empty())
                        close_reason = core_.pending_close_reason;
                }
                // Close frame: header (max 14) + payload (max 125) = 139 bytes,
                // plus 1 byte for encrypt()'s temporary content type append.
                uint8_t close_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
                size_t close_len = ws::build_close_frame(
                    close_buf, close_code, close_reason);

                if (core_.config.use_tls) {
                    // TLS output: record header + ciphertext + content type + auth tag
                    uint8_t tls_buf[TlsRecordCrypto::encrypted_size(
                        ws::kMaxFrameHeaderLen + 125)]{};
                    uint16_t tls_len = core_.crypto->encrypt(
                        close_buf, static_cast<uint16_t>(close_len), tls_buf);
                    if (tls_len > 0) {
                        (void)core_.tcp->send(tls_buf, tls_len);
                    }
                } else {
                    // Plain WS: send close frame directly over TCP
                    (void)core_.tcp->send(close_buf, close_len);
                }
            }
        }

        // Close TCP connection
        if (core_.tcp && core_.tcp->is_established()) {
            (void)core_.tcp->close();
        }

        core_.notify_state(TransportEvent::kStopped);
        SPDLOG_LOGGER_INFO(log, "Transport stopped");
    }

    /// True between successful create() and stop() — does not distinguish
    /// connected vs reconnecting. Use is_connected() or state() for finer-grained status.
    [[nodiscard]] bool is_running() const noexcept {
        return core_.running.load(std::memory_order_acquire);
    }

    /// Read-only access to the configuration used to create this transport.
    /// Useful for logging, diagnostics, and reconnection-aware logic that
    /// needs to inspect remote_host, ping_interval, etc. at runtime.
    [[nodiscard]] const TransportConfig& config() const noexcept {
        return core_.config;
    }

    /// Query the current connection state (lock-free, safe from any thread).
    [[nodiscard]] TransportState state() const noexcept {
        if (!core_.running.load(std::memory_order_acquire))
            return TransportState::kStopped;
        if (core_.reconnecting.load(std::memory_order_acquire))
            return TransportState::kReconnecting;
        return TransportState::kConnected;
    }

    /// Check if the transport is connected and data can flow.
    [[nodiscard]] bool is_connected() const noexcept {
        return state() == TransportState::kConnected;
    }

    /// Force an immediate reconnection attempt.
    ///
    /// Useful when the application detects stale data (e.g., market data
    /// feed going silent while TCP remains established) and wants to
    /// reconnect without waiting for pong timeout or TCP-level detection.
    ///
    /// The reconnect happens asynchronously in the RX thread. This method
    /// returns immediately after signaling. The transport must be running
    /// and auto-reconnect must be enabled (max_reconnect_attempts > 0).
    ///
    /// @return true if the reconnect was signaled, false if the transport
    ///         is not running or auto-reconnect is disabled.
    [[nodiscard]] bool reconnect_now() noexcept {
        if (!core_.running.load(std::memory_order_acquire)) return false;
        if (core_.config.max_reconnect_attempts <= 0) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "reconnect_now() called but auto-reconnect is disabled "
                "(max_reconnect_attempts=0)");
            return false;
        }
        SPDLOG_LOGGER_INFO(detail::transport_logger(),
            "reconnect_now() signaled by application");
        core_.force_reconnect.store(true, std::memory_order_release);
        return true;
    }

    // =======================================================================
    // Queue occupancy (backpressure monitoring) -> delegates to workers
    // =======================================================================

    /// Approximate number of messages pending in the TX queue.
    /// Useful for detecting backpressure before send() returns -EAGAIN.
    /// @note Result is approximate — the producer and consumer may
    ///       advance between the size() read and the caller's use.
    [[nodiscard]] size_t tx_queue_size() const noexcept {
        return tx_.queue_size();
    }

    /// Approximate number of messages available in the RX queue.
    [[nodiscard]] size_t rx_queue_size() const noexcept {
        return rx_.queue_size();
    }

    /// TX queue occupancy as a fraction [0.0, 1.0].
    [[nodiscard]] double tx_queue_fill_ratio() const noexcept {
        return tx_.queue_fill_ratio();
    }

    /// RX queue occupancy as a fraction [0.0, 1.0].
    [[nodiscard]] double rx_queue_fill_ratio() const noexcept {
        return rx_.queue_fill_ratio();
    }

    /// Peak TX queue occupancy since creation or last reset_stats().
    /// Useful for diagnosing transient backpressure spikes that
    /// instantaneous tx_queue_size() would miss.
    [[nodiscard]] size_t tx_queue_hwm() const noexcept {
        return tx_.queue_hwm();
    }

    /// Peak RX queue occupancy since creation or last reset_stats().
    [[nodiscard]] size_t rx_queue_hwm() const noexcept {
        return rx_.queue_hwm();
    }

    // =======================================================================
    // Statistics -> aggregates tx_ + rx_ + app-thread counters
    // =======================================================================

    /// Reset all statistics counters to zero.
    /// Useful for windowed measurement: call stats(), then reset_stats().
    /// @warning Not thread-safe with stats() — call from one thread only
    ///          (typically the application thread between measurement windows).
    void reset_stats() noexcept {
        tx_.reset_stats();
        rx_.reset_stats();
        queue_full_count_.store(0, std::memory_order_relaxed);
        pong_timeouts_.store(0, std::memory_order_relaxed);
        reconnect_count_.store(0, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // Connection metadata -> from core_
    // -----------------------------------------------------------------------

    /// Negotiated TLS version string (e.g. "TLSv1.3"), or "none" if not connected.
    [[nodiscard]] std::string_view tls_version() const noexcept {
        return core_.tls_version;
    }

    /// Negotiated cipher suite name (e.g. "TLS_AES_256_GCM_SHA384"), or "none".
    [[nodiscard]] std::string_view cipher_name() const noexcept {
        return core_.cipher_name;
    }

    /// Negotiated WebSocket subprotocol (from server's Sec-WebSocket-Protocol
    /// response header), or empty string if none was negotiated.
    [[nodiscard]] std::string_view ws_subprotocol() const noexcept {
        return core_.ws_subprotocol;
    }

    /// Resolved remote IP address (e.g. "10.0.0.1") from the last connection.
    /// Available if the underlying TcpImpl exposes a resolved_ip() method.
    /// Returns empty string if not available or not yet connected.
    [[nodiscard]] std::string_view remote_ip() const noexcept {
        return core_.remote_ip;
    }

    /// Aggregated connection metadata snapshot.
    /// Combines tls_version, cipher_name, ws_subprotocol, and remote_ip
    /// into a single struct for convenient logging and monitoring.
    [[nodiscard]] ConnectionInfo connection_info() const {
        return ConnectionInfo{
            .tls_version    = std::string(core_.tls_version),
            .cipher_name    = std::string(core_.cipher_name),
            .ws_subprotocol = std::string(core_.ws_subprotocol),
            .remote_ip      = std::string(core_.remote_ip),
            .remote_port    = core_.config.remote_port,
            .use_tls        = core_.config.use_tls,
        };
    }

    // -----------------------------------------------------------------------
    // Latency histograms -> delegates to workers
    // -----------------------------------------------------------------------

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
        return histogram_to_stats_(rx_.latency_histogram());
    }

    /// TX queue latency stats (enqueue -> flush).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats tx_latency_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "tx_latency_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats_(tx_.latency_histogram());
    }

    /// TX queue wait stats (enqueue -> drain).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats tx_queue_wait_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "tx_queue_wait_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats_(tx_.queue_wait_histogram());
    }

    /// TX encode+encrypt stats (drain -> flush).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats tx_encode_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "tx_encode_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats_(tx_.encode_histogram());
    }

    /// RX pipeline latency stats (arrival -> deliver).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats rx_latency_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_latency_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats_(rx_.latency_histogram());
    }

    /// RX decrypt stats (arrival -> decrypt done).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats rx_decrypt_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_decrypt_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats_(rx_.decrypt_histogram());
    }

    /// RX decode stats (decrypt done -> frame decoded).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats rx_decode_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_decode_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats_(rx_.decode_histogram());
    }

    /// Snapshot the RX pipeline latency histogram for windowed measurement.
    /// Use HdrHistogram::subtract() to compute per-window delta:
    ///   auto h1 = tp.rx_latency_histogram_snapshot();
    ///   /* ... wait ... */
    ///   auto h2 = tp.rx_latency_histogram_snapshot();
    ///   h2.subtract(h1);  // h2 now contains only the window's samples
    /// @warning Brief race with RX thread — acceptable for monitoring.
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] eph::utils::HdrHistogram rx_latency_histogram_snapshot() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_latency_histogram_snapshot() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return rx_.latency_histogram();
    }

    // -----------------------------------------------------------------------
    // Aggregated stats snapshot
    // -----------------------------------------------------------------------

    [[nodiscard]] TransportStats stats() const noexcept {
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - core_.created_at).count();

        auto tx_s = tx_.stats();
        auto rx_s = rx_.stats();

        return TransportStats{
            .tx_packets        = tx_s.packets,
            .tx_bytes          = tx_s.bytes,
            .tx_text_packets   = tx_s.text_packets,
            .tx_text_bytes     = tx_s.text_bytes,
            .tx_dropped        = tx_s.dropped,
            .rx_packets        = rx_s.rx_packets,
            .rx_bytes          = rx_s.rx_bytes,
            .rx_text_packets   = rx_s.rx_text_packets,
            .rx_text_bytes     = rx_s.rx_text_bytes,
            .rx_dropped        = rx_s.rx_dropped,
            .tcp_rx_packets    = [this]() -> uint64_t {
                if constexpr (requires { core_.tcp->tcp_stats(); })
                    return core_.tcp ? core_.tcp->tcp_stats().rx_packets : 0;
                else return 0;
            }(),
            .tcp_rx_bursts     = [this]() -> uint64_t {
                if constexpr (requires { core_.tcp->tcp_stats(); })
                    return core_.tcp ? core_.tcp->tcp_stats().rx_bursts : 0;
                else return 0;
            }(),
            .encrypt_errors    = tx_s.crypto_errors,
            .decrypt_errors    = rx_s.decrypt_errors,
            .queue_full_count  = queue_full_count_.load(std::memory_order_relaxed)
                               + tx_s.queue_full_count,
            .ws_pings_received = rx_s.ws_pings_received,
            .ws_pongs_sent     = rx_s.ws_pongs_sent,
            .pong_timeouts     = pong_timeouts_.load(std::memory_order_relaxed),
            .reconnect_count   = reconnect_count_.load(std::memory_order_relaxed),
            .tx_queue_hwm      = tx_s.queue_hwm,
            .rx_queue_hwm      = rx_s.rx_queue_hwm,
            .uptime_ns         = static_cast<uint64_t>(uptime > 0 ? uptime : 0),
            .handshake_ns      = core_.last_handshake_ns,
            .tcp_connect_ns    = core_.last_tcp_connect_ns,
            .tls_handshake_ns  = core_.last_tls_handshake_ns,
            .ws_upgrade_ns     = core_.last_ws_upgrade_ns,
            .remote_ip         = core_.remote_ip,
            .rtt               = rtt_stats(),
            .tx_latency        = histogram_to_stats_(tx_.latency_histogram()),
            .tx_queue_wait     = histogram_to_stats_(tx_.queue_wait_histogram()),
            .tx_encode         = histogram_to_stats_(tx_.encode_histogram()),
            .rx_latency        = histogram_to_stats_(rx_.latency_histogram()),
            .rx_decrypt        = histogram_to_stats_(rx_.decrypt_histogram()),
            .rx_decode         = histogram_to_stats_(rx_.decode_histogram()),
            .tls_write_seq     = core_.crypto ? core_.crypto->write_seq() : 0,
            .tls_read_seq      = core_.crypto ? core_.crypto->read_seq() : 0,
            .tls_seq_limit     = core_.config.use_tls ? tls_record::kMaxSequenceNumber : 0,
        };
    }

private:
    // =======================================================================
    // Construction (private — use create() factory)
    // =======================================================================

    Transport(TcpFactory tcp_factory, const TransportConfig& config)
        : core_{} // core_.config set via init_core_config_ below
        , tx_(core_, pong_timeouts_)
        , rx_(core_, reconnect_count_, typename RxWorkerT::Callbacks{
            .do_reconnect = [this]() -> bool { return do_reconnect_(); },
            .send_response = [this](const void* data, size_t len,
                                    uint8_t opcode) -> SendError {
                return tx_.enqueue(data, len, opcode);
            },
          })
        // ReconnectPolicy holds a const& to core_.config for reading
        // max_reconnect_attempts, reconnect_interval, and callbacks during
        // reconnection attempts. We must reference core_.config (not the
        // constructor parameter) because the parameter's lifetime ends with
        // the caller's scope, while Transport (and its reconnect_ member)
        // may outlive it.  core_.config is copied from the parameter in the
        // body below, before any reconnect attempt can occur.
        , reconnect_(init_core_config_(config))
    {
        core_.tcp_factory = std::move(tcp_factory);
    }

    /// Set core_.config and return a reference to it, for use in the
    /// initializer list to ensure ReconnectPolicy binds to the owned copy.
    const TransportConfig& init_core_config_(const TransportConfig& config) {
        core_.config = config;
        return core_.config;
    }

    // =======================================================================
    // Reconnection logic
    // =======================================================================

    /// Attempt reconnection with exponential backoff and jitter.
    /// Discards old SPSC queue data. Called from RX thread when disconnect
    /// is detected. Returns true if reconnection succeeded.
    bool do_reconnect_() {
        [[maybe_unused]] auto log = detail::transport_logger();

        // Guard: if stop() has already been called, do not enter reconnect.
        // This closes the race window between running=false and thread join
        // where do_reconnect_() could access freed crypto/tcp pointers.
        if (!core_.running.load(std::memory_order_acquire)) {
            SPDLOG_LOGGER_DEBUG(log, "do_reconnect: running=false, skipping");
            return false;
        }

        int max_attempts = core_.config.max_reconnect_attempts;
        if (max_attempts <= 0) {
            SPDLOG_LOGGER_ERROR(log, "Auto-reconnect disabled, stopping");
            return false;
        }

        core_.notify_state(TransportEvent::kDisconnected, core_.config.remote_host);

        // Record disconnect time for downtime measurement
        auto disconnect_time = std::chrono::steady_clock::now();

        // Signal TX thread to pause: it must not touch crypto/tcp
        // while we are reconnecting.
        core_.reconnecting.store(true, std::memory_order_release);

        // Discard stale queue data and reset worker state
        tx_.on_reconnected();
        rx_.on_reconnected();
        core_.closing.store(false, std::memory_order_release);

        // Reset reconnect policy for this reconnection cycle
        reconnect_.reset();

        bool success = false;
        while (!reconnect_.exhausted()) {
            // Check running flag each attempt — stop() may have been called
            if (!core_.running.load(std::memory_order_acquire)) {
                SPDLOG_LOGGER_DEBUG(log, "do_reconnect: stop() called during reconnect, aborting");
                core_.reconnecting.store(false, std::memory_order_release);
                return false;
            }
            core_.notify_state(TransportEvent::kReconnecting,
                std::format("{}/{}", reconnect_.attempts() + 1, max_attempts));

            // Clean up old connection state
            core_.crypto.reset();
            core_.tls.reset();
            core_.tcp.reset();

            success = reconnect_.attempt([this]()
                -> std::expected<void, ConnectionErrorInfo>
            {
                auto result = core_.do_connect();
                if (!result) return result;

                if constexpr (kIsWebSocket) {
                    return core_.template do_ws_upgrade<Framer>();
                }
                return {};
            });

            if (success) {
                auto total = reconnect_count_.fetch_add(1, std::memory_order_relaxed) + 1;
                core_.reconnecting.store(false, std::memory_order_release);
                core_.notify_state(TransportEvent::kConnected,
                    std::format("reconnect attempt {}", reconnect_.attempts()));

                auto downtime_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - disconnect_time).count());

                SPDLOG_LOGGER_INFO(log,
                    "Reconnected successfully on attempt {} (downtime: {:.1f}ms)",
                    reconnect_.attempts(),
                    static_cast<double>(downtime_ns) / 1e6);

                // Notify application — ideal for replaying subscriptions
                if (core_.config.on_reconnected) {
                    try {
                        core_.config.on_reconnected(
                            reconnect_.attempts(), downtime_ns,
                            static_cast<uint64_t>(total));
                    } catch (const std::exception& e) {
                        SPDLOG_LOGGER_ERROR(log,
                            "on_reconnected callback threw: {}", e.what());
                    } catch (...) {
                        SPDLOG_LOGGER_ERROR(log,
                            "on_reconnected callback threw non-std::exception");
                    }
                }
                return true;
            }
        }

        core_.reconnecting.store(false, std::memory_order_release);
        SPDLOG_LOGGER_ERROR(log,
            "All {} reconnect attempts exhausted", max_attempts);
        return false;
    }

    // =======================================================================
    // Utility helpers
    // =======================================================================

    /// Convert an HdrHistogram to RttStats (reused for RTT, TX latency, RX latency).
    static RttStats histogram_to_stats_(const eph::utils::HdrHistogram& h) noexcept {
        if (h.get_total_count() == 0) return {};
        return RttStats{
            .count   = h.get_total_count(),
            .min_ns  = h.get_min_value(),
            .max_ns  = h.get_max_value(),
            .mean_ns = h.get_mean(),
            .p50_ns  = h.get_value_at_percentile(50.0),
            .p99_ns  = h.get_value_at_percentile(99.0),
            .p999_ns = h.get_value_at_percentile(99.9),
        };
    }

    // =======================================================================
    // Composed members
    // =======================================================================

    TransportCore<TcpImpl> core_;
    TxWorkerT tx_;
    RxWorkerT rx_;
    ReconnectPolicy reconnect_;

    // App-thread counters (not owned by workers — written by send() path)
    std::atomic<uint64_t> queue_full_count_{0};
    std::atomic<uint64_t> reconnect_count_{0};
    std::atomic<uint64_t> pong_timeouts_{0};
};

} // namespace eph::net
