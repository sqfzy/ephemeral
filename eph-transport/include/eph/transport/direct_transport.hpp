#pragma once

/// @file direct_transport.hpp
/// Direct (threadless) WebSocket transport — the kDirect variant.
///
/// NO threads, NO queues. The application thread does everything:
///   send_direct() for TX, feed_rx()/process_pending()/poll() for RX.
///
/// Designed for single-threaded event loops (Reactor pattern, io_uring,
/// DPDK poll-mode) where the app already owns the polling thread and
/// adding a background RX thread would only add latency and jitter.
///
/// Usage:
///   auto transport = DirectTransport<MyTcp>::create(factory, config);
///   transport->send(data, len);         // encode + encrypt + TCP send
///   transport->poll();                  // TCP poll + decrypt + decode
///   // or split: feed_rx() + process_pending() for Reactor integration

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "eph/utils/alignment.hpp"
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
// DirectTransport — threadless, queueless transport
// ---------------------------------------------------------------------------

/// Threadless WebSocket transport with TLS 1.3 encryption.
///
/// Template parameters:
///   TcpImpl    -- a type satisfying the TcpTransport concept
///   Framer     -- message framer (default: WsFramer for WebSocket)
///   MaxPayload -- maximum application payload size per message
///
/// All send/receive operations happen on the calling thread.
/// No SPSC queues, no background threads — minimal latency path.
template <TcpTransport TcpImpl, MessageFramer Framer = WsFramer,
          size_t MaxPayload = 512>
class DirectTransport {
    static_assert(TcpTransport<TcpImpl>,
                  "TcpImpl must satisfy TcpTransport concept");
    static_assert(MessageFramer<Framer>,
                  "Framer must satisfy MessageFramer concept");
    static_assert(MaxPayload > 0, "MaxPayload must be > 0");
    static_assert(MaxPayload <= tls_const::kMaxRecordPayload,
                  "MaxPayload exceeds TLS max record size (16384)");

    // -- Capability constants: no threads, no queues --
    static constexpr bool kHasTxThread = false;
    static constexpr bool kHasTxQueue  = false;
    static constexpr bool kHasRxThread = false;
    static constexpr bool kHasRxQueue  = false;

    /// True when using WebSocket framing (enables WS handshake, ping/pong,
    /// close handshake, and fragmentation reassembly).
    static constexpr bool kIsWebSocket = std::is_same_v<Framer, WsFramer>;

    /// No queue = no eviction possible.
    static constexpr bool kRxEvicting = false;

    /// No queue = no last-only delivery (all frames delivered inline).
    static constexpr bool kLastOnlyDeliver = false;

    /// Placeholder for template compatibility with Transport stats API.
    static constexpr size_t QueueDepth = 1;

public:
    /// Factory callable: creates a new, already-connected TcpImpl instance.
    using TcpFactory = std::function<
        std::expected<std::unique_ptr<TcpImpl>, std::string>()>;

    static constexpr size_t max_payload() noexcept { return MaxPayload; }
    static constexpr size_t queue_depth() noexcept { return QueueDepth; }
    static constexpr bool   timestamps_enabled() noexcept { return kEnableTimestamps; }

    // -----------------------------------------------------------------------
    // Factory
    // -----------------------------------------------------------------------

