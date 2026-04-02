#pragma once

/// @file direct_transport.hpp
/// Direct (threadless) WebSocket transport — composes FrameProcessor +
/// TransportCore + ReconnectPolicy.
///
/// NO threads, NO queues. The application thread does everything:
///   send() for TX, feed_rx()/process_pending()/poll() for RX.
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
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <spdlog/spdlog.h>

#include "eph/core/framer_concept.hpp"
#include "eph/core/tcp_concept.hpp"
#include "eph/core/transport_errors.hpp"
#include "eph/transport/frame_processor.hpp"
#include "eph/transport/http.hpp"
#include "eph/transport/reconnect_policy.hpp"
#include "eph/transport/tls_decryptor.hpp"
#include "eph/transport/tls_encryptor.hpp"
#include "eph/transport/tls_record.hpp"
#include "eph/transport/tls_session.hpp"
#include "eph/transport/transport_core.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/transport/websocket.hpp"
#include "eph/transport/ws_framer.hpp"
#include "eph/utils/hdr_histogram.hpp"
#include "eph/utils/time.hpp"

namespace eph::net {

// Ensure kEnableTimestamps is available in this translation unit.
#ifndef EPH_ENABLE_TIMESTAMPS
#define EPH_ENABLE_TIMESTAMPS 0
#endif

#ifndef EPH_NET_ENABLE_TIMESTAMPS_DEFINED
#define EPH_NET_ENABLE_TIMESTAMPS_DEFINED
inline constexpr bool kEnableTimestamps = (EPH_ENABLE_TIMESTAMPS != 0);
#endif

// ---------------------------------------------------------------------------
// DirectTransport — threadless, queueless transport
// ---------------------------------------------------------------------------

/// Threadless WebSocket transport with TLS 1.3 encryption.
///
/// Composes: TransportCore (connection lifecycle), FrameProcessor (RX decode),
/// and ReconnectPolicy (reconnection with exponential backoff).
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

    /// No queue = no last-only delivery (all frames delivered inline).
    static constexpr bool kLastOnlyDeliver = false;

    /// Placeholder for template compatibility with Transport stats API.
    static constexpr size_t QueueDepth = 1;

    // -- DirectDeliver policy: calls on_message directly (no queue) --

    struct DirectDeliver {
        const TransportConfig& config;
        ThreadStats& rx_stats;

        void operator()(const uint8_t* data, uint16_t len, uint8_t opcode) noexcept {
            if (config.on_message) {
                try { config.on_message(data, len, opcode); } catch (...) {}
            }
            rx_stats.packets.fetch_add(1, std::memory_order_relaxed);
            rx_stats.bytes.fetch_add(len, std::memory_order_relaxed);
            if (opcode == ws::opcode::kText) {
                rx_stats.text_packets.fetch_add(1, std::memory_order_relaxed);
                rx_stats.text_bytes.fetch_add(len, std::memory_order_relaxed);
            }
        }
    };

    // -- SendFn type: wraps send_direct_() for FrameProcessor control frames --

    struct DirectSendFn {
        DirectTransport* self;

        SendError operator()(const void* data, size_t len, uint8_t opcode) noexcept {
            return self->send_direct_(data, len, opcode);
        }
    };

    // -- Concrete FrameProcessor type --
    using FP = FrameProcessor<TcpImpl, Framer, DirectDeliver, DirectSendFn,
                              MaxPayload, kLastOnlyDeliver>;

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

        auto t = std::unique_ptr<DirectTransport>(new DirectTransport(config));
        t->core_.tcp_factory = std::move(tcp_factory);
        t->core_.config = config;

        // Full handshake: TCP + TLS + WS upgrade (uses private methods
        // with the proven detail-file logic, not TransportCore::do_connect
        // which has API mismatches with the current http.hpp).
        auto conn_result = t->do_connect_();
        if (!conn_result) {
            SPDLOG_LOGGER_ERROR(log, "Initial connect failed: {}",
                                conn_result.error().message());
            return std::unexpected(conn_result.error());
        }

