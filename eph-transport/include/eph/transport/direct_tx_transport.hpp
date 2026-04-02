#pragma once

/// @file direct_tx_transport.hpp
/// DirectTxTransport — the kDirectTx variant of the Transport class split.
///
/// Has an RX thread + RX queue for receiving, but NO TX thread or TX queue.
/// All sends go directly from the application thread (send_direct path).
///
/// Thread model:
///   - Application thread: calls send()/recv(), send is blocking (direct)
///   - RX thread: busy-poll TCP rx, decrypt, parse WS frames, push to recv queue
///   - NO TX thread or TX queue
///
/// Use this when the application thread can tolerate the latency of
/// WS encode + TLS encrypt + TCP send inline, and wants to avoid the
/// extra thread and SPSC queue overhead of the fully threaded Transport.

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
#include <type_traits>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/utils/alignment.hpp"
#include "eph/containers/bounded_queue.hpp"
#include "eph/containers/evicting_queue.hpp"
#include "eph/utils/cpu.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/core/framer_concept.hpp"
#include "eph/transport/http.hpp"
#include "eph/core/tcp_concept.hpp"
#include "eph/transport/tls_decryptor.hpp"
#include "eph/transport/tls_encryptor.hpp"
#include "eph/transport/tls_record.hpp"
#include "eph/transport/tls_session.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/transport/websocket.hpp"
#include "eph/transport/ws_framer.hpp"

#include "eph/transport/detail/message_types.hpp"

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

    // -- Capability constants (DirectTx: RX thread + RX queue, no TX thread/queue) --
    static constexpr bool kHasTxThread = false;
    static constexpr bool kHasTxQueue  = false;
    static constexpr bool kHasRxThread = true;
    static constexpr bool kHasRxQueue  = true;

    /// True when using WebSocket framing.
    static constexpr bool kIsWebSocket = std::is_same_v<Framer, WsFramer>;

    /// True when the RX queue uses evicting (latest-value) semantics.
    static constexpr bool kRxEvicting =
        std::same_as<RxQueueTmpl<int, 2>,
                     eph::containers::EvictingQueue<int, 2>>;

    /// Controls whether only the last WS data frame per batch is delivered.
    static constexpr bool kLastOnlyDeliver = LastOnlyDeliver;

    // Mode-derived constants for compatibility with detail includes
    // Note: kIsThreaded/kIsDirectTx/kIsDirect removed — use separate classes instead

    using RxMsg = detail::RxMessage<MaxPayload>;
    using RxQueue = RxQueueTmpl<RxMsg, QueueDepth>;

