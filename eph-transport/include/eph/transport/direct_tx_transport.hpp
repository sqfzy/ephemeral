#pragma once

/// @file direct_tx_transport.hpp
/// DirectTxTransport — RX thread + RX queue, direct TX from app thread.
///
/// Composes TransportCore (connection state), RxWorker (RX thread + queue),
/// and ReconnectPolicy (exponential backoff). NO TxWorker — all sends go
/// directly from the application thread via send_direct_().
///
/// Thread model:
///   - Application thread: calls send()/recv(), send is blocking (direct)
///   - RX thread: busy-poll TCP rx, decrypt, parse WS frames, push to recv queue
///   - NO TX thread or TX queue

#include <atomic>
#include <chrono>
#include <cstdint>
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
#include "eph/transport/transport_core.hpp"
#include "eph/transport/rx_worker.hpp"
#include "eph/transport/reconnect_policy.hpp"
#include "eph/transport/tls_encryptor.hpp"
#include "eph/transport/tls_record.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/transport/websocket.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/transport/detail/message_types.hpp"
#include "eph/utils/hdr_histogram.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// DirectTxTransport -- public API
// ---------------------------------------------------------------------------

/// DirectTxTransport: RX thread + RX queue, direct TX from app thread.
///
/// Template parameters:
///   TcpImpl       -- a type satisfying the TcpTransport concept
///   Framer        -- message framer (default: WsFramer for WebSocket)
///   MaxPayload    -- maximum application payload size per message
///   QueueDepth    -- RX SPSC queue capacity (must be power of 2)
///   RxQueueTmpl   -- RX queue template (BoundedQueue or EvictingQueue)
///   LastOnlyDeliver -- when true, only the last WS data frame per
///                      process_ws_data() call is delivered
template <TcpTransport TcpImpl, MessageFramer Framer = WsFramer,
          size_t MaxPayload = 512, size_t QueueDepth = 1024,
          template <typename, size_t> class RxQueueTmpl =
              eph::containers::BoundedQueue,
          bool LastOnlyDeliver = false>
class DirectTxTransport {
    static_assert(TcpTransport<TcpImpl>,
                  "TcpImpl must satisfy TcpTransport concept");
    static_assert(MessageFramer<Framer>,
                  "Framer must satisfy MessageFramer concept");
    static_assert(MaxPayload > 0, "MaxPayload must be > 0");
    static_assert(MaxPayload <= tls_const::kMaxRecordPayload,
                  "MaxPayload exceeds TLS max record size (16384)");
    static_assert(std::has_single_bit(QueueDepth),
                  "QueueDepth must be power of 2");

    /// True when using WebSocket framing.
    static constexpr bool kIsWebSocket = std::is_same_v<Framer, WsFramer>;

    using RxWorkerT = RxWorker<TcpImpl, Framer, MaxPayload, QueueDepth,
                               RxQueueTmpl, LastOnlyDeliver>;

public:
    /// Factory callable: creates a new, already-connected TcpImpl instance.
    using TcpFactory = typename TransportCore<TcpImpl>::TcpFactory;

    /// Received message with opcode metadata (delegated from RxWorker).
    using ReceivedMessage = typename RxWorkerT::ReceivedMessage;

    static constexpr size_t max_payload() noexcept { return MaxPayload; }
    static constexpr size_t queue_depth() noexcept { return QueueDepth; }
    static constexpr bool   timestamps_enabled() noexcept {
        return kEnableTimestamps;
    }

    /// Create and connect a DirectTxTransport (TCP + TLS + WebSocket handshake).
    /// Blocking call — performs the full handshake sequence.
    /// Returns unique_ptr because DirectTxTransport owns threads and is non-movable.
    [[nodiscard]] static std::expected<std::unique_ptr<DirectTxTransport>, ConnectionErrorInfo>
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

        for (const auto& w : config.warnings()) {
            SPDLOG_LOGGER_WARN(log, "Config warning: {}", w);
        }

