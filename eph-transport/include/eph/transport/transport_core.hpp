#pragma once

/// @file transport_core.hpp
/// Shared connection state owned by all Transport variants.
///
/// TransportCore holds the TCP connection, TLS crypto state, config,
/// lifecycle atomics, and connection metadata. It does NOT own threads,
/// queues, or per-thread stats — those belong to TxWorker/RxWorker.
///
/// Workers receive a TransportCore& reference and operate on its fields.
/// The owning Transport class coordinates lifecycle (reconnect, stop).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

#include <cstring>

#include "eph/core/tcp_concept.hpp"
#include "eph/core/framer_concept.hpp"
#include "eph/transport/detail/message_types.hpp"
#include "eph/transport/http.hpp"
#include "eph/transport/tls_encryptor.hpp"
#include "eph/transport/tls_record.hpp"
#include "eph/transport/tls_session.hpp"
#include "eph/transport/transport_types.hpp"
#include "eph/transport/websocket.hpp"
#include "eph/transport/ws_framer.hpp"

namespace eph::net {

/// Shared connection state for all Transport variants.
///
/// This is a struct (not a class) because Workers need direct access
/// to its fields. Encapsulation is enforced at the Transport level,
/// not here — TransportCore is an internal implementation detail.
template <TcpTransport TcpImpl>
struct TransportCore {
    /// Factory callable: creates a new, already-connected TcpImpl.
    /// Called during initial create() and on each reconnection attempt.
    using TcpFactory = std::function<
        std::expected<std::unique_ptr<TcpImpl>, std::string>()>;

    // -- Connection objects --
    TcpFactory tcp_factory;                              ///< Creates fresh TCP connections
    std::unique_ptr<TcpImpl> tcp;                        ///< Active TCP connection
    std::unique_ptr<TlsSession<TcpImpl>> tls;            ///< TLS session (alive only during handshake for key export)
    std::unique_ptr<TlsRecordCrypto> crypto;             ///< Hot-path AEAD encryptor + decryptor
    TransportConfig config;                              ///< User-supplied configuration (copied at create time)

    // -- Lifecycle flags (accessed from multiple threads via atomics) --
    std::atomic<bool> running{false};                    ///< True while the transport is operational
    std::atomic<bool> reconnecting{false};               ///< True during reconnection (workers must pause)
    std::atomic<bool> closing{false};                    ///< True after receiving a WS Close frame
    std::atomic<bool> force_reconnect{false};            ///< Set by application to force a reconnect
    std::atomic<bool> close_requested{false};            ///< Set by application to initiate graceful close

    // -- Connection metadata (written during handshake, read-only after) --
    std::string tls_version{"none"};                     ///< Negotiated TLS version string (e.g., "TLSv1.3")
    std::string cipher_name{"none"};                     ///< Negotiated cipher suite name
    std::string ws_subprotocol;                          ///< Negotiated WebSocket subprotocol
    std::string remote_ip;                               ///< Resolved remote IP address
    uint64_t last_handshake_ns{0};                       ///< Total handshake duration (TCP + TLS + WS) in nanoseconds
    uint64_t last_tcp_connect_ns{0};                     ///< TCP connect (factory call) duration in nanoseconds
    uint64_t last_tls_handshake_ns{0};                   ///< TLS handshake duration in nanoseconds (0 if no TLS)
    uint64_t last_ws_upgrade_ns{0};                      ///< WebSocket Upgrade duration in nanoseconds
    std::chrono::steady_clock::time_point created_at;    ///< Timestamp when the transport was created

    // -- Close handshake state --
    uint16_t pending_close_code{ws::close_code::kNormal}; ///< Close status code for the pending close frame
    std::string pending_close_reason;                      ///< Close reason string for the pending close frame

    // -- Pong tracking (TX thread writes ping time, RX thread writes pong time) --
    std::atomic<int64_t> last_pong_ns{0};                ///< Steady-clock nanoseconds of last pong received
    std::atomic<uint64_t> last_ping_tsc{0};              ///< TSC timestamp of last ping sent (for RTT measurement)

    // -- TSC conversion state (cached at RX loop entry, used by FrameProcessor) --
    double ns_per_cycle{0.0};                            ///< TSC-to-nanosecond conversion factor
    uint64_t current_arrival_tsc{0};                     ///< TSC at TCP segment arrival (set by RX loop)
    uint64_t current_decrypt_done_tsc{0};                ///< TSC after TLS decryption completes

    // -----------------------------------------------------------------------
    // Connection establishment (control plane)
    // -----------------------------------------------------------------------