public:
    /// Factory callable: creates a new, already-connected TcpImpl instance.
    using TcpFactory = std::function<
        std::expected<std::unique_ptr<TcpImpl>, std::string>()>;

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

        auto t = std::unique_ptr<DirectTxTransport>(new DirectTxTransport());
        t->config_      = config;
        t->tcp_factory_ = std::move(tcp_factory);

        auto conn_result = t->do_connect();
        if (!conn_result) {
            SPDLOG_LOGGER_ERROR(log, "Initial connect failed: {}",
                                conn_result.error().message());
            return std::unexpected(conn_result.error());
        }

        // Pre-allocate WS fragmentation buffer
        if constexpr (kIsWebSocket) {
            t->ws_frag_buf_.reserve(MaxPayload);
        }

        t->created_at_ = std::chrono::steady_clock::now();
        t->running_.store(true, std::memory_order_release);
        t->notify_state(TransportEvent::kConnected, config.remote_host);

        // Flush any pending TCP ACK accumulated during handshake.
        if constexpr (requires { t->tcp_->flush_pending_ack(); }) {
            t->tcp_->flush_pending_ack();
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
    // Send API (application thread — direct send, no queue)
    // -----------------------------------------------------------------------

    /// Send data as a WebSocket frame (blocking — sends directly from app thread).
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
        if (opcode == ws::opcode::kText && !config_.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return send_direct(data, len, opcode);
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
        if (!config_.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        return send_direct(data, len, ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket text frame.
    [[nodiscard]] SendError send_text(std::string_view sv) noexcept {
        if (!config_.skip_utf8_validation && !ws::is_valid_utf8(sv)) {
            return SendError::kInvalidUtf8;
        }
        return send_direct(sv.data(), sv.size(), ws::opcode::kText);
    }

    /// Send a text frame WITHOUT UTF-8 validation (unchecked).
    [[nodiscard]] SendError send_text_unchecked(const void* data, size_t len) noexcept {
        return send_direct(data, len, ws::opcode::kText);
    }

    /// Send a string_view as an unchecked text frame.
    [[nodiscard]] SendError send_text_unchecked(std::string_view sv) noexcept {
        return send_direct(sv.data(), sv.size(), ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket binary frame.
    [[nodiscard]] SendError send_binary(std::string_view sv) noexcept {
        return send(sv.data(), sv.size(), ws::opcode::kBinary);
    }

    /// Send data with timeout — timeout is ignored (no queue), sends directly.
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_for(const void* data, size_t len,
                       std::chrono::duration<Rep, Period> timeout,
                       uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (opcode == ws::opcode::kText && !config_.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        (void)timeout;
        return send_direct(data, len, opcode);
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
        if (!config_.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        (void)timeout;
        return send_direct(data, len, ws::opcode::kText);
    }

    /// Send a string_view as a WebSocket text frame with timeout.
    template <typename Rep, typename Period>
    [[nodiscard]] SendError send_text_for(std::string_view sv,
                            std::chrono::duration<Rep, Period> timeout) noexcept {
        if (!config_.skip_utf8_validation && !ws::is_valid_utf8(sv)) {
            return SendError::kInvalidUtf8;
        }
        (void)timeout;
        return send_direct(sv.data(), sv.size(), ws::opcode::kText);
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
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;
        if (!ws::is_valid_close_code(status_code)) return SendError::kInvalidCloseCode;
        if (!reason.empty() && !ws::is_valid_utf8(reason)) return SendError::kInvalidUtf8;

        size_t reason_len = std::min(reason.size(), size_t{123});
        if (reason_len < reason.size()) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Close reason truncated from {} to 123 bytes (RFC 6455 §5.5 limit)",
                reason.size());
        }
        uint16_t payload_len = static_cast<uint16_t>(2 + reason_len);
        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

        // Direct send: encode close frame and send immediately
        uint8_t close_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
        size_t close_len = ws::build_close_frame(close_buf, status_code, reason);
        if (config_.use_tls && crypto_) {
            uint8_t tls_buf[TlsEncryptor::encrypted_size(
                ws::kMaxFrameHeaderLen + 125)];
            uint16_t enc_len = crypto_->enc.encrypt(
                close_buf, static_cast<uint16_t>(close_len), tls_buf);
            if (enc_len > 0) {
                tcp_->send(tls_buf, enc_len);
            } else {
                SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                    "send_close: encrypt failed for status_code={}", status_code);
                return SendError::kEncryptFailed;
            }
        } else {
            tcp_->send(close_buf, close_len);
        }
        return SendError::kOk;
    }

    /// Send a WebSocket Ping frame.
    SendError send_ping(const void* payload = nullptr,
                        size_t payload_len = 0) noexcept {
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;

        size_t original_len = payload_len;
        payload_len = std::min(payload_len, size_t{125});
        if (original_len > 125) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Ping payload truncated from {} to 125 bytes (RFC 6455 §5.5 limit)",
                original_len);
        }
        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

        // Direct send: encode ping frame and send immediately
        uint8_t ping_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
        size_t ping_len = ws::build_ping_frame(ping_buf, payload, payload_len);
        if (config_.use_tls && crypto_) {
            uint8_t tls_buf[TlsEncryptor::encrypted_size(
                ws::kMaxFrameHeaderLen + 125)];
            uint16_t enc_len = crypto_->enc.encrypt(
                ping_buf, static_cast<uint16_t>(ping_len), tls_buf);
            if (enc_len > 0) {
                tcp_->send(tls_buf, enc_len);
            } else {
                SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                    "send_ping: encrypt failed, payload_len={}", payload_len);
                return SendError::kEncryptFailed;
            }
        } else {
            tcp_->send(ping_buf, ping_len);
        }
        return SendError::kOk;
    }

    /// Batch-send multiple messages (sends each directly).
    SendError send_n(const std::span<const uint8_t>* payloads, size_t count,
                     uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;
        if (count > 0 && !payloads) [[unlikely]] return SendError::kNullData;

        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxPayload) return SendError::kMessageTooLarge;
            if (opcode == ws::opcode::kText && !config_.skip_utf8_validation &&
                !ws::is_valid_utf8(payloads[i].data(), payloads[i].size())) {
                return SendError::kInvalidUtf8;
            }
        }

        for (size_t i = 0; i < count; ++i) {
            auto err = send_direct(payloads[i].data(), payloads[i].size(), opcode);
            if (err != SendError::kOk) {
                SPDLOG_LOGGER_WARN(detail::transport_logger(),
                    "send_n: send_direct failed at index {}/{}: {}",
                    i, count, static_cast<int>(err));
                return err;
            }
        }
        return SendError::kOk;
    }

    /// Batch-send with timeout (timeout ignored — sends directly).
    template <typename Rep, typename Period>
    SendError send_n_for(const std::span<const uint8_t>* payloads, size_t count,
                         std::chrono::duration<Rep, Period> timeout,
                         uint8_t opcode = ws::opcode::kBinary) noexcept {
        (void)timeout;
        return send_n(payloads, count, opcode);
    }

    // -----------------------------------------------------------------------
    // Receive API (application thread)
    //
    // Same as Transport: pull from RX queue, or push via on_message callback.
    // -----------------------------------------------------------------------

    /// Try to receive a message (non-blocking).
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool recv(F&& callback) {
        bool consumed = rx_consume([&](const RxMsg& msg) {
            SPDLOG_LOGGER_TRACE(detail::transport_logger(),
                "RX dequeue: len={}, opcode={}", msg.len, msg.opcode);
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len));
        });
        return consumed;
    }

    /// Try to receive a message with opcode (non-blocking).
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool recv(F&& callback) {
        bool consumed = rx_consume([&](const RxMsg& msg) {
            SPDLOG_LOGGER_TRACE(detail::transport_logger(),
                "RX dequeue: len={}, opcode={}", msg.len, msg.opcode);
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len), msg.opcode);
        });
        return consumed;
    }

    /// Try to receive a message with opcode and arrival timestamp (non-blocking).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t, uint64_t>
    [[nodiscard]] bool recv(F&& callback) {
        static_assert(kEnableTimestamps,
            "recv() with TSC callback requires -DEPH_ENABLE_TIMESTAMPS=1");
        bool consumed = rx_consume([&](const RxMsg& msg) {
            SPDLOG_LOGGER_TRACE(detail::transport_logger(),
                "RX dequeue: len={}, opcode={}, tsc={}", msg.len, msg.opcode, msg.tsc);
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len), msg.opcode, msg.tsc);
        });
        return consumed;
    }

    /// Try to receive a message as a copied byte vector (non-blocking).
    [[nodiscard]] std::optional<std::vector<uint8_t>> try_recv() {
        std::optional<std::vector<uint8_t>> result;
        (void)rx_consume([&](const RxMsg& msg) {
            result.emplace(msg.data, msg.data + msg.len);
        });
        return result;
    }

    /// Received message with opcode metadata.
    struct ReceivedMessage {
        std::vector<uint8_t> data;
        uint8_t opcode = ws::opcode::kBinary;

        [[nodiscard]] bool is_text() const noexcept {
            return opcode == ws::opcode::kText;
        }
        [[nodiscard]] bool is_binary() const noexcept {
            return opcode == ws::opcode::kBinary;
        }
        [[nodiscard]] bool is_close() const noexcept {
            return opcode == ws::opcode::kClose;
        }
        [[nodiscard]] std::string_view text() const noexcept {
            return {reinterpret_cast<const char*>(data.data()), data.size()};
        }
        [[nodiscard]] uint16_t close_code() const noexcept {
            if (opcode != ws::opcode::kClose || data.size() < 2) return 0;
            return static_cast<uint16_t>((data[0] << 8) | data[1]);
        }
        [[nodiscard]] std::string_view close_reason() const noexcept {
            if (opcode != ws::opcode::kClose || data.size() <= 2) return {};
            return {reinterpret_cast<const char*>(data.data() + 2),
                    data.size() - 2};
        }
    };

    /// Try to receive a message with opcode info (non-blocking).
    [[nodiscard]] std::optional<ReceivedMessage> try_recv_msg() {
        std::optional<ReceivedMessage> result;
        (void)rx_consume([&](const RxMsg& msg) {
            result.emplace(ReceivedMessage{
                .data = std::vector<uint8_t>(msg.data, msg.data + msg.len),
                .opcode = msg.opcode,
            });
        });
        return result;
    }

    // -----------------------------------------------------------------------
    // Peek API (application thread)
    // -----------------------------------------------------------------------

    /// Peek at the next message without consuming (non-blocking).
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool recv_peek(F&& callback) {
        return rx_peek([&](const RxMsg& msg) {
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len));
        });
    }

    /// Peek at the next message with opcode without consuming.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool recv_peek(F&& callback) {
        return rx_peek([&](const RxMsg& msg) {
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len),
                        msg.opcode);
        });
    }

    /// Peek at the next message as a copied ReceivedMessage.
    [[nodiscard]] std::optional<ReceivedMessage> peek_recv_msg() {
        std::optional<ReceivedMessage> result;
        (void)rx_peek([&](const RxMsg& msg) {
            result.emplace(ReceivedMessage{
                .data = std::vector<uint8_t>(msg.data, msg.data + msg.len),
                .opcode = msg.opcode,
            });
        });
        return result;
    }

    /// Batch-receive up to max_count messages (non-blocking).
    /// @note Not available when RxQueue is EvictingQueue.
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t> && !kRxEvicting)
    [[nodiscard]] size_t recv_n(F&& callback, size_t max_count) {
        return rx_queue_.try_consume_n(max_count,
            [&](const RxMsg& msg, [[maybe_unused]] size_t idx) {
                std::invoke(std::forward<F>(callback), msg.data, msg.len);
            });
    }

    /// Batch-receive up to max_count messages with opcode (non-blocking).
    /// @note Not available when RxQueue is EvictingQueue.
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t, uint8_t> && !kRxEvicting)
    [[nodiscard]] size_t recv_n(F&& callback, size_t max_count) {
        return rx_queue_.try_consume_n(max_count,
            [&](const RxMsg& msg, [[maybe_unused]] size_t idx) {
                std::invoke(std::forward<F>(callback),
                            msg.data, msg.len, msg.opcode);
            });
    }

    /// Drain all available messages (non-blocking).
    /// @note Not available when RxQueue is EvictingQueue.
    template <typename F>
        requires (std::invocable<F, const uint8_t*, size_t> && !kRxEvicting)
    [[nodiscard]] size_t drain_recv(F&& callback) {
        return recv_n(std::forward<F>(callback), QueueDepth);
    }

    /// Blocking receive with timeout.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t>
    [[nodiscard]] bool wait_recv(F&& callback, std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (running_.load(std::memory_order_acquire)) {
            bool got = rx_consume([&](const RxMsg& msg) {
                std::invoke(std::forward<F>(callback),
                            static_cast<const uint8_t*>(msg.data),
                            static_cast<size_t>(msg.len));
            });
            if (got) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return false;
    }

    /// Blocking receive with opcode and timeout.
    template <typename F>
        requires std::invocable<F, const uint8_t*, size_t, uint8_t>
    [[nodiscard]] bool wait_recv(F&& callback, std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (running_.load(std::memory_order_acquire)) {
            bool got = rx_consume([&](const RxMsg& msg) {
                std::invoke(std::forward<F>(callback),
                            static_cast<const uint8_t*>(msg.data),
                            static_cast<size_t>(msg.len), msg.opcode);
            });
            if (got) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return false;
    }

    /// Blocking receive returning a ReceivedMessage with timeout.
    [[nodiscard]] std::optional<ReceivedMessage> wait_recv_msg(
            std::chrono::milliseconds timeout) {
        std::optional<ReceivedMessage> result;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (running_.load(std::memory_order_acquire)) {
            bool got = rx_consume([&](const RxMsg& msg) {
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
    bool close_gracefully(
            uint16_t status_code = ws::close_code::kNormal,
            std::string_view reason = "client shutdown",
            std::chrono::milliseconds timeout = std::chrono::milliseconds{3000}) noexcept {
        if (!running_.load(std::memory_order_acquire)) return false;

        pending_close_code_ = status_code;
        pending_close_reason_ = std::string(reason);
        close_requested_.store(true, std::memory_order_release);

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
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }

        stop();
        return true;
    }

    /// Start the RX worker thread. Only needed when TransportConfig::deferred_start
    /// is true. Must be called exactly once after create() returns.
    void start_threads() {
        auto* tp = this;
        rx_thread_ = std::thread([tp] { tp->rx_loop(); });
    }

    /// Stop the transport gracefully. Sends WebSocket Close frame.
    void stop() noexcept {
        bool was_running = running_.exchange(false, std::memory_order_acq_rel);

        auto log = detail::transport_logger();
        SPDLOG_LOGGER_INFO(log, "Stopping DirectTxTransport");

        // Join RX thread FIRST — ensures no concurrent access to
        // crypto_/tcp_ from RX thread when we send the Close frame.
        if (rx_thread_.joinable()) rx_thread_.join();

        // Send WebSocket Close frame after thread has exited (no race)
        if constexpr (kIsWebSocket) {
            if (was_running && tcp_ && tcp_->is_established() &&
                (config_.use_tls ? crypto_ != nullptr : true)) {
                uint16_t close_code = ws::close_code::kNormal;
                std::string_view close_reason = "client shutdown";
                if (close_requested_.load(std::memory_order_acquire)) {
                    close_code = pending_close_code_;
                    if (!pending_close_reason_.empty())
                        close_reason = pending_close_reason_;
                }
                uint8_t close_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
                size_t close_len = ws::build_close_frame(
                    close_buf, close_code, close_reason);

                if (config_.use_tls) {
                    uint8_t tls_buf[TlsRecordCrypto::encrypted_size(
                        ws::kMaxFrameHeaderLen + 125)]{};
                    uint16_t tls_len = crypto_->encrypt(
                        close_buf, static_cast<uint16_t>(close_len), tls_buf);
                    if (tls_len > 0) {
                        (void)tcp_->send(tls_buf, tls_len);
                    }
                } else {
                    (void)tcp_->send(close_buf, close_len);
                }
            }
        }

        // Close TCP connection
        if (tcp_ && tcp_->is_established()) {
            (void)tcp_->close();
        }

        notify_state(TransportEvent::kStopped);
        SPDLOG_LOGGER_INFO(log, "DirectTxTransport stopped");
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const TransportConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] TransportState state() const noexcept {
        if (!running_.load(std::memory_order_acquire))
            return TransportState::kStopped;
        if (reconnecting_.load(std::memory_order_acquire))
            return TransportState::kReconnecting;
        return TransportState::kConnected;
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return state() == TransportState::kConnected;
    }

    /// Force an immediate reconnection attempt (via RX thread).
    [[nodiscard]] bool reconnect_now() noexcept {
        if (!running_.load(std::memory_order_acquire)) return false;
        if (config_.max_reconnect_attempts <= 0) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "reconnect_now() called but auto-reconnect is disabled");
            return false;
        }
        SPDLOG_LOGGER_INFO(detail::transport_logger(),
            "reconnect_now() signaled by application");
        force_reconnect_.store(true, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // Queue occupancy (RX only — no TX queue)
    // -----------------------------------------------------------------------

    /// Approximate number of messages available in the RX queue.
    [[nodiscard]] size_t rx_queue_size() const noexcept {
        return rx_size();
    }

    /// RX queue occupancy as a fraction [0.0, 1.0].
    [[nodiscard]] double rx_queue_fill_ratio() const noexcept {
        return static_cast<double>(rx_size()) /
               static_cast<double>(QueueDepth);
    }

    /// Peak RX queue occupancy since creation or last reset_stats().
    [[nodiscard]] size_t rx_queue_hwm() const noexcept {
        return rx_hwm_.load(std::memory_order_relaxed);
    }

    /// Reset all statistics counters to zero.
    void reset_stats() noexcept {
        tx_stats_.reset();
        rx_stats_.reset();
        queue_full_count_.store(0, std::memory_order_relaxed);
        ws_pings_received_.store(0, std::memory_order_relaxed);
        ws_pongs_sent_.store(0, std::memory_order_relaxed);
        pong_timeouts_.store(0, std::memory_order_relaxed);
        reconnect_count_.store(0, std::memory_order_relaxed);
        rx_hwm_.store(0, std::memory_order_relaxed);
        rtt_histogram_.reset();
    }

    [[nodiscard]] std::string_view tls_version() const noexcept {
        return tls_version_;
    }

    [[nodiscard]] std::string_view cipher_name() const noexcept {
        return cipher_name_;
    }

    [[nodiscard]] std::string_view ws_subprotocol() const noexcept {
        return ws_subprotocol_;
    }

    [[nodiscard]] std::string_view remote_ip() const noexcept {
        return remote_ip_;
    }

    [[nodiscard]] ConnectionInfo connection_info() const {
        return ConnectionInfo{
            .tls_version    = std::string(tls_version_),
            .cipher_name    = std::string(cipher_name_),
            .ws_subprotocol = std::string(ws_subprotocol_),
            .remote_ip      = std::string(remote_ip_),
            .remote_port    = config_.remote_port,
            .use_tls        = config_.use_tls,
        };
    }

    [[nodiscard]] RttStats rtt_stats() const noexcept {
        return histogram_to_stats(rtt_histogram_);
    }

    /// RX pipeline latency stats (arrival -> deliver).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats rx_latency_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_latency_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(rx_latency_histogram_);
    }

    /// RX decrypt stats (arrival -> decrypt done).
    [[nodiscard]] RttStats rx_decrypt_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_decrypt_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(rx_decrypt_histogram_);
    }

    /// RX decode stats (decrypt done -> frame decoded).
    [[nodiscard]] RttStats rx_decode_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_decode_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(rx_decode_histogram_);
    }

    /// Snapshot the RX pipeline latency histogram for windowed measurement.
    [[nodiscard]] eph::utils::HdrHistogram rx_latency_histogram_snapshot() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_latency_histogram_snapshot() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return rx_latency_histogram_;
    }

    /// Aggregated transport statistics snapshot.
    /// Note: tx_queue_hwm is always 0 (no TX queue in DirectTxTransport).
    [[nodiscard]] TransportStats stats() const noexcept {
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - created_at_).count();
        return TransportStats{
            .tx_packets        = tx_stats_.packets.load(std::memory_order_relaxed),
            .tx_bytes          = tx_stats_.bytes.load(std::memory_order_relaxed),
            .tx_text_packets   = tx_stats_.text_packets.load(std::memory_order_relaxed),
            .tx_text_bytes     = tx_stats_.text_bytes.load(std::memory_order_relaxed),
            .tx_dropped        = tx_stats_.dropped.load(std::memory_order_relaxed),
            .rx_packets        = rx_stats_.packets.load(std::memory_order_relaxed),
            .rx_bytes          = rx_stats_.bytes.load(std::memory_order_relaxed),
            .rx_text_packets   = rx_stats_.text_packets.load(std::memory_order_relaxed),
            .rx_text_bytes     = rx_stats_.text_bytes.load(std::memory_order_relaxed),
            .rx_dropped        = rx_stats_.dropped.load(std::memory_order_relaxed),
            .tcp_rx_packets    = [this]() -> uint64_t {
                if constexpr (requires { tcp_->tcp_stats(); })
                    return tcp_ ? tcp_->tcp_stats().rx_packets : 0;
                else return 0;
            }(),
            .tcp_rx_bursts     = [this]() -> uint64_t {
                if constexpr (requires { tcp_->tcp_stats(); })
                    return tcp_ ? tcp_->tcp_stats().rx_bursts : 0;
                else return 0;
            }(),
            .encrypt_errors    = tx_stats_.crypto_errors.load(std::memory_order_relaxed),
            .decrypt_errors    = rx_stats_.crypto_errors.load(std::memory_order_relaxed),
            .queue_full_count  = queue_full_count_.load(std::memory_order_relaxed),
            .ws_pings_received = ws_pings_received_.load(std::memory_order_relaxed),
            .ws_pongs_sent     = ws_pongs_sent_.load(std::memory_order_relaxed),
            .pong_timeouts     = pong_timeouts_.load(std::memory_order_relaxed),
            .reconnect_count   = reconnect_count_.load(std::memory_order_relaxed),
            .tx_queue_hwm      = 0,  // No TX queue in DirectTxTransport
            .rx_queue_hwm      = rx_hwm_.load(std::memory_order_relaxed),
            .uptime_ns         = static_cast<uint64_t>(uptime > 0 ? uptime : 0),
            .handshake_ns      = last_handshake_ns_,
            .tcp_connect_ns    = last_tcp_connect_ns_,
            .tls_handshake_ns  = last_tls_handshake_ns_,
            .ws_upgrade_ns     = last_ws_upgrade_ns_,
            .remote_ip         = remote_ip_,
            .rtt               = rtt_stats(),
            .tx_latency        = {},  // No TX queue latency (direct send)
            .tx_queue_wait     = {},  // No TX queue wait (direct send)
            .tx_encode         = {},  // No TX encode histogram (direct send)
            .rx_latency        = histogram_to_stats(rx_latency_histogram_),
            .rx_decrypt        = histogram_to_stats(rx_decrypt_histogram_),
            .rx_decode         = histogram_to_stats(rx_decode_histogram_),
            .tls_write_seq     = crypto_ ? crypto_->write_seq() : 0,
            .tls_read_seq      = crypto_ ? crypto_->read_seq() : 0,
            .tls_seq_limit     = config_.use_tls ? tls_record::kMaxSequenceNumber : 0,
        };
    }