        SPDLOG_LOGGER_INFO(log,
            "Creating DirectTxTransport: {}:{}{}",
            config.remote_host, config.remote_port, config.ws_path);

        auto t = std::unique_ptr<DirectTxTransport>(new DirectTxTransport(config));
        t->core_.tcp_factory = std::move(tcp_factory);

        // Full handshake: TCP + TLS + WS Upgrade
        auto conn_result = t->core_.do_connect();
        if (!conn_result) {
            SPDLOG_LOGGER_ERROR(log, "Initial connect failed: {}",
                                conn_result.error().message());
            return std::unexpected(conn_result.error());
        }

        auto ws_result = t->core_.template do_ws_upgrade<Framer>();
        if (!ws_result) {
            SPDLOG_LOGGER_ERROR(log, "WS upgrade failed: {}",
                                ws_result.error().message());
            return std::unexpected(ws_result.error());
        }

        t->core_.created_at = std::chrono::steady_clock::now();
        t->core_.running.store(true, std::memory_order_release);
        t->core_.notify_state(TransportEvent::kConnected, config.remote_host);

        // Flush any pending TCP ACK accumulated during handshake.
        if constexpr (requires { t->core_.tcp->flush_pending_ack(); }) {
            t->core_.tcp->flush_pending_ack();
        }

        // Hook: allow caller to configure session before threads start.
        if (config.on_connected_before_threads) {
            config.on_connected_before_threads();
        }

        // Start RX thread (unless deferred_start requested)
        if (!config.deferred_start) {
            t->start_threads();
        }