    /// Create and connect a direct transport (TCP + TLS + WebSocket handshake).
    /// This is a blocking call — performs the full handshake sequence.
    /// NO threads are started (this is the point of DirectTransport).
    [[nodiscard]] static std::expected<std::unique_ptr<DirectTransport>, ConnectionErrorInfo>
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
            "Creating direct transport: {}:{}{}",
            config.remote_host, config.remote_port, config.ws_path);

        auto t = std::unique_ptr<DirectTransport>(new DirectTransport());
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
        if constexpr (requires { t->tcp_->flush_pending_ack(); }) {
            t->tcp_->flush_pending_ack();
        }

        // Hook: allow caller to configure session (e.g., shared RX ring)
        // after handshake completes.
        if (config.on_connected_before_threads) {
            config.on_connected_before_threads();
        }

        SPDLOG_LOGGER_INFO(log, "Direct transport ready: {}", config.remote_host);

        return t;
    }

    ~DirectTransport() {
        stop();
    }

    DirectTransport(const DirectTransport&)            = delete;
    DirectTransport& operator=(const DirectTransport&) = delete;
    DirectTransport(DirectTransport&&)                 = delete;
    DirectTransport& operator=(DirectTransport&&)      = delete;

    // -----------------------------------------------------------------------
    // Send API (application thread — direct encode + encrypt + TCP send)
    // -----------------------------------------------------------------------

    /// Send data as a WebSocket frame (synchronous, direct).
    ///
    /// Semantics: the data is encoded, encrypted, and sent on the wire
    /// before this call returns. No queue involved.
    [[nodiscard]] SendError send(const void* data, size_t len,
                   uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (len > 0 && !data) [[unlikely]] return SendError::kNullData;
        // RFC 6455 §5.6: text frames must contain valid UTF-8
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

    /// Send a string_view as a WebSocket binary frame.
    [[nodiscard]] SendError send_binary(std::string_view sv) noexcept {
        return send(sv.data(), sv.size(), ws::opcode::kBinary);
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

    /// Send a WebSocket Close frame with a custom status code and reason.
    SendError send_close(uint16_t status_code,
                         std::string_view reason = {}) noexcept {
        if (!running_.load(std::memory_order_acquire)) return SendError::kNotConnected;
        if (!ws::is_valid_close_code(status_code)) return SendError::kInvalidCloseCode;
        // RFC 6455 §7.1.6: close reason must be valid UTF-8
        if (!reason.empty() && !ws::is_valid_utf8(reason)) return SendError::kInvalidUtf8;

        size_t reason_len = std::min(reason.size(), size_t{123});
        if (reason_len < reason.size()) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Close reason truncated from {} to 123 bytes (RFC 6455 §5.5 limit)",
                reason.size());
        }
        uint16_t payload_len = static_cast<uint16_t>(2 + reason_len);

        if (payload_len > MaxPayload) return SendError::kMessageTooLarge;

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

    /// Send a WebSocket Ping frame to probe connection liveness.
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

    /// Batch-send multiple messages (all-or-nothing within reason).
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

    // -----------------------------------------------------------------------
    // Direct RX: feed_rx / process_pending / poll
    // -----------------------------------------------------------------------

    /// Accumulate raw TCP payload into the reassembly buffer.
    /// Only memcpy — does NOT trigger TLS decrypt or WS decode.
    /// Call from Reactor's on_data callback or any external data source.
    /// Must be called from a single thread (no concurrent calls).
    void feed_rx(const uint8_t* data, uint16_t len) noexcept {
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
    /// Executes: TLS decrypt -> WS decode -> on_message -> flush_pending_ack.
    /// Call after one or more feed_rx() calls (e.g., after Reactor burst).
    /// Must be called from the same thread as feed_rx().
    void process_pending() noexcept {
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
    /// Convenience for direct mode without Reactor.
    [[nodiscard]] std::expected<uint16_t, std::string> poll() noexcept {
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

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Initiate a graceful WebSocket close handshake (RFC 6455 §7.1.1).
    ///
    /// In direct mode there is no RX thread to wait for the server Close
    /// response, so this sends the Close frame and stops immediately.
    bool close_gracefully(
            uint16_t status_code = ws::close_code::kNormal,
            std::string_view reason = "client shutdown",
            [[maybe_unused]] std::chrono::milliseconds timeout =
                std::chrono::milliseconds{3000}) noexcept {
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

        // kDirect: no RX thread to wait for server Close — stop immediately
        stop();
        return true;
    }

    /// Stop the transport. Sends WebSocket Close frame, closes TCP.
    /// No threads to join (this is DirectTransport).
    void stop() noexcept {
        bool was_running = running_.exchange(false, std::memory_order_acq_rel);

        auto log = detail::transport_logger();
        SPDLOG_LOGGER_INFO(log, "Stopping direct transport");

        // Send WebSocket Close frame (no thread race — single thread)
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
        SPDLOG_LOGGER_INFO(log, "Direct transport stopped");
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

    // -----------------------------------------------------------------------
    // Stats & diagnostics
    // -----------------------------------------------------------------------

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

    [[nodiscard]] RttStats tx_latency_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "tx_latency_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(tx_latency_histogram_);
    }

    [[nodiscard]] RttStats tx_queue_wait_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "tx_queue_wait_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(tx_queue_wait_histogram_);
    }

    [[nodiscard]] RttStats tx_encode_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "tx_encode_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(tx_encode_histogram_);
    }

    [[nodiscard]] RttStats rx_latency_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_latency_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(rx_latency_histogram_);
    }

    [[nodiscard]] RttStats rx_decrypt_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_decrypt_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(rx_decrypt_histogram_);
    }

    [[nodiscard]] RttStats rx_decode_stats() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_decode_stats() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return histogram_to_stats(rx_decode_histogram_);
    }

    [[nodiscard]] eph::utils::HdrHistogram rx_latency_histogram_snapshot() const noexcept {
        static_assert(kEnableTimestamps,
            "rx_latency_histogram_snapshot() requires -DEPH_ENABLE_TIMESTAMPS=1");
        return rx_latency_histogram_;
    }

    void reset_stats() noexcept {
        tx_stats_.reset();
        rx_stats_.reset();
        queue_full_count_.store(0, std::memory_order_relaxed);
        ws_pings_received_.store(0, std::memory_order_relaxed);
        ws_pongs_sent_.store(0, std::memory_order_relaxed);
        pong_timeouts_.store(0, std::memory_order_relaxed);
        reconnect_count_.store(0, std::memory_order_relaxed);
        rtt_histogram_.reset();
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
            .tx_queue_hwm      = 0,  // no TX queue in direct mode
            .rx_queue_hwm      = 0,  // no RX queue in direct mode
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
    DirectTransport() = default;

    // -----------------------------------------------------------------------
    // Direct send: WS encode -> [TLS encrypt] -> TCP send
    // -----------------------------------------------------------------------

    /// Direct send: frame encode -> [TLS encrypt] -> TCP send.
    /// Called from the application thread; no SPSC queue involved.
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
    // Histogram helper
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

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    // Connection state
    TransportConfig                        config_;
    TcpFactory                             tcp_factory_;
    std::unique_ptr<TcpImpl>               tcp_;
    std::unique_ptr<TlsSession<TcpImpl>>   tls_;   // Only used during create(), not on hot path
    std::unique_ptr<TlsRecordCrypto>       crypto_;

    // Connection metadata captured after each successful handshake
    std::string                            tls_version_{"none"};
    std::string                            cipher_name_{"none"};
    std::string                            ws_subprotocol_{};
    std::string                            remote_ip_{};

    // Uptime tracking
    std::chrono::steady_clock::time_point  created_at_{};

    // Stub types for detail file compatibility — referenced inside
    // if constexpr dead branches (kHasTxQueue=false, kHasRxQueue=false).
    // Never used at runtime.
    using TxMsg = detail::TxMessage<MaxPayload>;
    using RxMsg = detail::TxMessage<MaxPayload>; // reuse TxMessage shape
    struct QueueStub_ {
        void clear() noexcept {}
        template<typename F> bool try_produce(F&&) noexcept { return false; }
        template<typename F> bool try_consume(F&&) noexcept { return false; }
        size_t size() const noexcept { return 0; }
    };
    [[no_unique_address]] QueueStub_       tx_queue_{};
    [[no_unique_address]] QueueStub_       rx_queue_{};
    std::atomic<size_t>                    tx_hwm_{0};
    std::atomic<size_t>                    rx_hwm_{0};
    uint64_t                               tx_hwm_counter_{0};
    uint64_t                               rx_hwm_counter_{0};

    /// Stub rx_enqueue — never called (kHasRxQueue=false), but name must exist
    /// for if constexpr dead branch in deliver_message().
    bool rx_enqueue(const uint8_t*, uint16_t, uint8_t) noexcept { return false; }
    size_t rx_size() const noexcept { return 0; }

    /// Update a high-watermark atomically (shared helper for detail files).
    static void update_hwm(std::atomic<size_t>& hwm, size_t current) noexcept {
        size_t prev = hwm.load(std::memory_order_relaxed);
        while (current > prev &&
               !hwm.compare_exchange_weak(prev, current,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    // Lifecycle atomics
    std::atomic<bool>                      running_{false};
    std::atomic<bool>                      reconnecting_{false};
    std::atomic<bool>                      closing_{false};
    std::atomic<bool>                      force_reconnect_{false};

    // Per-thread stats (no cross-core contention — single thread in direct mode)
    ThreadStats                            tx_stats_{};
    ThreadStats                            rx_stats_{};

    // App-thread-only counters
    std::atomic<uint64_t>                  queue_full_count_{0};
    std::atomic<uint64_t>                  ws_pings_received_{0};
    std::atomic<uint64_t>                  ws_pongs_sent_{0};
    std::atomic<uint64_t>                  reconnect_count_{0};
    std::atomic<uint64_t>                  pong_timeouts_{0};

    // Handshake timing
    uint64_t                               last_handshake_ns_{0};
    uint64_t                               last_tcp_connect_ns_{0};
    uint64_t                               last_tls_handshake_ns_{0};
    uint64_t                               last_ws_upgrade_ns_{0};

    // TSC timing for latency measurement
    uint64_t                               current_arrival_tsc_{0};
    uint64_t                               current_decrypt_done_tsc_{0};
    double                                 ns_per_cycle_{0.0};

    // Pong timeout tracking
    std::atomic<int64_t>                   last_pong_ns_{0};
    std::atomic<uint64_t>                  last_ping_tsc_{0};

    // Per-thread flags (single thread — no synchronization needed)
    bool                                   ping_awaiting_pong_{false};
    bool                                   seq_warning_logged_{false};
    bool                                   rx_seq_warning_logged_{false};

    // Close handshake
    uint16_t                               pending_close_code_{ws::close_code::kNormal};
    std::string                            pending_close_reason_{};
    std::atomic<bool>                      close_requested_{false};

    // Histograms: RTT
    eph::utils::HdrHistogram               rtt_histogram_{
        100, 10'000'000'000ULL, 3
    };

    // TX histograms (declared for stats() API compatibility — always empty in direct mode)
    eph::utils::HdrHistogram               tx_latency_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               tx_queue_wait_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               tx_encode_histogram_{
        10, 1'000'000'000ULL, 3
    };

    // RX histograms
    eph::utils::HdrHistogram               rx_latency_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               rx_decrypt_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram               rx_decode_histogram_{
        10, 1'000'000'000ULL, 3
    };

    // WebSocket fragmentation reassembly buffer (single thread only)
    std::vector<uint8_t>                   ws_frag_buf_;
    uint8_t                                ws_frag_opcode_ = 0;
    Framer                                 rx_framer_{};

    // -----------------------------------------------------------------------
    // Direct RX state: persistent reassembly buffers for poll()
    // -----------------------------------------------------------------------

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
    DirectRxState direct_rx_{};

    // Private method implementations (shared detail files)
#include "eph/transport/detail/transport_state.hpp"
#include "eph/transport/detail/transport_frame.hpp"
};

} // namespace eph::net