        // Build FrameProcessor with DirectDeliver + DirectSendFn
        t->init_frame_processor_();

        t->core_.created_at = std::chrono::steady_clock::now();
        t->core_.running.store(true, std::memory_order_release);
        t->core_.notify_state(TransportEvent::kConnected, config.remote_host);

        // Flush any pending TCP ACK accumulated during handshake.
        if constexpr (requires { t->core_.tcp->flush_pending_ack(); }) {
            t->core_.tcp->flush_pending_ack();
        }

        // Hook: allow caller to configure session (e.g., shared RX ring)
        // after handshake completes.
        if (config.on_connected_before_threads) {
            config.on_connected_before_threads();
        }

        t->reconnect_.reset();

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
        // RFC 6455 section 5.6: text frames must contain valid UTF-8
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

    /// Send a string_view as a WebSocket binary frame.
    [[nodiscard]] SendError send_binary(std::string_view sv) noexcept {
        return send(sv.data(), sv.size(), ws::opcode::kBinary);
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

    /// Send a WebSocket Close frame with a custom status code and reason.
    /// Delegates to TransportCore::send_close_direct() (shared with DirectTxTransport).
    SendError send_close(uint16_t status_code,
                         std::string_view reason = {}) noexcept {
        return core_.send_close_direct(status_code, reason, MaxPayload);
    }

    /// Send a WebSocket Ping frame to probe connection liveness.
    /// Delegates to TransportCore::send_ping_direct() (shared with DirectTxTransport).
    SendError send_ping(const void* payload = nullptr,
                        size_t payload_len = 0) noexcept {
        return core_.send_ping_direct(payload, payload_len, MaxPayload);
    }

    /// Batch-send multiple messages (all-or-nothing within reason).
    SendError send_n(const std::span<const uint8_t>* payloads, size_t count,
                     uint8_t opcode = ws::opcode::kBinary) noexcept {
        if (!core_.running.load(std::memory_order_acquire))
            return SendError::kNotConnected;
        if (count > 0 && !payloads) [[unlikely]] return SendError::kNullData;

        for (size_t i = 0; i < count; ++i) {
            if (payloads[i].size() > MaxPayload) return SendError::kMessageTooLarge;
            if (opcode == ws::opcode::kText &&
                !core_.config.skip_utf8_validation &&
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
                core_.ns_per_cycle = npc.value_or(0.0);
            }
            rx.initialized = true;
        }

        if (core_.config.use_tls) {
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
            if constexpr (requires { core_.tcp->last_rx_burst_tsc(); }) {
                core_.current_arrival_tsc = core_.tcp->last_rx_burst_tsc();
            }
        }

        // Plain mode: process framed data directly from WS buffer
        if (!core_.config.use_tls) {
            if (rx.ws_reassembly_len == 0) return;

            size_t ws_consumed = fp_->process(
                rx.ws_reassembly_storage.get(), rx.ws_reassembly_len);

            ws_consumed = std::min(ws_consumed, rx.ws_reassembly_len);
            size_t ws_remaining = rx.ws_reassembly_len - ws_consumed;
            if (ws_remaining > 0 && ws_consumed > 0) {
                std::memmove(rx.ws_reassembly_storage.get(),
                             rx.ws_reassembly_storage.get() + ws_consumed,
                             ws_remaining);
            }
            rx.ws_reassembly_len = ws_remaining;

            if constexpr (requires { core_.tcp->flush_pending_ack(); }) {
                core_.tcp->flush_pending_ack();
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
            bool ok = core_.crypto->dec.decrypt(
                rec_ptr, static_cast<uint16_t>(record_total),
                rx.decrypt_buf.get(), decrypted_len);

            if (!ok) {
                rx_stats_.crypto_errors.fetch_add(1, std::memory_order_relaxed);
                SPDLOG_LOGGER_WARN(log, "process_pending: TLS decrypt failed");
                break;
            }

            if constexpr (kEnableTimestamps) {
                core_.current_decrypt_done_tsc = eph::utils::TSC::now();
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

            size_t ws_consumed = fp_->process(ws_data, ws_data_len);

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

        if constexpr (requires { core_.tcp->flush_pending_ack(); }) {
            core_.tcp->flush_pending_ack();
        }
    }

    /// Self-driven poll: burst from TCP + feed + process in one call.
    /// Convenience for direct mode without Reactor.
    [[nodiscard]] std::expected<uint16_t, std::string> poll() noexcept {
        if (!core_.running.load(std::memory_order_acquire))
            return std::unexpected(std::string("transport not running"));

        auto rx_result = core_.tcp->poll_rx(
            [this](const uint8_t* data, uint16_t len) {
                feed_rx(data, len);
            });

        if (!rx_result) {
            core_.running.store(false, std::memory_order_release);
            return std::unexpected(std::format("TCP rx error: {}", rx_result.error()));
        }

        if (*rx_result == 0) return uint16_t{0};

        process_pending();
        return *rx_result;
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /// Initiate a graceful WebSocket close handshake (RFC 6455 section 7.1.1).
    ///
    /// In direct mode there is no RX thread to wait for the server Close
    /// response, so this sends the Close frame and stops immediately.
    bool close_gracefully(
            uint16_t status_code = ws::close_code::kNormal,
            std::string_view reason = "client shutdown",
            [[maybe_unused]] std::chrono::milliseconds timeout =
                std::chrono::milliseconds{3000}) noexcept {
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

        // kDirect: no RX thread to wait for server Close — stop immediately
        stop();
        return true;
    }

    /// Stop the transport. Sends WebSocket Close frame, closes TCP.
    /// No threads to join (this is DirectTransport).
    void stop() noexcept {
        bool was_running = core_.running.exchange(false, std::memory_order_acq_rel);

        auto log = detail::transport_logger();
        SPDLOG_LOGGER_INFO(log, "Stopping direct transport");

        // Send WebSocket Close frame (no thread race — single thread)
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
        SPDLOG_LOGGER_INFO(log, "Direct transport stopped");
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

    // -----------------------------------------------------------------------
    // Stats & diagnostics
    // -----------------------------------------------------------------------

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
        ws_pings_received_.store(0, std::memory_order_relaxed);
        ws_pongs_sent_.store(0, std::memory_order_relaxed);
        pong_timeouts_.store(0, std::memory_order_relaxed);
        reconnect_count_.store(0, std::memory_order_relaxed);
        rtt_histogram_.reset();
    }

    [[nodiscard]] TransportStats stats() const noexcept {
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - core_.created_at).count();
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
            .decrypt_errors    = rx_stats_.crypto_errors.load(std::memory_order_relaxed),
            .queue_full_count  = 0,  // no queue in direct mode
            .ws_pings_received = ws_pings_received_.load(std::memory_order_relaxed),
            .ws_pongs_sent     = ws_pongs_sent_.load(std::memory_order_relaxed),
            .pong_timeouts     = pong_timeouts_.load(std::memory_order_relaxed),
            .reconnect_count   = reconnect_count_.load(std::memory_order_relaxed),
            .tx_queue_hwm      = 0,  // no TX queue in direct mode
            .rx_queue_hwm      = 0,  // no RX queue in direct mode
            .uptime_ns         = static_cast<uint64_t>(uptime > 0 ? uptime : 0),
            .handshake_ns      = core_.last_handshake_ns,
            .tcp_connect_ns    = core_.last_tcp_connect_ns,
            .tls_handshake_ns  = core_.last_tls_handshake_ns,
            .ws_upgrade_ns     = core_.last_ws_upgrade_ns,
            .remote_ip         = core_.remote_ip,
            .rtt               = rtt_stats(),
            .tx_latency        = histogram_to_stats(tx_latency_histogram_),
            .tx_queue_wait     = histogram_to_stats(tx_queue_wait_histogram_),
            .tx_encode         = histogram_to_stats(tx_encode_histogram_),
            .rx_latency        = histogram_to_stats(rx_latency_histogram_),
            .rx_decrypt        = histogram_to_stats(rx_decrypt_histogram_),
            .rx_decode         = histogram_to_stats(rx_decode_histogram_),
            .tls_write_seq     = core_.crypto ? core_.crypto->write_seq() : 0,
            .tls_read_seq      = core_.crypto ? core_.crypto->read_seq() : 0,
            .tls_seq_limit     = core_.config.use_tls ? tls_record::kMaxSequenceNumber : 0,
        };
    }

private:
    // -----------------------------------------------------------------------
    // Construction (private — use create() factory)
    // -----------------------------------------------------------------------

    explicit DirectTransport(const TransportConfig& config)
        : reconnect_(config)
    {}

    // -----------------------------------------------------------------------
    // FrameProcessor initialization
    // -----------------------------------------------------------------------

    /// Build the FrameProcessor after connection is established.
    /// Called during create() and after each successful reconnect.
    void init_frame_processor_() {
        typename FP::Deps deps{
            .core               = core_,
            .deliver            = DirectDeliver{core_.config, rx_stats_},
            .send_response      = DirectSendFn{this},
            .rx_stats           = rx_stats_,
            .ws_pings_received  = ws_pings_received_,
            .ws_pongs_sent      = ws_pongs_sent_,
            .rtt_histogram      = rtt_histogram_,
            .rx_latency_histogram = rx_latency_histogram_,
            .rx_decrypt_histogram = rx_decrypt_histogram_,
            .rx_decode_histogram  = rx_decode_histogram_,
            .rx_hwm             = rx_hwm_,
            .rx_hwm_counter     = rx_hwm_counter_,
        };
        fp_ = std::make_unique<FP>(std::move(deps));
    }

    // -----------------------------------------------------------------------
    // Connection establishment (private — reused by create() and reconnect)
    // -----------------------------------------------------------------------

    /// Full connection sequence: TCP (via factory) -> [TLS] -> [WS Upgrade] -> [key export].
    /// TLS phases are skipped when config_.use_tls is false (plain ws://).
    /// On success, core_.tcp (and optionally core_.tls, core_.crypto) are populated.
    [[nodiscard]] std::expected<void, ConnectionErrorInfo> do_connect_() {
        auto log = detail::transport_logger();
        auto connect_start = std::chrono::steady_clock::now();

        // Phase 1: Create TCP session via factory
        auto tcp_phase_start = std::chrono::steady_clock::now();
        auto tcp_result = core_.tcp_factory();
        if (!tcp_result) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kFactoryFailed,
                std::format("TCP factory failed: {}", tcp_result.error())});
        }
        core_.tcp = std::move(*tcp_result);
        core_.last_tcp_connect_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - tcp_phase_start).count());

        if (!core_.tcp->is_established()) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kTcpNotEstablished,
                "TCP factory returned non-established session"});
        }

        core_.last_tls_handshake_ns = 0;
        if (core_.config.use_tls) {
            // Phase 2: TLS handshake
            auto tls_phase_start = std::chrono::steady_clock::now();
            TlsConfig tls_cfg{
                .hostname = core_.config.remote_host,
                .ca_cert_path = core_.config.ca_cert_path,
                .verify_peer = core_.config.verify_peer,
                .handshake_timeout = core_.config.tls_timeout,
                .client_cert_path = core_.config.client_cert_path,
                .client_key_path = core_.config.client_key_path,
            };

            auto tls_result = TlsSession<TcpImpl>::create(*core_.tcp, tls_cfg);
            if (!tls_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsSessionFailed,
                    std::format("TLS session failed: {}", tls_result.error())});
            }
            core_.tls = std::make_unique<TlsSession<TcpImpl>>(std::move(*tls_result));

            auto hs_result = core_.tls->handshake();
            if (!hs_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsHandshakeFailed,
                    std::format("TLS handshake failed: {}", hs_result.error())});
            }
            core_.last_tls_handshake_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - tls_phase_start).count());
        }

        // Phase 3: WebSocket upgrade (only for WsFramer)
        if constexpr (kIsWebSocket) {
            auto ws_phase_start = std::chrono::steady_clock::now();
            auto ws_result = do_ws_upgrade_();
            if (!ws_result) {
                return std::unexpected(ws_result.error());
            }
            core_.last_ws_upgrade_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - ws_phase_start).count());
        }

        if (core_.config.use_tls) {
            // Phase 4: Extract keys for AEAD hot path
            auto hot_state = core_.tls->extract_hot_state();
            if (!hot_state) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsKeyExportFailed,
                    std::format("TLS key export failed: {}", hot_state.error())});
            }

            size_t key_len = core_.tls->cipher_key_len();
            auto crypto = TlsRecordCrypto::create(*hot_state, key_len);
            if (!crypto) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsKeyExportFailed,
                    std::format("TLS AEAD init failed: {}", crypto.error())});
            }
            core_.crypto = std::make_unique<TlsRecordCrypto>(std::move(*crypto));

            core_.tls_version = core_.tls->tls_version();
            core_.cipher_name = core_.tls->cipher_name();
        } else {
            core_.tls_version = "none";
            core_.cipher_name = "none";
        }

        // Extract resolved IP if the TCP backend exposes it
        if constexpr (requires { core_.tcp->resolved_ip(); }) {
            core_.remote_ip = std::string(core_.tcp->resolved_ip());
        }

        // Initialize pong timestamp to "now" so pong timeout doesn't fire
        // before the first ping/pong exchange completes.
        if constexpr (kIsWebSocket) {
            core_.last_pong_ns.store(
                std::chrono::steady_clock::now().time_since_epoch().count(),
                std::memory_order_relaxed);
        }

        // Record handshake duration
        auto connect_end = std::chrono::steady_clock::now();
        core_.last_handshake_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                connect_end - connect_start).count());

        SPDLOG_LOGGER_INFO(log,
            "Connected: {} ({}, handshake: {:.1f}ms)",
            core_.config.remote_host,
            core_.config.use_tls
                ? std::format("TLS: {}, cipher: {}", core_.tls_version, core_.cipher_name)
                : std::string("plain WS"),
            static_cast<double>(core_.last_handshake_ns) / 1e6);
        return {};
    }

    /// WebSocket HTTP Upgrade handshake (RFC 6455).
    /// Called from do_connect_() when using WsFramer.
    [[nodiscard]] std::expected<void, ConnectionErrorInfo> do_ws_upgrade_() {
        auto log = detail::transport_logger();

        // Generate WebSocket key
        auto ws_key_result = http::generate_ws_key();
        if (!ws_key_result) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kWsUpgradeFailed, ws_key_result.error()});
        }
        std::string ws_key = std::move(*ws_key_result);

        // Build upgrade request
        // RFC 6455 section 4.1: Host header includes port only when non-default
        std::string host = core_.config.remote_host;
        uint16_t default_port = core_.config.use_tls ? 443 : 80;
        if (core_.config.remote_port != default_port) {
            host += ":" + std::to_string(core_.config.remote_port);
        }

        // Build extra headers including subprotocol if configured
        std::string headers = core_.config.extra_headers;
        if (!core_.config.ws_subprotocol.empty()) {
            headers += std::format("Sec-WebSocket-Protocol: {}\r\n",
                                   core_.config.ws_subprotocol);
        }

        auto request_result = http::build_upgrade_request(
            host, core_.config.ws_path, ws_key, headers);
        if (!request_result) [[unlikely]] {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kWsUpgradeFailed,
                request_result.error()});
        }
        std::string request = std::move(*request_result);

        SPDLOG_LOGGER_DEBUG(log, "Sending WebSocket upgrade request ({})",
            core_.config.use_tls ? "TLS" : "plain TCP");

        // Send upgrade request through TLS or plain TCP
        if (core_.config.use_tls) {
            auto write_result = core_.tls->handshake_write(request.data(),
                                                           static_cast<int>(request.size()));
            if (!write_result || *write_result <= 0) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kWsUpgradeFailed,
                    "Failed to send WebSocket upgrade request"});
            }
        } else {
            auto write_result = core_.tcp->send(request.data(), request.size());
            if (!write_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kWsUpgradeFailed,
                    std::format("Failed to send WebSocket upgrade request: {}",
                                write_result.error())});
            }
        }

        // Read upgrade response (with timeout).
        static constexpr size_t kMaxUpgradeResponseSize = 65536;
        std::vector<uint8_t> response_buf;
        response_buf.reserve(4096);

        auto deadline = std::chrono::steady_clock::now() + core_.config.ws_timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            uint8_t buf[4096];
            int bytes_read = 0;

            if (core_.config.use_tls) {
                auto read_result = core_.tls->handshake_read(buf, sizeof(buf));
                if (!read_result) {
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeFailed,
                        std::format("Failed to read upgrade response: {}",
                                    read_result.error())});
                }
                bytes_read = *read_result;
            } else {
                auto rx_result = core_.tcp->poll_rx(
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

                // Store negotiated subprotocol
                core_.ws_subprotocol = std::move(parsed->sec_ws_protocol);

                SPDLOG_LOGGER_INFO(log, "WebSocket upgrade successful{}",
                    core_.ws_subprotocol.empty() ? ""
                        : std::format(" (subprotocol: {})", core_.ws_subprotocol));
                return {};
            }
        }

        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kWsUpgradeFailed,
            "WebSocket upgrade response timeout"});
    }

    // -----------------------------------------------------------------------
    // Direct send: WS encode -> [TLS encrypt] -> TCP send
    // -----------------------------------------------------------------------

    /// Direct send: frame encode -> [TLS encrypt] -> TCP send.
    /// Called from the application thread; no SPSC queue involved.
    SendError send_direct_(const void* data, size_t len, uint8_t opcode) noexcept {
        if (len > MaxPayload) return SendError::kMessageTooLarge;
        if (!core_.running.load(std::memory_order_acquire))
            return SendError::kNotConnected;

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
    // Members — composed components
    // -----------------------------------------------------------------------

    /// Shared connection state: TCP, TLS, config, lifecycle atomics.
    TransportCore<TcpImpl> core_;

    /// Reconnection with exponential backoff + jitter.
    ReconnectPolicy reconnect_;

    /// Frame decoder/processor — handles WS decode, fragmentation, control
    /// frames, and delivers data frames via DirectDeliver.
    std::unique_ptr<FP> fp_;

    // -----------------------------------------------------------------------
    // Members — stats (single thread, no cross-core contention)
    // -----------------------------------------------------------------------

    /// TX stats (single thread — app thread does all sends).
    ThreadStats tx_stats_{};

    /// RX stats (single thread — app thread does all receives).
    ThreadStats rx_stats_{};

    // -----------------------------------------------------------------------
    // Members — histograms
    // -----------------------------------------------------------------------

    /// RTT histogram (ping -> pong round-trip).
    eph::utils::HdrHistogram rtt_histogram_{
        100, 10'000'000'000ULL, 3
    };

    /// RX latency breakdown histograms.
    eph::utils::HdrHistogram rx_latency_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram rx_decrypt_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram rx_decode_histogram_{
        10, 1'000'000'000ULL, 3
    };

    /// TX histograms (declared for stats() API compatibility — always empty
    /// in direct mode since there is no TX worker).
    eph::utils::HdrHistogram tx_latency_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram tx_queue_wait_histogram_{
        10, 1'000'000'000ULL, 3
    };
    eph::utils::HdrHistogram tx_encode_histogram_{
        10, 1'000'000'000ULL, 3
    };

    // -----------------------------------------------------------------------
    // Members — counters
    // -----------------------------------------------------------------------

    std::atomic<uint64_t> ws_pings_received_{0};
    std::atomic<uint64_t> ws_pongs_sent_{0};
    std::atomic<uint64_t> reconnect_count_{0};
    std::atomic<uint64_t> pong_timeouts_{0};
    std::atomic<size_t>   rx_hwm_{0};
    uint64_t              rx_hwm_counter_{0};

    // -----------------------------------------------------------------------
    // Members — DirectRxState (persistent reassembly buffers for poll)
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
};

} // namespace eph::net
