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

#include "eph/core/tcp_concept.hpp"
#include "eph/core/framer_concept.hpp"
#include "eph/transport/detail/message_types.hpp"
#include "eph/transport/http.hpp"
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
    using TcpFactory = std::function<
        std::expected<std::unique_ptr<TcpImpl>, std::string>()>;

    // -- Connection objects --
    TcpFactory tcp_factory;
    std::unique_ptr<TcpImpl> tcp;
    std::unique_ptr<TlsSession<TcpImpl>> tls;  // only during handshake
    std::unique_ptr<TlsRecordCrypto> crypto;
    TransportConfig config;

    // -- Lifecycle flags (multi-thread access) --
    std::atomic<bool> running{false};
    std::atomic<bool> reconnecting{false};
    std::atomic<bool> closing{false};
    std::atomic<bool> force_reconnect{false};
    std::atomic<bool> close_requested{false};

    // -- Connection metadata (written during handshake, read-only after) --
    std::string tls_version{"none"};
    std::string cipher_name{"none"};
    std::string ws_subprotocol;
    std::string remote_ip;
    uint64_t last_handshake_ns{0};
    uint64_t last_tcp_connect_ns{0};
    uint64_t last_tls_handshake_ns{0};
    uint64_t last_ws_upgrade_ns{0};
    std::chrono::steady_clock::time_point created_at;

    // -- Close handshake state --
    uint16_t pending_close_code{ws::close_code::kNormal};
    std::string pending_close_reason;

    // -- Pong tracking (TX writes ping time, RX writes pong time) --
    std::atomic<int64_t> last_pong_ns{0};
    std::atomic<uint64_t> last_ping_tsc{0};

    // -- TSC conversion (cached at RX loop entry) --
    double ns_per_cycle{0.0};
    uint64_t current_arrival_tsc{0};
    uint64_t current_decrypt_done_tsc{0};

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

        auto log = detail::transport_logger();
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

    /// Notify state change callbacks safely (catches exceptions).
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