private:
    DirectTxTransport() = default;

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    static RttStats histogram_to_stats(const eph::utils::HdrHistogram& h) noexcept {
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

    static void update_hwm(std::atomic<size_t>& hwm, size_t current) noexcept {
        size_t prev = hwm.load(std::memory_order_relaxed);
        while (current > prev &&
               !hwm.compare_exchange_weak(prev, current,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    // -----------------------------------------------------------------------
    // RxQueue dispatch helpers — abstract BoundedQueue vs EvictingQueue
    // -----------------------------------------------------------------------

    bool rx_enqueue(const uint8_t* data, uint16_t len,
                    uint8_t opcode) noexcept {
        if constexpr (kRxEvicting) {
            rx_queue_.produce([&](RxMsg& slot) {
                std::memcpy(slot.data, data, len);
                slot.len = len;
                slot.opcode = opcode;
                if constexpr (kEnableTimestamps) {
                    slot.tsc = current_arrival_tsc_;
                }
            });
            return true;
        } else {
            return rx_queue_.try_produce([&](RxMsg& msg) {
                std::memcpy(msg.data, data, len);
                msg.len = len;
                msg.opcode = opcode;
                if constexpr (kEnableTimestamps) {
                    msg.tsc = current_arrival_tsc_;
                }
            });
        }
    }

    template <typename F>
    bool rx_consume(F&& visitor) {
        if constexpr (kRxEvicting) {
            return rx_queue_.try_consume_latest(std::forward<F>(visitor));
        } else {
            return rx_queue_.try_consume(std::forward<F>(visitor));
        }
    }

    template <typename F>
    bool rx_peek(F&& visitor) {
        if constexpr (kRxEvicting) {
            return rx_queue_.try_peek_latest(std::forward<F>(visitor));
        } else {
            return rx_queue_.try_peek(std::forward<F>(visitor));
        }
    }

    [[nodiscard]] size_t rx_size() const noexcept {
        if constexpr (kRxEvicting) {
            return rx_queue_.size_approx();
        } else {
            return rx_queue_.size();
        }
    }

    // -----------------------------------------------------------------------
    // Direct send: WS encode -> [TLS encrypt] -> TCP send.
    // Called from the application thread; crypto_->enc is exclusively
    // owned by the send caller (safe: RX thread only uses crypto_->dec).
    //
    // Thread safety: do NOT call send_direct() concurrently with stop().
    // Call stop() only after the app thread has finished all send_direct()
    // calls (stop() may access crypto_ for the WS Close frame).
    // -----------------------------------------------------------------------

    SendError send_direct(const void* data, size_t len, uint8_t opcode) noexcept {
        if (len > MaxPayload) return SendError::kMessageTooLarge;
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;

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
        if (config_.use_tls) {
            if (!crypto_) return SendError::kNotConnected;
            constexpr size_t kTlsBufSize =
                TlsEncryptor::encrypted_size(
                    static_cast<uint16_t>(kFrameOverhead + MaxPayload));
            uint8_t tls_buf[kTlsBufSize];

            uint16_t enc_len = crypto_->enc.encrypt(
                ws_buf, static_cast<uint16_t>(ws_len), tls_buf);
            if (enc_len == 0) {
                SPDLOG_LOGGER_ERROR(log, "send_direct: encrypt failed");
                return SendError::kEncryptFailed;
            }

            auto result = tcp_->send(tls_buf, enc_len);
            if (!result) {
                SPDLOG_LOGGER_WARN(log, "send_direct: TCP send failed: {}",
                    result.error());
                return SendError::kTcpSendFailed;
            }
        } else {
            auto result = tcp_->send(ws_buf, ws_len);
            if (!result) {
                SPDLOG_LOGGER_WARN(log, "send_direct: TCP send failed: {}",
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
    // Member data
    // -----------------------------------------------------------------------

    TransportConfig                        config_;
    TcpFactory                             tcp_factory_;
    uint64_t                               current_arrival_tsc_{0};
    uint64_t                               current_decrypt_done_tsc_{0};
    double                                 ns_per_cycle_{0.0};
    std::unique_ptr<TcpImpl>               tcp_;
    std::unique_ptr<TlsSession<TcpImpl>>   tls_;

    // Connection metadata
    std::string                            tls_version_{"none"};
    std::string                            cipher_name_{"none"};
    std::string                            ws_subprotocol_{};
    std::string                            remote_ip_{};
    std::unique_ptr<TlsRecordCrypto>       crypto_;

    // Uptime tracking
    std::chrono::steady_clock::time_point  created_at_{};

    // Stub types for detail file compatibility — referenced inside
    // if constexpr (kHasTxQueue) dead branches. Never used at runtime.
    using TxMsg = detail::TxMessage<MaxPayload>;
    struct TxQueueStub_ {
        void clear() noexcept {}
        template<typename F> bool try_produce(F&&) noexcept { return false; }
        size_t size() const noexcept { return 0; }
    };
    [[no_unique_address]] TxQueueStub_     tx_queue_{};
    std::atomic<size_t>                    tx_hwm_{0};
    uint64_t                               tx_hwm_counter_{0};

    RxQueue                                rx_queue_{};
    Framer                                 rx_framer_{};

    std::atomic<bool>                      running_{false};
    std::atomic<bool>                      reconnecting_{false};
    std::atomic<bool>                      closing_{false};
    std::atomic<bool>                      force_reconnect_{false};

    // NO tx_thread_ — DirectTxTransport has no TX thread
    std::thread                            rx_thread_{};

    // Per-thread stats
    ThreadStats                            tx_stats_{};
    ThreadStats                            rx_stats_{};

    // Queue high-watermark (RX only — no TX queue)
    std::atomic<size_t>                    rx_hwm_{0};
    uint64_t                               rx_hwm_counter_{0};  // RX thread only

    std::atomic<uint64_t>                  queue_full_count_{0};
    std::atomic<uint64_t>                  ws_pings_received_{0};
    std::atomic<uint64_t>                  ws_pongs_sent_{0};
    std::atomic<uint64_t>                  reconnect_count_{0};
    std::atomic<uint64_t>                  pong_timeouts_{0};
    uint64_t                               last_handshake_ns_{0};
    uint64_t                               last_tcp_connect_ns_{0};
    uint64_t                               last_tls_handshake_ns_{0};
    uint64_t                               last_ws_upgrade_ns_{0};

    // Pong timeout tracking
    std::atomic<int64_t>                   last_pong_ns_{0};

    // Per-thread variables
    bool                                   ping_awaiting_pong_{false};
    bool                                   seq_warning_logged_{false};
    bool                                   rx_seq_warning_logged_{false};

    // Close handshake state
    uint16_t                               pending_close_code_{ws::close_code::kNormal};
    std::string                            pending_close_reason_{};
    std::atomic<bool>                      close_requested_{false};

    // RTT measurement
    std::atomic<uint64_t>                  last_ping_tsc_{0};
    eph::utils::HdrHistogram               rtt_histogram_{
        100, 10'000'000'000ULL, 3
    };

    // RX-only latency histograms (no TX histograms — direct send)
    eph::utils::HdrHistogram               rx_latency_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               rx_decrypt_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               rx_decode_histogram_{
        10, 1'000'000'000ULL, 3
    };

    // Unused but needed by detail includes that reference them
    eph::utils::HdrHistogram               tx_latency_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               tx_queue_wait_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               tx_encode_histogram_{
        10, 1'000'000'000ULL, 3
    };

    // WebSocket fragmentation reassembly buffer (RX thread only).
    std::vector<uint8_t>                   ws_frag_buf_;
    uint8_t                                ws_frag_opcode_ = 0;

    // Private method implementations (split for readability)
    // NOTE: NO transport_tx.hpp — DirectTxTransport has no TX thread loop
#include "eph/transport/detail/transport_state.hpp"
#include "eph/transport/detail/transport_rx.hpp"
#include "eph/transport/detail/transport_frame.hpp"
};

} // namespace eph::net
