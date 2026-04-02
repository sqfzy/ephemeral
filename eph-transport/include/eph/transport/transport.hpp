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
// Compile-time timestamp control (shared with SocketTransport)
// ---------------------------------------------------------------------------

/// Compile-time switch for per-message TSC timestamps in Transport.
/// Pass -DEPH_ENABLE_TIMESTAMPS=1 via the build system to enable.
/// Controls both Transport (message TSC, internal histograms) and
/// SocketTransport (SO_TIMESTAMPING kernel timestamps).
#ifndef EPH_ENABLE_TIMESTAMPS
#define EPH_ENABLE_TIMESTAMPS 0
#endif

inline constexpr bool kEnableTimestamps = (EPH_ENABLE_TIMESTAMPS != 0);

/// Transport operating mode — controls threading and queue behavior.
/// Selected at compile time to eliminate dead code via if constexpr.
enum class TransportMode {
    kThreaded,  ///< TX thread + RX thread + SPSC queues (default)
    kDirectTx,  ///< App thread sends directly; RX thread + queue for receive
    kDirect,    ///< App thread does both TX and RX; no background threads
};

namespace detail {
/// Zero-size placeholder for conditionally-disabled members.
struct Empty {};
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
/// @tparam LastOnlyDeliver  When true, only the last WS data frame per
///   process_ws_data() call is delivered; intermediate frames are decoded
///   but skipped.  Useful for single-symbol streams where only the latest
///   value matters.  For multi-symbol combined streams, set to false so
///   every message reaches the application.
template <TcpTransport TcpImpl, MessageFramer Framer = WsFramer,
          TransportMode Mode = TransportMode::kThreaded,
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

    // -- Mode-derived compile-time constants --
    static constexpr bool kIsThreaded = (Mode == TransportMode::kThreaded);
    static constexpr bool kIsDirectTx = (Mode == TransportMode::kDirectTx);
    static constexpr bool kIsDirect   = (Mode == TransportMode::kDirect);
    static constexpr bool kHasTxThread = kIsThreaded;
    static constexpr bool kHasRxThread = !kIsDirect;
    static constexpr bool kHasTxQueue  = kIsThreaded;
    static constexpr bool kHasRxQueue  = !kIsDirect;

    using TxMsg = detail::TxMessage<MaxPayload>;
    using RxMsg = detail::RxMessage<MaxPayload>;
    using TxQueue = eph::containers::BoundedQueue<TxMsg, QueueDepth>;
    using RxQueue = RxQueueTmpl<RxMsg, QueueDepth>;

public:
    /// Factory callable: creates a new, already-connected TcpImpl instance.
    /// Called during initial connect and on each reconnection attempt.
    using TcpFactory = std::function<
        std::expected<std::unique_ptr<TcpImpl>, std::string>()>;

    static constexpr size_t max_payload() noexcept { return MaxPayload; }
    static constexpr size_t queue_depth() noexcept { return QueueDepth; }
    static constexpr bool   timestamps_enabled() noexcept { return kEnableTimestamps; }

    /// Create and connect a transport (TCP + TLS + WebSocket handshake).
    /// This is a blocking call -- performs the full handshake sequence.
    /// Returns unique_ptr because Transport owns threads and is non-movable.
    ///
    /// On failure, returns ConnectionErrorInfo with a typed error code
    /// for programmatic handling and a detail string for logging.
    [[nodiscard]] static std::expected<std::unique_ptr<Transport>, ConnectionErrorInfo>
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