        SPDLOG_LOGGER_INFO(log, "DirectTxTransport ready: {}",
                           config.remote_host);
        return t;
    }

    ~DirectTxTransport() {
        stop();
    }

    DirectTxTransport(const DirectTxTransport&)            = delete;
    DirectTxTransport& operator=(const DirectTxTransport&) = delete;
    DirectTxTransport(DirectTxTransport&&)                 = delete;
    DirectTxTransport& operator=(DirectTxTransport&&)      = delete;

    // -----------------------------------------------------------------------
    // Send API (application thread -- direct send, no queue)
    // -----------------------------------------------------------------------

    /// Send data as a WebSocket frame (blocking -- sends directly from app thread).
    ///
    /// Semantics: kOk means the message was framed, encrypted, and handed
    /// to the TCP layer. No SPSC queue is involved.
    ///
    /// @param data     Payload data
    /// @param len      Payload length (must be <= MaxPayload)
    /// @param opcode   WebSocket opcode (default: binary)
    /// @return SendError::kOk on success, or a specific error code
    [[nodiscard]] SendError send(const void* data, size_t len,
                   uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (opcode == ws::opcode::kText && !core_.config.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return send_direct_(data, len, opcode);
    }

    /// Send data from a span (convenience overload).
    [[nodiscard]] SendError send(std::span<const uint8_t> data,
                   uint8_t opcode = ws::opcode::kBinary) noexcept {
        return send(data.data(), data.size(), opcode);
    }

    /// Send data as a WebSocket binary frame.
    [[nodiscard]] SendError send_binary(const void* data, size_t len) noexcept {
        return send(data, len, ws::opcode::kBinary);
    }

    /// Send data as a WebSocket text frame with UTF-8 validation.
    [[nodiscard]] SendError send_text(const void* data, size_t len) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (!core_.config.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return send_direct_(data, len, ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket text frame.
    [[nodiscard]] SendError send_text(std::string_view sv) noexcept {
        if (!core_.config.skip_utf8_validation && !ws::is_valid_utf8(sv)) {
            return SendError::kInvalidUtf8;
        }
        return send_direct_(sv.data(), sv.size(), ws::opcode::kText);
    }

    /// Send a text frame WITHOUT UTF-8 validation (unchecked).
    [[nodiscard]] SendError send_text_unchecked(const void* data, size_t len) noexcept {
        return send_direct_(data, len, ws::opcode::kText);
    }

    /// Send a string_view as an unchecked text frame.
    [[nodiscard]] SendError send_text_unchecked(std::string_view sv) noexcept {
        return send_direct_(sv.data(), sv.size(), ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket binary frame.
    [[nodiscard]] SendError send_binary(std::string_view sv) noexcept {
        return send(sv.data(), sv.size(), ws::opcode::kBinary);
    }

    /// Send data with timeout -- timeout is ignored (no queue), sends directly.
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_for(const void* data, size_t len,
                       std::chrono::duration<Rep, Period> timeout,
                       uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (opcode == ws::opcode::kText && !core_.config.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        (void)timeout;
        return send_direct_(data, len, opcode);
    }

    /// Send data from a span with timeout (convenience overload).
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_for(std::span<const uint8_t> data,
                       std::chrono::duration<Rep, Period> timeout,
                       uint8_t opcode = ws::opcode::kBinary) noexcept {
        return send_for(data.data(), data.size(), timeout, opcode);
    }

    /// Send a WebSocket text frame with timeout (timeout ignored, sends directly).
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_text_for(const void* data, size_t len,
                            std::chrono::duration<Rep, Period> timeout) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (!core_.config.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        (void)timeout;
        return send_direct_(data, len, ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket text frame with timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_text_for(std::string_view sv,
                            std::chrono::duration<Rep, Period> timeout) noexcept {
        if (!core_.config.skip_utf8_validation && !ws::is_valid_utf8(sv)) {
            return SendError::kInvalidUtf8;
        }
        (void)timeout;
        return send_direct_(sv.data(), sv.size(), ws::opcode::kText);
    }

    /// Send a WebSocket binary frame with timeout.
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
    SendError send_close(uint16_t status_code,
                         std::string_view reason = {}) noexcept {
        if (!core_.running.load(std::memory_order_acquire)) return SendError::kNotConnected;
        if (!ws::is_valid_close_code(status_code)) return SendError::kInvalidCloseCode;
        if (!reason.empty() && !ws::is_valid_utf8(reason)) return SendError::kInvalidUtf8;

        size_t reason_len = std::min(reason.size(), size_t{123});
        if (reason_len < reason.size()) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Close reason truncated from {} to 123 bytes (RFC 6455 S5.5 limit)",
                reason.size());
        }
        uint16_t payload_len = static_cast<uint16_t>(2 + reason_len);
        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

        // Direct send: encode close frame and send immediately
        uint8_t close_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
        size_t close_len = ws::build_close_frame(close_buf, status_code, reason);
        if (core_.config.use_tls && core_.crypto) {
            uint8_t tls_buf[TlsEncryptor::encrypted_size(
                ws::kMaxFrameHeaderLen + 125)];
            uint16_t enc_len = core_.crypto->enc.encrypt(
                close_buf, static_cast<uint16_t>(close_len), tls_buf);
            if (enc_len > 0) {
                core_.tcp->send(tls_buf, enc_len);
            } else {
                SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                    "send_close: encrypt failed for status_code={}", status_code);
                return SendError::kEncryptFailed;
            }
        } else {
            core_.tcp->send(close_buf, close_len);
        }
        return SendError::kOk;
    }

    /// Send a WebSocket Ping frame.
    SendError send_ping(const void* payload = nullptr,
                        size_t payload_len = 0) noexcept {
        if (!core_.running.load(std::memory_order_acquire)) return SendError::kNotConnected;

        size_t original_len = payload_len;
        payload_len = std::min(payload_len, size_t{125});
        if (original_len > 125) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Ping payload truncated from {} to 125 bytes (RFC 6455 S5.5 limit)",
                original_len);
        }
        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

        // Direct send: encode ping frame and send immediately
        uint8_t ping_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
        size_t ping_len = ws::build_ping_frame(ping_buf, payload, payload_len);
        if (core_.config.use_tls && core_.crypto) {
            uint8_t tls_buf[TlsEncryptor::encrypted_size(
                ws::kMaxFrameHeaderLen + 125)];
            uint16_t enc_len = core_.crypto->enc.encrypt(
                ping_buf, static_cast<uint16_t>(ping_len), tls_buf);
            if (enc_len > 0) {
                core_.tcp->send(tls_buf, enc_len);
            } else {
                SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                    "send_ping: encrypt failed, payload_len={}", payload_len);
                return SendError::kEncryptFailed;
            }
        } else {
            core_.tcp->send(ping_buf, ping_len);
        }
        return SendError::kOk;
    }

    /// Batch-send multiple messages (sends each directly).
    SendError send_n(const std::span<const uint8_t>* payloads, size_t count,
                     uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (!core_.running.load(std::memory_order_acquire)) return SendError::kNotConnected;
        if (count > 0 && !payloads) [[unlikely]] return SendError::kNullData;

        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxPayload) return SendError::kMessageTooLarge;
            if (opcode == ws::opcode::kText && !core_.config.skip_utf8_validation &&
                !ws::is_valid_utf8(payloads[i].data(), payloads[i].size())) {
                return SendError::kInvalidUtf8;
            }
        }

        for (size_t i = 0; i < count; ++i) {
            auto err = send_direct_(payloads[i].data(), payloads[i].size(), opcode);
            if (err != SendError::kOk) {
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "send_n: send_direct_ failed at index {}/{}: {}",
                    i, count, static_cast<int>(err));
                return err;
            }
        }
        return SendError::kOk;
    }

    /// Batch-send with timeout (timeout ignored -- sends directly).
    template <typename Rep, typename Period>
    SendError send_n_for(const std::span<const uint8_t>* payloads, size_t count,
                         std::chrono::duration<Rep, Period> timeout,
                         uint8_t opcode = ws::opcode::kBinary) noexcept {
        (void)timeout;
        return send_n(payloads, count, opcode);
    }

    // -----------------------------------------------------------------------
    // Receive API (application thread -- delegates to RxWorker)
    // -----------------------------------------------------------------------

    /// Try to receive a message (non-blocking).
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool recv(F&& callback) {
        return rx_.recv(std::forward<F>(callback));
    }

    /// Try to receive a message with opcode (non-blocking).
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool recv(F&& callback) {
        return rx_.recv(std::forward<F>(callback));
    }

    /// Try to receive a message with opcode and arrival TSC (non-blocking).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t, uint64_t>
    [[nodiscard]] bool recv(F&& callback) {
        return rx_.recv(std::forward<F>(callback));
    }

    /// Try to receive a message as a copied byte vector (non-blocking).
    [[nodiscard]] std::optional<std::vector<uint8_t>> try_recv() {
        return rx_.try_recv();
    }

    /// Try to receive a message with opcode info (non-blocking).
    [[nodiscard]] std::optional<ReceivedMessage> try_recv_msg() {
        return rx_.try_recv_msg();
    }

    // -----------------------------------------------------------------------
    // Peek API (application thread)
    // -----------------------------------------------------------------------

    /// Peek at the next message without consuming (non-blocking).
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool recv_peek(F&& callback) {
        return rx_.recv_peek(std::forward<F>(callback));
    }

    /// Peek at the next message with opcode without consuming.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool recv_peek(F&& callback) {
        return rx_.recv_peek(std::forward<F>(callback));
    }

    /// Peek at the next message as a copied ReceivedMessage.
    [[nodiscard]] std::optional<ReceivedMessage> peek_recv_msg() {
        return rx_.peek_recv_msg();
    }

    // -----------------------------------------------------------------------
    // Batch receive
    // -----------------------------------------------------------------------

    /// Batch-receive up to max_count messages (non-blocking).
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t> && !RxWorkerT::kRxEvicting)
    [[nodiscard]] size_t recv_n(F&& callback, size_t max_count) {
        return rx_.recv_n(std::forward<F>(callback), max_count);
    }

    /// Batch-receive up to max_count messages with opcode (non-blocking).
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t, uint8_t> && !RxWorkerT::kRxEvicting)
    [[nodiscard]] size_t recv_n(F&& callback, size_t max_count) {
        return rx_.recv_n(std::forward<F>(callback), max_count);
    }

    /// Drain all available messages (non-blocking).
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t> && !RxWorkerT::kRxEvicting)
    [[nodiscard]] size_t drain_recv(F&& callback) {
        return rx_.drain_recv(std::forward<F>(callback));
    }

    // -----------------------------------------------------------------------
    // Blocking receive
    // -----------------------------------------------------------------------

    /// Blocking receive with timeout.
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

    /// Blocking receive returning a ReceivedMessage with timeout.
    [[nodiscard]] std::optional<ReceivedMessage> wait_recv_msg(
            std::chrono::milliseconds timeout) {
        return rx_.wait_recv_msg(timeout);
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Initiate a graceful WebSocket close handshake (RFC 6455 S7.1.1).
    bool close_gracefully(
            uint16_t status_code = ws::close_code::kNormal,
            std::string_view reason = "client shutdown",
            std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) noexcept {
        if (!core_.running.load(std::memory_order_acquire)) return false;

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

        // Wait for the server Close response (RX thread sets closing=true)
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

        stop();
        return true;
    }

    /// Start the RX worker thread. Only needed when TransportConfig::deferred_start
    /// is true. Must be called exactly once after create() returns.
    void start_threads() {
        rx_.start();
    }

    /// Stop the transport gracefully. Sends WebSocket Close frame.
    void stop() noexcept {
        bool was_running = core_.running.exchange(false, std::memory_order_acq_rel);

        auto log = detail::transport_logger();
        SPDLOG_LOGGER_INFO(log, "Stopping DirectTxTransport");

        // Join RX thread FIRST -- ensures no concurrent access to
        // crypto/tcp from RX thread when we send the Close frame.
        rx_.stop();

        // Send WebSocket Close frame after thread has exited (no race)
        if constexpr (kIsWebSocket) {
            if (was_running && core_.tcp && core_.tcp->is_established() &&
                (core_.config.use_tls ? core_.crypto != nullptr : true)) {
                uint16_t close_code = ws::close_code::kNormal;
                std::string_view close_reason = "client shutdown";
                if (core_.close_requested.load(std::memory_order_acquire)) {
                    close_code = core_.pending_close_code;
                    if (!core_.pending_close_reason.empty())
                        close_reason = core_.pending_close_reason;
                }
                uint8_t close_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
                size_t close_len = ws::build_close_frame(
                    close_buf, close_code, close_reason);

                if (core_.config.use_tls) {
                    uint8_t tls_buf[TlsRecordCrypto::encrypted_size(
                        ws::kMaxFrameHeaderLen + 125)]{};
                    uint16_t tls_len = core_.crypto->encrypt(
                        close_buf, static_cast<uint16_t>(close_len), tls_buf);
                    if (tls_len > 0) {
                        (void)core_.tcp->send(tls_buf, tls_len);
                    }
                } else {
                    (void)core_.tcp->send(close_buf, close_len);
                }
            }
        }

        // Close TCP connection
        if (core_.tcp && core_.tcp->is_established()) {
            (void)core_.tcp->close();
        }

        core_.notify_state(TransportEvent::kStopped);
        SPDLOG_LOGGER_INFO(log, "DirectTxTransport stopped");
    }

    [[nodiscard]] bool is_running() const noexcept {
        return core_.running.load(std::memory_order_acquire);
    }

    [[nodiscard]] const TransportConfig& config() const noexcept {
        return core_.config;
    }

    [[nodiscard]] TransportState state() const noexcept {
        if (!core_.running.load(std::memory_order_acquire))
            return TransportState::kStopped;
        if (core_.reconnecting.load(std::memory_order_acquire))
            return TransportState::kReconnecting;
        return TransportState::kConnected;
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return state() == TransportState::kConnected;
    }

    /// Force an immediate reconnection attempt (via RX thread).
    [[nodiscard]] bool reconnect_now() noexcept {
        if (!core_.running.load(std::memory_order_acquire)) return false;
        if (core_.config.max_reconnect_attempts <= 0) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "reconnect_now() called but auto-reconnect is disabled");
            return false;
        }
        SPDLOG_LOGGER_INFO(detail::transport_logger(),
            "reconnect_now() signaled by application");
        core_.force_reconnect.store(true, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // Queue occupancy (RX only -- no TX queue)
    // -----------------------------------------------------------------------

    /// Approximate number of messages available in the RX queue.
    [[nodiscard]] size_t rx_queue_size() const noexcept {
        return rx_.queue_size();
    }

    /// RX queue occupancy as a fraction [0.0, 1.0].
    [[nodiscard]] double rx_queue_fill_ratio() const noexcept {
        return rx_.queue_fill_ratio();
    }

    /// Peak RX queue occupancy since creation or last reset_stats().
    [[nodiscard]] size_t rx_queue_hwm() const noexcept {
        return rx_.queue_hwm();
    }

    /// Reset all statistics counters to zero.
    void reset_stats() noexcept {
        tx_stats_.reset();
        rx_.reset_stats();
        pong_timeouts_.store(0, std::memory_order_relaxed);
        reconnect_count_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::string_view tls_version() const noexcept {
        return core_.tls_version;
    }

    [[nodiscard]] std::string_view cipher_name() const noexcept {
        return core_.cipher_name;
    }

    [[nodiscard]] std::string_view ws_subprotocol() const noexcept {
        return core_.ws_subprotocol;
    }

    [[nodiscard]] std::string_view remote_ip() const noexcept {
        return core_.remote_ip;
    }

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

    [[nodiscard]] RttStats rtt_stats() const noexcept {
        return histogram_to_stats_(rtt_histogram_);
    }

    /// RX pipeline latency stats (arrival -> deliver).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats rx_latency_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_latency_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats_(rx_.latency_histogram());
    }

    /// RX decrypt stats (arrival -> decrypt done).
    [[nodiscard]] RttStats rx_decrypt_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_decrypt_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats_(rx_.decrypt_histogram());
    }

    /// RX decode stats (decrypt done -> frame decoded).
    [[nodiscard]] RttStats rx_decode_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_decode_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats_(rx_.decode_histogram());
    }

    /// Snapshot the RX pipeline latency histogram for windowed measurement.
    [[nodiscard]] eph::utils::HdrHistogram rx_latency_histogram_snapshot() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_latency_histogram_snapshot() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return rx_.latency_histogram();
    }

    /// Aggregated transport statistics snapshot.
    /// Note: tx_queue_hwm is always 0 (no TX queue in DirectTxTransport).
    [[nodiscard]] TransportStats stats() const noexcept {
        auto rx_s = rx_.stats();
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - core_.created_at).count();
        return TransportStats{
            .tx_packets        = tx_stats_.packets.load(std::memory_order_relaxed),
            .tx_bytes          = tx_stats_.bytes.load(std::memory_order_relaxed),
            .tx_text_packets   = tx_stats_.text_packets.load(std::memory_order_relaxed),
            .tx_text_bytes     = tx_stats_.text_bytes.load(std::memory_order_relaxed),
            .tx_dropped        = tx_stats_.dropped.load(std::memory_order_relaxed),
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
            .encrypt_errors    = tx_stats_.crypto_errors.load(std::memory_order_relaxed),
            .decrypt_errors    = rx_s.decrypt_errors,
            .queue_full_count  = 0,
            .ws_pings_received = rx_s.ws_pings_received,
            .ws_pongs_sent     = rx_s.ws_pongs_sent,
            .pong_timeouts     = pong_timeouts_.load(std::memory_order_relaxed),
            .reconnect_count   = reconnect_count_.load(std::memory_order_relaxed),
            .tx_queue_hwm      = 0,  // No TX queue in DirectTxTransport
            .rx_queue_hwm      = rx_s.rx_queue_hwm,
            .uptime_ns         = static_cast<uint64_t>(uptime > 0 ? uptime : 0),
            .handshake_ns      = core_.last_handshake_ns,
            .tcp_connect_ns    = core_.last_tcp_connect_ns,
            .tls_handshake_ns  = core_.last_tls_handshake_ns,
            .ws_upgrade_ns     = core_.last_ws_upgrade_ns,
            .remote_ip         = core_.remote_ip,
            .rtt               = rtt_stats(),
            .tx_latency        = {},  // No TX queue latency (direct send)
            .tx_queue_wait     = {},  // No TX queue wait (direct send)
            .tx_encode         = {},  // No TX encode histogram (direct send)
            .rx_latency        = rx_s.rx_latency,
            .rx_decrypt        = rx_s.rx_decrypt,
            .rx_decode         = rx_s.rx_decode,
            .tls_write_seq     = core_.crypto ? core_.crypto->write_seq() : 0,
            .tls_read_seq      = core_.crypto ? core_.crypto->read_seq() : 0,
            .tls_seq_limit     = core_.config.use_tls ? tls_record::kMaxSequenceNumber : 0,
        };
    }