    /// Full handshake: TCP connect → TLS 1.3 → key export.
    /// Called during initial create() and on each reconnect.
    [[nodiscard]] std::expected<void, ConnectionErrorInfo> do_connect() noexcept {
        auto log = detail::transport_logger();

        // Phase 1: TCP connect
        auto tcp_start = std::chrono::steady_clock::now();
        auto tcp_result = tcp_factory();
        if (!tcp_result) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kFactoryFailed, tcp_result.error()});
        }
        tcp = std::move(*tcp_result);
        auto tcp_elapsed = std::chrono::steady_clock::now() - tcp_start;
        last_tcp_connect_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tcp_elapsed).count());

        if (!tcp->is_established()) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kTcpNotEstablished,
                "TCP connection not established after factory"});
        }

        // Extract remote IP if available
        if constexpr (requires { tcp->resolved_ip(); }) {
            remote_ip = tcp->resolved_ip();
        }

        SPDLOG_LOGGER_DEBUG(log, "TCP connected in {:.2f}ms",
            last_tcp_connect_ns / 1e6);

        // Phase 2: TLS handshake (if enabled)
        if (config.use_tls) {
            auto tls_start = std::chrono::steady_clock::now();

            TlsConfig tls_cfg{
                .hostname        = config.remote_host,
                .ca_cert_path    = config.ca_cert_path,
                .verify_peer     = config.verify_peer,
                .client_cert_path = config.client_cert_path,
                .client_key_path  = config.client_key_path,
            };

            auto tls_result = TlsSession<TcpImpl>::create(*tcp, tls_cfg);
            if (!tls_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsSessionFailed, tls_result.error()});
            }
            tls = std::make_unique<TlsSession<TcpImpl>>(std::move(*tls_result));

            auto hs_result = tls->handshake();
            if (!hs_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsHandshakeFailed, hs_result.error()});
            }

            auto tls_elapsed = std::chrono::steady_clock::now() - tls_start;
            last_tls_handshake_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(tls_elapsed).count());

            // Extract traffic keys for hot-path AEAD
            auto hot = tls->extract_hot_state();
            if (!hot) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsKeyExportFailed, hot.error()});
            }

            size_t key_len = tls->cipher_key_len();
            auto crypto_result = TlsRecordCrypto::create(*hot, key_len);
            if (!crypto_result) {
                return std::unexpected(ConnectionErrorInfo{
                    ConnectionError::kTlsKeyExportFailed,
                    std::format("TLS AEAD init failed: {}", crypto_result.error())});
            }
            crypto = std::make_unique<TlsRecordCrypto>(std::move(*crypto_result));

            tls_version = tls->tls_version();
            cipher_name = tls->cipher_name();

            SPDLOG_LOGGER_DEBUG(log, "TLS handshake in {:.2f}ms: {} / {}",
                last_tls_handshake_ns / 1e6, tls_version, cipher_name);
        }

        // Initialize pong tracking
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        last_pong_ns.store(now_ns, std::memory_order_relaxed);

        last_handshake_ns = last_tcp_connect_ns + last_tls_handshake_ns;

        SPDLOG_LOGGER_INFO(log, "Connection established: {}:{}",
            config.remote_host, config.remote_port);
        return {};
    }

    /// WebSocket HTTP Upgrade handshake (RFC 6455).
    /// Must be called after do_connect() when using WebSocket framing.
    template <MessageFramer Framer>
    [[nodiscard]] std::expected<void, ConnectionErrorInfo> do_ws_upgrade() noexcept {
        if constexpr (!std::is_same_v<Framer, WsFramer>) {
            // Non-WebSocket framers skip the upgrade
            return {};
        }

        [[maybe_unused]] auto log = detail::transport_logger();
        auto ws_start = std::chrono::steady_clock::now();

        // Build upgrade request
        auto ws_key_result = http::generate_ws_key();
        if (!ws_key_result) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kWsUpgradeFailed, ws_key_result.error()});
        }
        std::string ws_key = std::move(*ws_key_result);
        std::string path = config.ws_path.empty() ? "/" : config.ws_path;

        // Build Host header with port (RFC 6455 §4.1)
        std::string host = config.remote_host;
        uint16_t default_port = config.use_tls ? 443 : 80;
        if (config.remote_port != default_port) {
            host += ":" + std::to_string(config.remote_port);
        }
        std::string headers = config.extra_headers;
        if (!config.ws_subprotocol.empty()) {
            headers += std::format("Sec-WebSocket-Protocol: {}\r\n",
                                   config.ws_subprotocol);
        }
        auto request_result = http::build_upgrade_request(
            host, path, ws_key, headers);
        if (!request_result) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kWsUpgradeFailed, request_result.error()});
        }
        auto request = std::move(*request_result);

        SPDLOG_LOGGER_DEBUG(log, "WS Upgrade: {} bytes to {}:{}{}",
            request.size(), config.remote_host, config.remote_port, path);

        // Send upgrade request
        std::expected<size_t, std::string> send_result;
        if (config.use_tls && tls) {
            send_result = tls->handshake_write(
                reinterpret_cast<const uint8_t*>(request.data()), request.size());
        } else {
            send_result = tcp->send(
                reinterpret_cast<const uint8_t*>(request.data()), request.size());
        }
        if (!send_result) {
            return std::unexpected(ConnectionErrorInfo{
                ConnectionError::kWsUpgradeFailed,
                std::format("WS upgrade send failed: {}", send_result.error())});
        }

        // Read upgrade response
        std::string response_buf;
        response_buf.reserve(4096);
        uint8_t chunk[4096];
        auto deadline = std::chrono::steady_clock::now() + config.ws_timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            std::expected<size_t, std::string> read_result;
            if (config.use_tls && tls) {
                read_result = tls->handshake_read(chunk, sizeof(chunk));
            } else {
                uint16_t n = 0;
                auto poll_result = tcp->poll_rx(
                    [&](const uint8_t* data, uint16_t len) {
                        std::memcpy(chunk, data, len);
                        n = len;
                    });
                if (poll_result) {
                    read_result = static_cast<size_t>(n);
                } else {
                    read_result = std::unexpected(poll_result.error());
                }
            }

            if (!read_result || *read_result == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                continue;
            }

            response_buf.append(reinterpret_cast<const char*>(chunk), *read_result);

            // Check for end of HTTP headers
            if (response_buf.find("\r\n\r\n") != std::string::npos) {
                auto parsed = http::parse_upgrade_response(
                    response_buf.data(), response_buf.size());
                if (!parsed) {
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsUpgradeRejected,
                        std::format("WS upgrade parse failed: {}", parsed.error())});
                }

                // Validate Sec-WebSocket-Accept
                if (!http::validate_ws_accept(ws_key, parsed->sec_ws_accept)) {
                    return std::unexpected(ConnectionErrorInfo{
                        ConnectionError::kWsAcceptInvalid,
                        "Sec-WebSocket-Accept mismatch"});
                }

                ws_subprotocol = parsed->sec_ws_protocol;

                auto ws_elapsed = std::chrono::steady_clock::now() - ws_start;
                last_ws_upgrade_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        ws_elapsed).count());
                last_handshake_ns += last_ws_upgrade_ns;

                SPDLOG_LOGGER_DEBUG(log, "WS upgrade in {:.2f}ms, subprotocol: '{}'",
                    last_ws_upgrade_ns / 1e6, ws_subprotocol);
                return {};
            }
        }

        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kWsUpgradeFailed,
            std::format("WS upgrade timeout after {}ms",
                config.ws_timeout.count())});
    }

    // -----------------------------------------------------------------------
    // Direct-send helpers (shared by DirectTransport & DirectTxTransport)
    //
    // These encapsulate the "build WS control frame → TLS encrypt → TCP send"
    // pattern, eliminating code duplication across the two Direct variants.
    // Transport (threaded) has its own versions that enqueue to TxWorker.
    // -----------------------------------------------------------------------

    /// Send a pre-encoded frame directly: TLS encrypt (if enabled) → TCP send.
    ///
    /// @param frame      Encoded WS frame bytes
    /// @param frame_len  Length of the encoded frame
    /// @return SendError::kOk on success
    SendError send_raw_direct(const uint8_t* frame, size_t frame_len) noexcept {
        if (!running.load(std::memory_order_acquire))
            return SendError::kNotConnected;

        if (config.use_tls) {
            if (!crypto) return SendError::kNotConnected;
            // Max control frame: 14-byte WS header + 125-byte payload
            uint8_t tls_buf[TlsEncryptor::encrypted_size(
                ws::kMaxFrameHeaderLen + 125)];
            uint16_t enc_len = crypto->enc.encrypt(
                frame, static_cast<uint16_t>(frame_len), tls_buf);
            if (enc_len == 0) return SendError::kEncryptFailed;
            auto r = tcp->send(tls_buf, enc_len);
            if (!r) return SendError::kTcpSendFailed;
        } else {
            auto r = tcp->send(frame, frame_len);
            if (!r) return SendError::kTcpSendFailed;
        }
        return SendError::kOk;
    }

    /// Send a WebSocket Close frame directly (no queue).
    ///
    /// Validates the close code and reason, builds the WS close frame,
    /// then delegates to send_raw_direct() for encrypt + TCP send.
    ///
    /// @param status_code  Close reason code (e.g., ws::close_code::kNormal)
    /// @param reason       Optional reason string (max 123 bytes, truncated)
    /// @param max_payload  MaxPayload of the owning transport (for size check)
    /// @return SendError::kOk on success
    SendError send_close_direct(uint16_t status_code,
                                std::string_view reason,
                                size_t max_payload) noexcept {
        if (!running.load(std::memory_order_acquire))
            return SendError::kNotConnected;
        if (!ws::is_valid_close_code(status_code))
            return SendError::kInvalidCloseCode;
        // RFC 6455 §7.1.6: close reason must be valid UTF-8
        if (!reason.empty() && !ws::is_valid_utf8(reason))
            return SendError::kInvalidUtf8;

        size_t reason_len = std::min(reason.size(), size_t{123});
        if (reason_len < reason.size()) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Close reason truncated from {} to 123 bytes "
                "(RFC 6455 §5.5 limit)", reason.size());
        }
        uint16_t payload_len = static_cast<uint16_t>(2 + reason_len);
        if (payload_len > max_payload) return SendError::kMessageTooLarge;

        uint8_t close_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
        size_t close_len = ws::build_close_frame(
            close_buf, status_code, reason);

        auto err = send_raw_direct(close_buf, close_len);
        if (err != SendError::kOk) {
            SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                "send_close_direct: failed for status_code={}: {}",
                status_code, static_cast<int>(err));
        }
        return err;
    }

    /// Send a WebSocket Ping frame directly (no queue).
    ///
    /// Validates and truncates payload per RFC 6455 §5.5, builds the
    /// WS ping frame, then delegates to send_raw_direct().
    ///
    /// @param payload      Optional ping payload (nullptr for empty)
    /// @param payload_len  Payload length (truncated to 125 if larger)
    /// @param max_payload  MaxPayload of the owning transport (for size check)
    /// @return SendError::kOk on success
    SendError send_ping_direct(const void* payload,
                               size_t payload_len,
                               size_t max_payload) noexcept {
        if (!running.load(std::memory_order_acquire))
            return SendError::kNotConnected;

        // RFC 6455 §5.5: control frame payload MUST NOT exceed 125 bytes
        size_t original_len = payload_len;
        payload_len = std::min(payload_len, size_t{125});
        if (original_len > 125) {
            SPDLOG_LOGGER_WARN(detail::transport_logger(),
                "Ping payload truncated from {} to 125 bytes "
                "(RFC 6455 §5.5 limit)", original_len);
        }
        if (payload_len > max_payload) return SendError::kMessageTooLarge;

        uint8_t ping_buf[ws::kMaxFrameHeaderLen + 125 + 1]{};
        size_t ping_len = ws::build_ping_frame(
            ping_buf, payload, payload_len);

        auto err = send_raw_direct(ping_buf, ping_len);
        if (err != SendError::kOk) {
            SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                "send_ping_direct: failed, payload_len={}: {}",
                payload_len, static_cast<int>(err));
        }
        return err;
    }

    // -----------------------------------------------------------------------
    // State change notification
    // -----------------------------------------------------------------------

    /// Notify state change callbacks safely (catches and logs exceptions).
    ///
    /// @param event   The lifecycle event to report
    /// @param detail  Context string (e.g., error message, attempt count)
    void notify_state(TransportEvent event,
                      std::string_view detail = {}) noexcept {
        if (config.on_state_change) {
            try {
                config.on_state_change(event, detail);
            } catch (const std::exception& e) {
                SPDLOG_LOGGER_ERROR(detail::transport_logger(),
                    "on_state_change callback threw: {}", e.what());
            }
        }

        auto log = detail::transport_logger();
        switch (event) {
        case TransportEvent::kConnected:
            SPDLOG_LOGGER_INFO(log, "State: Connected ({})", detail);
            break;
        case TransportEvent::kDisconnected:
            SPDLOG_LOGGER_WARN(log, "State: Disconnected ({})", detail);
            break;
        case TransportEvent::kReconnecting:
            SPDLOG_LOGGER_INFO(log, "State: Reconnecting ({})", detail);
            break;
        case TransportEvent::kStopped:
            SPDLOG_LOGGER_INFO(log, "State: Stopped");
            break;
        }
    }
};

} // namespace eph::net