        // Log non-fatal config warnings before proceeding
        for (const auto& w : config.warnings()) {
            SPDLOG_LOGGER_WARN(log, "Config warning: {}", w);
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
        if constexpr (kIsWebSocket) {
            t->ws_frag_buf_.reserve(MaxPayload);
        }

        t->created_at_ = std::chrono::steady_clock::now();
        t->running_.store(true, std::memory_order_release);
        t->notify_state(TransportEvent::kConnected, config.remote_host);

        // Flush any pending TCP ACK accumulated during handshake.
        // Critical for deferred_start: without this, the server's TCP
        // window fills up and it stops sending data before RX threads start.
        if constexpr (requires { t->tcp_->flush_pending_ack(); }) {
            t->tcp_->flush_pending_ack();
        }

        // Hook: allow caller to configure session (e.g., shared RX ring)
        // after handshake but before threads start polling.
        if (config.on_connected_before_threads) {
            config.on_connected_before_threads();
        }

        // Deferred start: if requested, don't start threads now.
        // Caller must call start_threads() later.
        // In direct modes (kDirect), no threads are needed at all.
        if constexpr (kHasTxThread || kHasRxThread) {
            if (!config.deferred_start) {
                t->start_threads();
            }
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

    // -----------------------------------------------------------------------
    // Send API (application thread)
    // -----------------------------------------------------------------------

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
        if (opcode == ws::opcode::kText && !config_.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        if constexpr (kHasTxQueue) {
            return enqueue_tx(data, len, opcode);
        } else {
            return send_direct(data, len, opcode);
        }
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
        if (!config_.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        if constexpr (kHasTxQueue) {
            return enqueue_tx(data, len, ws::opcode::kText);
        } else {
            return send_direct(data, len, ws::opcode::kText);
        }
    }

    /// Send a string_view as a WebSocket text frame (convenience for JSON APIs).
    /// Validates UTF-8 encoding per RFC 6455 §5.6 unless
    /// TransportConfig::skip_utf8_validation is true.
    [[nodiscard]] SendError send_text(std::string_view sv) noexcept {
        if (!config_.skip_utf8_validation && !ws::is_valid_utf8(sv)) {
            return SendError::kInvalidUtf8;
        }
        if constexpr (kHasTxQueue) {
            return enqueue_tx(sv.data(), sv.size(), ws::opcode::kText);
        } else {
            return send_direct(sv.data(), sv.size(), ws::opcode::kText);
        }
    }

    /// Send a text frame WITHOUT UTF-8 validation (unchecked).
    ///
    /// Use this when you know the payload is valid UTF-8 (e.g., ASCII-only
    /// JSON) and want to skip the validation overhead on the hot path.
    /// If the payload is not valid UTF-8, the remote peer may close the
    /// connection per RFC 6455 §5.6 — this is the caller's responsibility.
    [[nodiscard]] SendError send_text_unchecked(const void* data, size_t len) noexcept {
        if constexpr (kHasTxQueue) {
            return enqueue_tx(data, len, ws::opcode::kText);
        } else {
            return send_direct(data, len, ws::opcode::kText);
        }
    }

    /// Send a string_view as an unchecked text frame (no UTF-8 validation).
    [[nodiscard]] SendError send_text_unchecked(std::string_view sv) noexcept {
        if constexpr (kHasTxQueue) {
            return enqueue_tx(sv.data(), sv.size(), ws::opcode::kText);
        } else {
            return send_direct(sv.data(), sv.size(), ws::opcode::kText);
        }
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
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        // RFC 6455 §5.6: text frames must contain valid UTF-8
        if (opcode == ws::opcode::kText && !config_.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        if constexpr (kHasTxQueue) {
            return enqueue_tx_for(data, len, timeout, opcode);
        } else {
            // Direct mode: timeout is meaningless (no queue), send immediately
            (void)timeout;
            return send_direct(data, len, opcode);
        }
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
    /// Validation is skipped when TransportConfig::skip_utf8_validation is true.
    template <typename Rep, typename Period>
    SendError send_text_for(const void* data, size_t len,
                            std::chrono::duration<Rep, Period> timeout) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (!config_.skip_utf8_validation &&
            !ws::is_valid_utf8(static_cast<const uint8_t*>(data), len)) {
            return SendError::kInvalidUtf8;
        }
        if constexpr (kHasTxQueue) {
            return enqueue_tx_for(data, len, timeout, ws::opcode::kText);
        } else {
            (void)timeout;
            return send_direct(data, len, ws::opcode::kText);
        }
    }

    /// Send a string_view as a WebSocket text frame with timeout.
    template <typename Rep, typename Period>
    SendError send_text_for(std::string_view sv,
                            std::chrono::duration<Rep, Period> timeout) noexcept {
        if (!config_.skip_utf8_validation && !ws::is_valid_utf8(sv)) {
            return SendError::kInvalidUtf8;
        }
        if constexpr (kHasTxQueue) {
            return enqueue_tx_for(sv.data(), sv.size(), timeout, ws::opcode::kText);
        } else {
            (void)timeout;
            return send_direct(sv.data(), sv.size(), ws::opcode::kText);
        }
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
        if (reason_len < reason.size()) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Close reason truncated from {} to 123 bytes (RFC 6455 §5.5 limit)",
                reason.size());
        }
        uint16_t payload_len = static_cast<uint16_t>(2 + reason_len);

        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

        if constexpr (kHasTxQueue) {
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
        } else {
            // Direct mode: encode close frame and send immediately
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
        size_t original_len = payload_len;
        payload_len = std::min(payload_len, size_t{125});
        if (original_len > 125) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Ping payload truncated from {} to 125 bytes (RFC 6455 §5.5 limit)",
                original_len);
        }
        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

        if constexpr (kHasTxQueue) {
            bool ok = tx_queue_.try_produce([&](TxMsg& msg) {
                if (payload && payload_len > 0) {
                    std::memcpy(msg.data, payload, payload_len);
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
        } else {
            // Direct mode: encode ping frame and send immediately
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
        if (count > 0 && !payloads) [[unlikely]] return SendError::kNullData;

        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxPayload) return SendError::kMessageTooLarge;
            // RFC 6455 §5.6: text frames must contain valid UTF-8
            if (opcode == ws::opcode::kText && !config_.skip_utf8_validation &&
                !ws::is_valid_utf8(payloads[i].data(), payloads[i].size())) {
                return SendError::kInvalidUtf8;
            }
        }

        if constexpr (kHasTxQueue) {
            // Write directly into queue slots — no temporary array needed.
            bool ok = tx_queue_.try_produce_n(count,
                [&](TxMsg& slot, size_t i) {
                    std::memcpy(slot.data, payloads[i].data(), payloads[i].size());
                    slot.len = static_cast<uint16_t>(payloads[i].size());
                    slot.opcode = opcode;
                    if constexpr (kEnableTimestamps) {
                        slot.tsc = eph::utils::TSC::now();
                    }
                });

            if (!ok) {
                queue_full_count_.fetch_add(1, std::memory_order_relaxed);
                return SendError::kQueueFull;
            }
            return SendError::kOk;
        } else {
            // Direct mode: send each message individually
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
    SendError send_n_for(const std::span<const uint8_t>* payloads, size_t count,
                         std::chrono::duration<Rep, Period> timeout,
                         uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;
        if (count > 0 && !payloads) [[unlikely]] return SendError::kNullData;

        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxPayload) return SendError::kMessageTooLarge;
            // RFC 6455 §5.6: text frames must contain valid UTF-8
            if (opcode == ws::opcode::kText && !config_.skip_utf8_validation &&
                !ws::is_valid_utf8(payloads[i].data(), payloads[i].size())) {
                return SendError::kInvalidUtf8;
            }
        }

        if constexpr (kHasTxQueue) {
            bool ok = tx_queue_.try_produce_n_for(count,
                [&](TxMsg& slot, size_t i) {
                    std::memcpy(slot.data, payloads[i].data(), payloads[i].size());
                    slot.len = static_cast<uint16_t>(payloads[i].size());
                    slot.opcode = opcode;
                    if constexpr (kEnableTimestamps) {
                        slot.tsc = eph::utils::TSC::now();
                    }
                }, timeout);

            if (!ok) {
                queue_full_count_.fetch_add(1, std::memory_order_relaxed);
                return SendError::kQueueFull;
            }
            return SendError::kOk;
        } else {
            // Direct mode: send each message individually (timeout irrelevant)
            for (size_t i = 0; i < count; ++i) {
                auto err = send_direct(payloads[i].data(), payloads[i].size(), opcode);
                if (err != SendError::kOk) {
                    SPDLOG_LOGGER_WARN(detail::transport_logger(),
                        "send_n_for: send_direct failed at index {}/{}: {}",
                        i, count, static_cast<int>(err));
                    return err;
                }
            }
            return SendError::kOk;
        }
    }

    // -----------------------------------------------------------------------
    // Receive API (application thread)
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
    // -----------------------------------------------------------------------

    /// Try to receive a message (non-blocking).
    /// @param callback  Called with (data_ptr, len) if a message is available.
    /// @return true if a message was consumed, false if queue empty.
    /// @warning The data pointer passed to callback is only valid for the
    ///          duration of the callback invocation. Copy the data if you
    ///          need it after the callback returns -- the underlying SPSC
    ///          queue slot will be reused.
    template <typename F>
        requires (kHasRxQueue && std::invocable<F, const uint8_t*, size_t>)
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
    /// @param callback  Called with (data_ptr, len, opcode) if a message is available.
    ///                  opcode is one of ws::opcode::kBinary, ws::opcode::kText, etc.
    /// @return true if a message was consumed, false if queue empty.
    template <typename F>
        requires (kHasRxQueue && std::invocable<F, const uint8_t*, size_t, uint8_t>)
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
    /// @param callback  Called with (data_ptr, len, opcode, arrival_tsc).
    ///                  arrival_tsc is the raw TSC cycle count captured at
    ///                  rx_burst time (RX thread). Use TSC::to_ns(now - tsc)
    ///                  in the callback to compute per-frame RX latency.
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    /// @return true if a message was consumed, false if queue empty.
    template <typename F>
        requires (kHasRxQueue && std::invocable<F, const uint8_t*, size_t, uint8_t, uint64_t>)
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
    /// Returns the payload bytes, or nullopt if the queue is empty.
    /// Prefer the callback variant for zero-copy hot paths.
    [[nodiscard]] std::optional<std::vector<uint8_t>> try_recv()
        requires (kHasRxQueue)
    {
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
    [[nodiscard]] std::optional<ReceivedMessage> try_recv_msg()
        requires (kHasRxQueue)
    {
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
        requires (kHasRxQueue && std::invocable<F, const uint8_t*, size_t>)
    [[nodiscard]] bool recv_peek(F&& callback) {
        return rx_peek([&](const RxMsg& msg) {
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len));
        });
    }

    /// Peek at the next message with opcode without consuming (non-blocking).
    /// @param callback  Called with (data_ptr, len, opcode) if a message is available.
    /// @return true if a message was peeked, false if queue empty.
    template <typename F>
        requires (kHasRxQueue && std::invocable<F, const uint8_t*, size_t, uint8_t>)
    [[nodiscard]] bool recv_peek(F&& callback) {
        return rx_peek([&](const RxMsg& msg) {
            std::invoke(std::forward<F>(callback),
                        static_cast<const uint8_t*>(msg.data),
                        static_cast<size_t>(msg.len),
                        msg.opcode);
        });
    }

    /// Peek at the next message as a copied ReceivedMessage (non-blocking).
    /// Returns nullopt if the queue is empty.
    [[nodiscard]] std::optional<ReceivedMessage> peek_recv_msg()
        requires (kHasRxQueue)
    {
        std::optional<ReceivedMessage> result;
        (void)rx_peek([&](const RxMsg& msg) {
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
    /// @note Not available when RxQueue is EvictingQueue (latest-value semantics).
    ///       Use recv() in a loop instead.
    ///
    /// @param callback    Called with (data_ptr, len) for each message
    /// @param max_count   Maximum number of messages to consume
    /// @return Number of messages actually consumed
    template <typename F>
        requires (kHasRxQueue && std::invocable<F, const uint8_t*, size_t> && !kRxEvicting)
    [[nodiscard]] size_t recv_n(F&& callback, size_t max_count) {
        return rx_queue_.try_consume_n(max_count,
            [&](const RxMsg& msg, [[maybe_unused]] size_t idx) {
                std::invoke(std::forward<F>(callback), msg.data, msg.len);
            });
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
        requires (kHasRxQueue && std::invocable<F, const uint8_t*, size_t, uint8_t> && !kRxEvicting)
    [[nodiscard]] size_t recv_n(F&& callback, size_t max_count) {
        return rx_queue_.try_consume_n(max_count,
            [&](const RxMsg& msg, [[maybe_unused]] size_t idx) {
                std::invoke(std::forward<F>(callback),
                            msg.data, msg.len, msg.opcode);
            });
    }

    /// Drain all available messages (non-blocking).
    /// Equivalent to recv_n(callback, queue_depth()).
    /// @note Not available when RxQueue is EvictingQueue.
    template <typename F>
        requires (kHasRxQueue && std::invocable<F, const uint8_t*, size_t> && !kRxEvicting)
    [[nodiscard]] size_t drain_recv(F&& callback) {
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
        requires (kHasRxQueue && std::invocable<F, const uint8_t*, size_t>)
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
        requires (kHasRxQueue && std::invocable<F, const uint8_t*, size_t, uint8_t>)
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

    /// Blocking receive returning a ReceivedMessage with opcode and timeout.
    /// Returns nullopt on timeout or transport stopped.
    [[nodiscard]] std::optional<ReceivedMessage> wait_recv_msg(
            std::chrono::milliseconds timeout)
        requires (kHasRxQueue)
    {
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
        if (!running_.load(std::memory_order_acquire)) return false;
        // Store close code/reason so stop() can propagate them in the
        // final Close frame instead of using a hardcoded default.
        // Write code/reason BEFORE setting close_requested_ (release)
        // so that stop() sees consistent values after acquire load (M9).
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

        if constexpr (kHasRxThread) {
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
        }
        // else: kDirect has no RX thread; just stop immediately after sending Close.

        // Server responded with Close (or kDirect: no wait) — stop cleanly
        stop();
        return true;
    }

    // -----------------------------------------------------------------------
    // Direct RX: poll() (kDirect mode only)
    // -----------------------------------------------------------------------

    /// Poll for incoming data. Direct mode only (kDirect).
    ///
    /// Performs one round of: TCP poll_rx → [TLS decrypt] → frame decode.
    /// Decoded messages are delivered via on_message callback (must be set
    /// in TransportConfig). Returns the number of bytes received from TCP,
    /// or an error string if the connection is broken.
    ///
    /// Designed for single-threaded event loops: call poll() repeatedly
    /// to process incoming data. Non-blocking if no data is available.
    // -----------------------------------------------------------------------
    // Direct RX: feed_rx / process_pending / poll (kDirect mode)
    // -----------------------------------------------------------------------

    /// Accumulate raw TCP payload into the reassembly buffer.
    /// Only memcpy — does NOT trigger TLS decrypt or WS decode.
    /// Call from Reactor's on_data callback or any external data source.
    /// Must be called from a single thread (no concurrent calls).
    void feed_rx(const uint8_t* data, uint16_t len) noexcept
        requires (!kHasRxThread)
    {
        auto& rx = direct_rx_;
        auto log = detail::transport_logger();

        // Lazy-init TSC conversion factor on first call
        if (!rx.initialized) [[unlikely]] {
            if constexpr (kEnableTimestamps) {
                auto npc = eph::utils::TSC::get_ns_per_cycle();
                ns_per_cycle_ = npc.value_or(0.0);
            }
            rx.initialized = true;
        }

        if (config_.use_tls) {
            if (rx.reassembly_len + len <= kReassemblyBufSize) {
                std::memcpy(rx.reassembly_storage.get() + rx.reassembly_len,
                            data, len);
                rx.reassembly_len += len;
            } else {
                SPDLOG_LOGGER_ERROR(log,
                    "feed_rx: TLS reassembly overflow ({} + {} > {})",
                    rx.reassembly_len, len, kReassemblyBufSize);
                rx.reassembly_len = 0;
                rx.ws_reassembly_len = 0;
            }
        } else {
            if (rx.ws_reassembly_len + len <= kWsReassemblyBufSize) {
                std::memcpy(rx.ws_reassembly_storage.get() + rx.ws_reassembly_len,
                            data, len);
                rx.ws_reassembly_len += len;
            } else {
                SPDLOG_LOGGER_ERROR(log,
                    "feed_rx: WS reassembly overflow ({} + {} > {})",
                    rx.ws_reassembly_len, len, kWsReassemblyBufSize);
                rx.ws_reassembly_len = 0;
            }
        }
    }

    /// Process all data accumulated by feed_rx().
    /// Executes: TLS decrypt → WS decode → on_message → flush_pending_ack.
    /// Call after one or more feed_rx() calls (e.g., after Reactor burst).
    /// Must be called from the same thread as feed_rx().
    void process_pending() noexcept
        requires (!kHasRxThread)
    {
        auto& rx = direct_rx_;
        auto log = detail::transport_logger();

        // Set arrival TSC for latency measurement
        if constexpr (kEnableTimestamps) {
            if constexpr (requires { tcp_->last_rx_burst_tsc(); }) {
                current_arrival_tsc_ = tcp_->last_rx_burst_tsc();
            }
        }

        // Plain mode: process framed data directly from WS buffer
        if (!config_.use_tls) {
            if (rx.ws_reassembly_len == 0) return;

            size_t ws_consumed = process_frame_data(
                rx.ws_reassembly_storage.get(), rx.ws_reassembly_len);

            ws_consumed = std::min(ws_consumed, rx.ws_reassembly_len);
            size_t ws_remaining = rx.ws_reassembly_len - ws_consumed;
            if (ws_remaining > 0 && ws_consumed > 0) {
                std::memmove(rx.ws_reassembly_storage.get(),
                             rx.ws_reassembly_storage.get() + ws_consumed,
                             ws_remaining);
            }
            rx.ws_reassembly_len = ws_remaining;

            if constexpr (requires { tcp_->flush_pending_ack(); }) {
                tcp_->flush_pending_ack();
            }
            return;
        }

        // TLS mode: decrypt complete records, then process WS frames
        if (rx.reassembly_len == 0) return;

        size_t consumed = 0;
        while (rx.reassembly_len - consumed >=
               tls_record::kRecordHeaderLen + tls_record::kAuthTagLen) {
            const uint8_t* rec_ptr = rx.reassembly_storage.get() + consumed;

            uint8_t content_type;
            uint16_t payload_len;
            if (!tls_record::parse_record_header(rec_ptr, content_type, payload_len))
                break;

            size_t record_total = tls_record::kRecordHeaderLen + payload_len;
            if (rx.reassembly_len - consumed < record_total) break;

            uint16_t decrypted_len;
            bool ok = crypto_->dec.decrypt(
                rec_ptr, static_cast<uint16_t>(record_total),
                rx.decrypt_buf.get(), decrypted_len);

            if (!ok) {
                rx_stats_.crypto_errors.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_LOGGER_WARN(log, "process_pending: TLS decrypt failed");
                break;
            }

            if constexpr (kEnableTimestamps) {
                current_decrypt_done_tsc_ = eph::utils::TSC::now();
            }

            // Handle WS reassembly across TLS records
            const uint8_t* ws_data;
            size_t ws_data_len;
            if (rx.ws_reassembly_len > 0) {
                if (rx.ws_reassembly_len + decrypted_len <= kWsReassemblyBufSize) {
                    std::memcpy(rx.ws_reassembly_storage.get() + rx.ws_reassembly_len,
                                rx.decrypt_buf.get(), decrypted_len);
                    rx.ws_reassembly_len += decrypted_len;
                } else {
                    rx.ws_reassembly_len = 0;
                    consumed += record_total;
                    continue;
                }
                ws_data = rx.ws_reassembly_storage.get();
                ws_data_len = rx.ws_reassembly_len;
            } else {
                ws_data = rx.decrypt_buf.get();
                ws_data_len = decrypted_len;
            }

            size_t ws_consumed = process_frame_data(ws_data, ws_data_len);

            ws_consumed = std::min(ws_consumed, ws_data_len);
            size_t ws_remaining = ws_data_len - ws_consumed;
            if (ws_remaining > 0) {
                if (ws_data == rx.decrypt_buf.get()) {
                    std::memcpy(rx.ws_reassembly_storage.get(),
                                rx.decrypt_buf.get() + ws_consumed, ws_remaining);
                } else {
                    std::memmove(rx.ws_reassembly_storage.get(),
                                 rx.ws_reassembly_storage.get() + ws_consumed,
                                 ws_remaining);
                }
                rx.ws_reassembly_len = ws_remaining;
            } else {
                rx.ws_reassembly_len = 0;
            }

            consumed += record_total;
        }

        // Compact TLS reassembly buffer
        if (consumed > 0) {
            rx.reassembly_len -= consumed;
            if (rx.reassembly_len > 0) {
                std::memmove(rx.reassembly_storage.get(),
                             rx.reassembly_storage.get() + consumed,
                             rx.reassembly_len);
            }
        }

        if constexpr (requires { tcp_->flush_pending_ack(); }) {
            tcp_->flush_pending_ack();
        }
    }

    /// Self-driven poll: burst from TCP + feed + process in one call.
    /// Convenience for kDirect mode without Reactor.
    [[nodiscard]] std::expected<uint16_t, std::string> poll() noexcept
        requires (kIsDirect)
    {
        if (!running_.load(std::memory_order_acquire))
            return std::unexpected(std::string("transport not running"));

        auto rx_result = tcp_->poll_rx(
            [this](const uint8_t* data, uint16_t len) {
                feed_rx(data, len);
            });

        if (!rx_result) {
            running_.store(false, std::memory_order_release);
            return std::unexpected(std::format("TCP rx error: {}", rx_result.error()));
        }

        if (*rx_result == 0) return uint16_t{0};

        process_pending();
        return *rx_result;
    }

    /// Stop the transport gracefully. Sends WebSocket Close frame.
    ///
    /// Thread safety: waits for TX/RX threads to exit BEFORE touching
    /// crypto_ or tcp_, avoiding data races on shared state.
    /// Start RX/TX worker threads. Only needed when TransportConfig::deferred_start
    /// is true. Must be called exactly once after create() returns.
    void start_threads() {
        auto* tp = this;
        if constexpr (kHasTxThread) {
            tx_thread_ = std::thread([tp] { tp->tx_loop(); });
        }
        if constexpr (kHasRxThread) {
            rx_thread_ = std::thread([tp] { tp->rx_loop(); });
        }
    }

    void stop() noexcept {
        bool was_running = running_.exchange(false, std::memory_order_acq_rel);

        auto log = detail::transport_logger();
        SPDLOG_LOGGER_INFO(log, "Stopping transport");

        // Join worker threads FIRST — ensures no concurrent access to
        // crypto_/tcp_ from TX/RX threads when we send the Close frame.
        if constexpr (kHasTxThread) {
            if (tx_thread_.joinable()) tx_thread_.join();
        }
        if constexpr (kHasRxThread) {
            if (rx_thread_.joinable()) rx_thread_.join();
        }

        // Send WebSocket Close frame after threads have exited (no race)
        // Only applicable when using WsFramer — other framers have no
        // close handshake at the framing layer.
        if constexpr (kIsWebSocket) {
            if (was_running && tcp_ && tcp_->is_established() &&
                (config_.use_tls ? crypto_ != nullptr : true)) {
                // Acquire-load close_requested_ to synchronize with the
                // release-store in close_gracefully(), ensuring we see
                // the code/reason written by the app thread (M9).
                uint16_t close_code = ws::close_code::kNormal;
                std::string_view close_reason = "client shutdown";
                if (close_requested_.load(std::memory_order_acquire)) {
                    close_code = pending_close_code_;
                    if (!pending_close_reason_.empty())
                        close_reason = pending_close_reason_;
                }
                // Close frame: header (max 14) + payload (max 125) = 139 bytes,
                // plus 1 byte for encrypt()'s temporary content type append.
                uint8_t close_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
                size_t close_len = ws::build_close_frame(
                    close_buf, close_code, close_reason);

                if (config_.use_tls) {
                    // TLS output: record header + ciphertext + content type + auth tag
                    uint8_t tls_buf[TlsRecordCrypto::encrypted_size(
                        ws::kMaxFrameHeaderLen + 125)]{};
                    uint16_t tls_len = crypto_->encrypt(
                        close_buf, static_cast<uint16_t>(close_len), tls_buf);
                    if (tls_len > 0) {
                        (void)tcp_->send(tls_buf, tls_len);
                    }
                } else {
                    // Plain WS: send close frame directly over TCP
                    (void)tcp_->send(close_buf, close_len);
                }
            }
        }

        // Close TCP connection
        if (tcp_ && tcp_->is_established()) {
            (void)tcp_->close();
        }

        notify_state(TransportEvent::kStopped);
        SPDLOG_LOGGER_INFO(log, "Transport stopped");
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// Read-only access to the configuration used to create this transport.
    /// Useful for logging, diagnostics, and reconnection-aware logic that
    /// needs to inspect remote_host, ping_interval, etc. at runtime.
    [[nodiscard]] const TransportConfig& config() const noexcept {
        return config_;
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
        if (!running_.load(std::memory_order_acquire)) return false;
        if (config_.max_reconnect_attempts <= 0) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "reconnect_now() called but auto-reconnect is disabled "
                "(max_reconnect_attempts=0)");
            return false;
        }
        SPDLOG_LOGGER_INFO(detail::transport_logger(),
            "reconnect_now() signaled by application");
        force_reconnect_.store(true, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // Queue occupancy (backpressure monitoring)
    // -----------------------------------------------------------------------

    /// Approximate number of messages pending in the TX queue.
    /// Useful for detecting backpressure before send() returns -EAGAIN.
    /// @note Result is approximate — the producer and consumer may
    ///       advance between the size() read and the caller's use.
    [[nodiscard]] size_t tx_queue_size() const noexcept
        requires (kHasTxQueue)
    {
        return tx_queue_.size();
    }

    /// Approximate number of messages available in the RX queue.
    [[nodiscard]] size_t rx_queue_size() const noexcept
        requires (kHasRxQueue)
    {
        return rx_size();
    }

    /// TX queue occupancy as a fraction [0.0, 1.0].
    [[nodiscard]] double tx_queue_fill_ratio() const noexcept
        requires (kHasTxQueue)
    {
        return static_cast<double>(tx_queue_.size()) /
               static_cast<double>(QueueDepth);
    }

    /// RX queue occupancy as a fraction [0.0, 1.0].
    [[nodiscard]] double rx_queue_fill_ratio() const noexcept
        requires (kHasRxQueue)
    {
        return static_cast<double>(rx_size()) /
               static_cast<double>(QueueDepth);
    }

    /// Peak TX queue occupancy since creation or last reset_stats().
    /// Useful for diagnosing transient backpressure spikes that
    /// instantaneous tx_queue_size() would miss.
    [[nodiscard]] size_t tx_queue_hwm() const noexcept {
        return tx_hwm_.load(std::memory_order_relaxed);
    }

    /// Peak RX queue occupancy since creation or last reset_stats().
    [[nodiscard]] size_t rx_queue_hwm() const noexcept {
        return rx_hwm_.load(std::memory_order_relaxed);
    }

    /// Reset all statistics counters to zero.
    /// Useful for windowed measurement: call stats(), then reset_stats().
    /// @warning Not thread-safe with stats() — call from one thread only
    ///          (typically the application thread between measurement windows).
    void reset_stats() noexcept {
        tx_stats_.reset();
        rx_stats_.reset();
        queue_full_count_.store(0, std::memory_order_relaxed);
        ws_pings_received_.store(0, std::memory_order_relaxed);
        ws_pongs_sent_.store(0, std::memory_order_relaxed);
        pong_timeouts_.store(0, std::memory_order_relaxed);
        reconnect_count_.store(0, std::memory_order_relaxed);
        tx_hwm_.store(0, std::memory_order_relaxed);
        rx_hwm_.store(0, std::memory_order_relaxed);
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

    /// Aggregated connection metadata snapshot.
    /// Combines tls_version, cipher_name, ws_subprotocol, and remote_ip
    /// into a single struct for convenient logging and monitoring.
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
        return histogram_to_stats(rtt_histogram_);
    }

    /// TX queue latency stats (enqueue → flush).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats tx_latency_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "tx_latency_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(tx_latency_histogram_);
    }

    /// TX queue wait stats (enqueue → drain).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats tx_queue_wait_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "tx_queue_wait_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(tx_queue_wait_histogram_);
    }

    /// TX encode+encrypt stats (drain → flush).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats tx_encode_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "tx_encode_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(tx_encode_histogram_);
    }

    /// RX pipeline latency stats (arrival → deliver).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats rx_latency_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_latency_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(rx_latency_histogram_);
    }

    /// RX decrypt stats (arrival → decrypt done).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats rx_decrypt_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_decrypt_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(rx_decrypt_histogram_);
    }

    /// RX decode stats (decrypt done → frame decoded).
    /// Requires -DEPH_ENABLE_TIMESTAMPS=1 at compile time.
    [[nodiscard]] RttStats rx_decode_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_decode_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(rx_decode_histogram_);
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
        return rx_latency_histogram_;
    }

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
            .tx_queue_hwm      = tx_hwm_.load(std::memory_order_relaxed),
            .rx_queue_hwm      = rx_hwm_.load(std::memory_order_relaxed),
            .uptime_ns         = static_cast<uint64_t>(uptime > 0 ? uptime : 0),
            .handshake_ns      = last_handshake_ns_,
            .tcp_connect_ns    = last_tcp_connect_ns_,
            .tls_handshake_ns  = last_tls_handshake_ns_,
            .ws_upgrade_ns     = last_ws_upgrade_ns_,
            .remote_ip         = remote_ip_,
            .rtt               = rtt_stats(),
            .tx_latency        = histogram_to_stats(tx_latency_histogram_),
            .tx_queue_wait     = histogram_to_stats(tx_queue_wait_histogram_),
            .tx_encode         = histogram_to_stats(tx_encode_histogram_),
            .rx_latency        = histogram_to_stats(rx_latency_histogram_),
            .rx_decrypt        = histogram_to_stats(rx_decrypt_histogram_),
            .rx_decode         = histogram_to_stats(rx_decode_histogram_),
            .tls_write_seq     = crypto_ ? crypto_->write_seq() : 0,
            .tls_read_seq      = crypto_ ? crypto_->read_seq() : 0,
            .tls_seq_limit     = config_.use_tls ? tls_record::kMaxSequenceNumber : 0,
        };
    }

private:
    Transport() = default;

    // -----------------------------------------------------------------------
    // Internal enqueue helpers (no UTF-8 validation — caller is responsible)
    // -----------------------------------------------------------------------

    /// Convert an HdrHistogram to RttStats (reused for RTT, TX latency, RX latency).
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

    /// Update a high-watermark atomically (relaxed CAS loop).
    /// Only stores when current size exceeds the recorded peak.
    static void update_hwm(std::atomic<size_t>& hwm, size_t current) noexcept {
        size_t prev = hwm.load(std::memory_order_relaxed);
        while (current > prev &&
               !hwm.compare_exchange_weak(prev, current,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {
            // prev updated by CAS failure — retry
        }
    }

    // -----------------------------------------------------------------------
    // RxQueue dispatch helpers — abstract BoundedQueue vs EvictingQueue
    // -----------------------------------------------------------------------

    /// Enqueue a decoded message into the RX queue.
    /// BoundedQueue: try_produce (backpressure — may fail).
    /// EvictingQueue: push (evicts oldest — never fails).
    bool rx_enqueue(const uint8_t* data, uint16_t len,
                    uint8_t opcode) noexcept
        requires (kHasRxQueue)
    {
        if constexpr (kRxEvicting) {
            // Write directly into the queue slot via produce() to avoid
            // allocating a 16KB+ RxMsg on the stack and then copying the
            // entire struct into the slot.  Only `len` bytes are copied.
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

    /// Consume one message from the RX queue.
    /// BoundedQueue: try_consume (FIFO).
    /// EvictingQueue: try_consume_latest (latest-value).
    template <typename F>
        requires (kHasRxQueue)
    bool rx_consume(F&& visitor) {
        if constexpr (kRxEvicting) {
            return rx_queue_.try_consume_latest(std::forward<F>(visitor));
        } else {
            return rx_queue_.try_consume(std::forward<F>(visitor));
        }
    }

    /// Peek one message from the RX queue without consuming.
    template <typename F>
        requires (kHasRxQueue)
    bool rx_peek(F&& visitor) {
        if constexpr (kRxEvicting) {
            return rx_queue_.try_peek_latest(std::forward<F>(visitor));
        } else {
            return rx_queue_.try_peek(std::forward<F>(visitor));
        }
    }

    /// Approximate RX queue size.
    [[nodiscard]] size_t rx_size() const noexcept
        requires (kHasRxQueue)
    {
        if constexpr (kRxEvicting) {
            return rx_queue_.size_approx();
        } else {
            return rx_queue_.size();
        }
    }

    // -----------------------------------------------------------------------

    /// Enqueue a message to TX queue without UTF-8 validation.
    /// Shared implementation for send() and send_text() to avoid
    /// double-validating UTF-8 on the hot path.
    SendError enqueue_tx(const void* data, size_t len,
                         uint8_t opcode) noexcept
        requires (kHasTxQueue)
    {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (len > MaxPayload) return SendError::kMessageTooLarge;
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;

        bool ok = tx_queue_.try_produce([&](TxMsg& msg) {
            std::memcpy(msg.data, data, len);
            msg.len = static_cast<uint16_t>(len);
            msg.opcode = opcode;
            if constexpr (kEnableTimestamps) {
                msg.tsc = eph::utils::TSC::now();
            }
        });

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            // Queue full → HWM is max capacity
            update_hwm(tx_hwm_, QueueDepth);
            SPDLOG_LOGGER_TRACE(detail::transport_logger(),
                "TX enqueue failed: queue full (len={}, opcode={})",
                len, opcode);
            return SendError::kQueueFull;
        }
        SPDLOG_LOGGER_TRACE(detail::transport_logger(),
            "TX enqueue: len={}, opcode={}", len, opcode);
        // Sample HWM every 64 enqueues to avoid cross-core size() read
        // on every send(). size() reads both writer tail and reader head
        // which sit on separate cache lines — expensive on every call.
        if ((++tx_hwm_counter_ & 63) == 0) {
            update_hwm(tx_hwm_, tx_queue_.size());
        }
        return SendError::kOk;
    }

    /// Enqueue with timeout, no UTF-8 validation.
    template <typename Rep, typename Period>
        requires (kHasTxQueue)
    SendError enqueue_tx_for(const void* data, size_t len,
                             std::chrono::duration<Rep, Period> timeout,
                             uint8_t opcode) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        if (len > MaxPayload) return SendError::kMessageTooLarge;
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;

        bool ok = tx_queue_.try_produce_for([&](TxMsg& msg) {
            std::memcpy(msg.data, data, len);
            msg.len = static_cast<uint16_t>(len);
            msg.opcode = opcode;
            if constexpr (kEnableTimestamps) {
                msg.tsc = eph::utils::TSC::now();
            }
        }, timeout);

        if (!ok) {
            queue_full_count_.fetch_add(1, std::memory_order_relaxed);
            update_hwm(tx_hwm_, QueueDepth);
            return SendError::kQueueFull;
        }
        if ((++tx_hwm_counter_ & 63) == 0) {
            update_hwm(tx_hwm_, tx_queue_.size());
        }
        return SendError::kOk;
    }

    /// Direct send: WS encode -> [TLS encrypt] -> TCP send.
    /// Used in kDirectTx and kDirect modes — no SPSC queue involved.
    /// Called from the application thread; crypto_->enc is exclusively
    /// owned by the send caller (safe: RX thread only uses crypto_->dec).
    SendError send_direct(const void* data, size_t len, uint8_t opcode) noexcept
        requires (!kHasTxQueue)
    {
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

    TransportConfig                        config_;
    TcpFactory                             tcp_factory_;
    uint64_t                               current_arrival_tsc_{0};
    // NOTE: current_decrypt_done_tsc_ is set once per TLS record, not per WS frame.
    // A single TLS record may contain multiple WS frames; the decrypt histogram
    // therefore reflects per-TLS-record decryption latency, not per-WS-frame.
    uint64_t                               current_decrypt_done_tsc_{0};
    double                                 ns_per_cycle_{0.0};  // cached at rx_loop entry
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

    [[no_unique_address]]
    std::conditional_t<kHasTxQueue, TxQueue, detail::Empty>  tx_queue_{};
    [[no_unique_address]]
    std::conditional_t<kHasRxQueue, RxQueue, detail::Empty>  rx_queue_{};
    Framer                                 rx_framer_{};  // RX-side framer instance (stateless framers: zero overhead)

    std::atomic<bool>                      running_{false};
    // RX sets reconnecting_=true before modifying crypto_/tcp_;
    // TX spins while this flag is set to avoid data races.
    std::atomic<bool>                      reconnecting_{false};
    // RX sets closing_=true when a server Close frame is received.
    // TX drains the queue (sending the Close response) before exiting.
    std::atomic<bool>                      closing_{false};
    // Application sets force_reconnect_=true via reconnect_now();
    // RX thread checks this and triggers do_reconnect() when set.
    std::atomic<bool>                      force_reconnect_{false};
    [[no_unique_address]]
    std::conditional_t<kHasTxThread, std::thread, detail::Empty>  tx_thread_{};
    [[no_unique_address]]
    std::conditional_t<kHasRxThread, std::thread, detail::Empty>  rx_thread_{};

    // Per-thread stats to avoid cross-core atomic contention
    ThreadStats                            tx_stats_{};
    ThreadStats                            rx_stats_{};

    // Queue high-watermark: peak occupancy since creation or last reset.
    // Updated by enqueue (TX) and deliver (RX) paths with relaxed atomics.
    // Sampled every 64 operations to avoid cross-core size() reads.
    std::atomic<size_t>                    tx_hwm_{0};
    std::atomic<size_t>                    rx_hwm_{0};
    uint64_t                               tx_hwm_counter_{0};  // app thread only
    uint64_t                               rx_hwm_counter_{0};  // RX thread only
    // App-thread-only counters (no contention -- only send() writes these)
    std::atomic<uint64_t>                  queue_full_count_{0};
    std::atomic<uint64_t>                  ws_pings_received_{0};
    std::atomic<uint64_t>                  ws_pongs_sent_{0};
    std::atomic<uint64_t>                  reconnect_count_{0};
    std::atomic<uint64_t>                  pong_timeouts_{0};
    uint64_t                               last_handshake_ns_{0};
    uint64_t                               last_tcp_connect_ns_{0};
    uint64_t                               last_tls_handshake_ns_{0};
    uint64_t                               last_ws_upgrade_ns_{0};

    // Pong timeout tracking (TX thread writes ping time, RX thread writes pong time).
    // Using atomics with relaxed ordering — occasional stale reads are acceptable
    // since pong timeout detection is a best-effort liveness check, not a
    // precision requirement.
    using SteadyTimePoint = std::chrono::steady_clock::time_point;
    std::atomic<int64_t>                   last_pong_ns_{0};      // RX writes, TX reads

    // -- Per-thread variables: no synchronization needed, private to owning thread --
    // TX-thread-only (written/read exclusively by tx_loop):
    bool                                   ping_awaiting_pong_{false};
    bool                                   seq_warning_logged_{false};
    // RX-thread-only (written/read exclusively by rx_loop):
    bool                                   rx_seq_warning_logged_{false};

    // Close code/reason from close_gracefully(), propagated to stop().
    // Written by the app thread (close_gracefully) and read by stop()
    // after threads have joined. The close_requested_ flag provides
    // happens-before ordering: code/reason are written BEFORE setting
    // the flag (release), and read AFTER loading it (acquire) (M9).
    uint16_t                               pending_close_code_{ws::close_code::kNormal};
    std::string                            pending_close_reason_{};
    std::atomic<bool>                      close_requested_{false};

    // RTT measurement: TX thread writes TSC timestamp when sending ping,
    // RX thread reads it when pong arrives and records the delta in the
    // histogram.  The histogram is owned exclusively by the RX thread.
    std::atomic<uint64_t>                  last_ping_tsc_{0};     // TX writes, RX reads
    eph::utils::HdrHistogram               rtt_histogram_{
        100,          // lowest: 100 ns (~0.1 us)
        10'000'000'000ULL, // highest: 10 s (covers even slow WAN)
        3             // 3 significant digits
    };

    // Per-message latency histograms (only recorded when kEnableTimestamps=true).
    //
    // TX total:       enqueue (app) → flush (encode+encrypt done)
    // TX queue wait:  enqueue (app) → drain (TX thread picks up from SPSC)
    // TX encode:      drain → flush (WS encode + TLS encrypt)
    //
    // RX total:       arrival (poll_rx) → frame decoded
    // RX decrypt:     arrival → TLS decrypt done
    // RX decode:      TLS decrypt done → WS frame decoded
    eph::utils::HdrHistogram               tx_latency_histogram_{
        10, 1'000'000'000ULL, 3  // 10ns–1s, 3 significant digits
    };
    eph::utils::HdrHistogram               tx_queue_wait_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               tx_encode_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               rx_latency_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               rx_decrypt_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               rx_decode_histogram_{
        10, 1'000'000'000ULL, 3
    };

    // WebSocket fragmentation reassembly buffer (RX thread only).
    // Accumulates continuation frames until FIN=1.
    std::vector<uint8_t>                   ws_frag_buf_;
    uint8_t                                ws_frag_opcode_ = 0;

    // -----------------------------------------------------------------------
    // kDirect mode: persistent reassembly buffers for poll()
    // -----------------------------------------------------------------------
    // In kThreaded/kDirectTx, these are stack-allocated inside rx_loop().
    // In kDirect, poll() returns between calls so state must persist.

    static constexpr size_t kReassemblyBufSize =
        4 * (tls_const::kMaxRecordPayload + tls_record::kRecordHeaderLen +
             tls_record::kAuthTagLen + 1);
    static constexpr size_t kFrameReassemblyOverhead = kIsWebSocket
        ? ws::kMaxFrameHeaderLen : Framer::max_overhead();
    static constexpr size_t kWsReassemblyBufSize =
        kFrameReassemblyOverhead + MaxPayload + 256;

    struct DirectRxState {
        std::unique_ptr<uint8_t[]> decrypt_buf =
            std::make_unique<uint8_t[]>(tls_const::kMaxRecordPayload + 256);
        std::unique_ptr<uint8_t[]> reassembly_storage =
            std::make_unique<uint8_t[]>(kReassemblyBufSize);
        size_t reassembly_len = 0;
        std::unique_ptr<uint8_t[]> ws_reassembly_storage =
            std::make_unique<uint8_t[]>(kWsReassemblyBufSize);
        size_t ws_reassembly_len = 0;
        bool initialized = false;
    };

    [[no_unique_address]]
    std::conditional_t<kIsDirect, DirectRxState, detail::Empty> direct_rx_{};

    // Private method implementations (split for readability)
#include "eph/transport/detail/transport_state.hpp"
#include "eph/transport/detail/transport_tx.hpp"
#include "eph/transport/detail/transport_rx.hpp"
#include "eph/transport/detail/transport_frame.hpp"
};

} // namespace eph::net