private:
    /// Private constructor -- use create() factory.
    explicit DirectTxTransport(const TransportConfig& config)
        : core_{}
        , rx_(core_, reconnect_count_, make_rx_callbacks_())
        , reconnect_(config)
    {
        core_.config = config;
    }

    // -----------------------------------------------------------------------
    // RxWorker callback wiring
    // -----------------------------------------------------------------------

    /// Build the RxWorker::Callbacks struct.
    /// - do_reconnect: runs ReconnectPolicy loop
    /// - send_response: pong/close via send_direct_ (no TxWorker)
    typename RxWorkerT::Callbacks make_rx_callbacks_() {
        return typename RxWorkerT::Callbacks{
            .do_reconnect = [this]() -> bool {
                return do_reconnect_();
            },
            .send_response = [this](const void* data, size_t len, uint8_t opcode) -> SendError {
                return send_direct_(data, len, opcode);
            },
        };
    }

    // -----------------------------------------------------------------------
    // Reconnection (same as Transport, but no tx_.on_reconnected())
    // -----------------------------------------------------------------------

    /// Execute reconnection with exponential backoff via ReconnectPolicy.
    /// Returns true if reconnected successfully, false if exhausted.
    [[nodiscard]] bool do_reconnect_() noexcept {
        auto log = detail::transport_logger();

        if (core_.config.max_reconnect_attempts <= 0) {
            SPDLOG_LOGGER_WARN(log,
                "Auto-reconnect disabled (max_reconnect_attempts=0)");
            return false;
        }

        core_.reconnecting.store(true, std::memory_order_release);
        core_.notify_state(TransportEvent::kReconnecting, "connection lost");

        // Close existing connection
        if (core_.tcp && core_.tcp->is_established()) {
            (void)core_.tcp->close();
        }
        core_.crypto.reset();
        core_.tls.reset();

        reconnect_.reset();

        while (!reconnect_.exhausted()) {
            if (!core_.running.load(std::memory_order_acquire)) {
                SPDLOG_LOGGER_INFO(log, "Reconnect aborted: transport stopped");
                core_.reconnecting.store(false, std::memory_order_release);
                return false;
            }

            bool ok = reconnect_.attempt([this]()
                -> std::expected<void, ConnectionErrorInfo> {
                auto conn = core_.do_connect();
                if (!conn) return conn;
                return core_.template do_ws_upgrade<Framer>();
            });

            if (ok) {
                reconnect_count_.fetch_add(1, std::memory_order_relaxed);
                rx_.on_reconnected();
                // No tx_.on_reconnected() -- no TxWorker

                // Flush any pending TCP ACK from the new handshake
                if constexpr (requires { core_.tcp->flush_pending_ack(); }) {
                    core_.tcp->flush_pending_ack();
                }

                core_.reconnecting.store(false, std::memory_order_release);
                core_.notify_state(TransportEvent::kConnected, "reconnected");

                SPDLOG_LOGGER_INFO(log,
                    "Reconnected (total: {})", reconnect_count_.load(std::memory_order_relaxed));
                return true;
            }
        }

        SPDLOG_LOGGER_ERROR(log,
            "Reconnect exhausted after {} attempts", reconnect_.attempts());
        core_.reconnecting.store(false, std::memory_order_release);
        core_.notify_state(TransportEvent::kDisconnected, "reconnect exhausted");
        return false;
    }

    // -----------------------------------------------------------------------
    // Direct send: WS encode -> [TLS encrypt] -> TCP send.
    // Called from the application thread; core_.crypto->enc is exclusively
    // owned by the send caller (safe: RX thread only uses crypto->dec).
    //
    // Thread safety: do NOT call send_direct_() concurrently with stop().
    // Call stop() only after the app thread has finished all send_direct_()
    // calls (stop() may access crypto for the WS Close frame).
    // -----------------------------------------------------------------------

    SendError send_direct_(const void* data, size_t len, uint8_t opcode) noexcept {
        if (len > MaxPayload) return SendError::kMessageTooLarge;
        if (!core_.running.load(std::memory_order_acquire)) return SendError::kNotConnected;

        auto log = detail::transport_logger();

        // 1. Frame encode
        constexpr size_t kFrameOverhead = kIsWebSocket
            ? ws::kMaxFrameHeaderLen : Framer::max_overhead();
        constexpr size_t kBufSize = kFrameOverhead + MaxPayload + 1;
        uint8_t ws_buf[kBufSize];

        size_t ws_len;
        if constexpr (kIsWebSocket) {
            ws_len = ws::encode_frame(ws_buf, opcode,
                static_cast<const uint8_t*>(data), len);
        } else {
            Framer framer{};
            ws_len = framer.encode(ws_buf,
                static_cast<const uint8_t*>(data), len, opcode);
        }

        // 2. TLS encrypt (if enabled) + 3. TCP send
        if (core_.config.use_tls) {
            if (!core_.crypto) return SendError::kNotConnected;
            constexpr size_t kTlsBufSize =
                TlsEncryptor::encrypted_size(
                    static_cast<uint16_t>(kFrameOverhead + MaxPayload));
            uint8_t tls_buf[kTlsBufSize];

            uint16_t enc_len = core_.crypto->enc.encrypt(
                ws_buf, static_cast<uint16_t>(ws_len), tls_buf);
            if (enc_len == 0) {
                SPDLOG_LOGGER_ERROR(log, "send_direct_: encrypt failed");
                return SendError::kEncryptFailed;
            }

            auto result = core_.tcp->send(tls_buf, enc_len);
            if (!result) {
                SPDLOG_LOGGER_WARN(log, "send_direct_: TCP send failed: {}",
                    result.error());
                return SendError::kTcpSendFailed;
            }
        } else {
            auto result = core_.tcp->send(ws_buf, ws_len);
            if (!result) {
                SPDLOG_LOGGER_WARN(log, "send_direct_: TCP send failed: {}",
                    result.error());
                return SendError::kTcpSendFailed;
            }
        }

        // Stats
        tx_stats_.packets.fetch_add(1, std::memory_order_relaxed);
        tx_stats_.bytes.fetch_add(len, std::memory_order_relaxed);
        if (opcode == ws::opcode::kText) {
            tx_stats_.text_packets.fetch_add(1, std::memory_order_relaxed);
            tx_stats_.text_bytes.fetch_add(len, std::memory_order_relaxed);
        }

        return SendError::kOk;
    }

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

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

    // -----------------------------------------------------------------------
    // Member data -- composed components
    // -----------------------------------------------------------------------

    TransportCore<TcpImpl> core_;
    RxWorkerT rx_;
    ReconnectPolicy reconnect_;

    // TX stats (owned directly, no TxWorker)
    ThreadStats tx_stats_{};
    std::atomic<uint64_t> pong_timeouts_{0};
    std::atomic<uint64_t> reconnect_count_{0};

    // RTT measurement (ping/pong round-trip)
    eph::utils::HdrHistogram rtt_histogram_{
        100, 10'000'000'000ULL, 3
    };
};

} // namespace eph::net
