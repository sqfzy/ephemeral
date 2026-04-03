#pragma once

/// @file socket_connect.hpp
/// Convenience connect functions and type aliases for socket-backed transports.
///
/// Provides socket_wss_connect(), socket_ws_connect(), connect(), and
/// preset-based type aliases (SocketWssTransport, etc.).

#include <expected>
#include <memory>
#include <optional>
#include <type_traits>

#include "eph/net/socket_transport.hpp"
#include "eph/transport/presets.hpp"
#include "eph/transport/transport.hpp"

namespace eph::net {

// ---------------------------------------------------------------------------
// Type aliases — socket backend
// ---------------------------------------------------------------------------
// Naming convention:
//   Socket{Wss|Ws}{Small|Large|Evict}Transport
//   - Wss = WebSocket over TLS (default), Ws = plain WebSocket (use_tls=false)
//   - Small = 64B payload / 256 depth (compact single-symbol)
//   - Large = 4KB payload / 512 depth (JSON market data)
//   - Evict = EvictingQueue RX (drop stale, market data streams)
//   - (none) = 512B payload / 1024 depth (default)

/// @brief Standard WSS (TLS) WebSocket transport using kernel sockets.
///
/// 512-byte max payload, 1024-deep SPSC queues.
using SocketWssTransport = DefaultTransport<SocketTransport>;

/// @brief Small-payload WSS variant (64B messages, single-symbol feeds).
using SocketWssSmallTransport = SmallTransport<SocketTransport>;

/// @brief Large-payload WSS variant (4KB messages, e.g. JSON market data).
using SocketWssLargeTransport = LargeTransport<SocketTransport>;

/// @brief Evicting WSS variant -- drops stale messages when RX queue is full.
///
/// Ideal for market data streams where only the latest update matters.
using SocketWssEvictTransport = EvictTransport<SocketTransport>;

/// @brief Standard plain WS (no TLS) transport using kernel sockets.
///
/// Same as SocketWssTransport but configured with use_tls=false.
/// Use for internal/test services where TLS is not needed.
using SocketWsTransport = DefaultTransport<SocketTransport>;

/// @brief Raw TCP transport (no WebSocket framing).
///
/// For FIX or custom protocols that handle their own message boundaries.
using SocketRawTransport = RawTransport<SocketTransport>;

/// @brief Direct TX mode: app sends directly on caller thread, RX thread delivers via callback/queue.
using SocketDirectTxTransport      = DirectTxDefaultTransport<SocketTransport>;
/// @brief Direct TX small-payload variant (64B, single-symbol).
using SocketDirectTxSmallTransport = DirectTxSmallTransport<SocketTransport>;
/// @brief Direct TX raw variant (no WebSocket framing).
using SocketDirectTxRawTransport   = DirectTxRawTransport<SocketTransport>;

/// @brief Full direct mode: no background threads, app calls send() + poll() directly.
using SocketDirectTransport      = DirectDefaultTransport<SocketTransport>;
/// @brief Full direct small-payload variant (64B, single-symbol).
using SocketDirectSmallTransport = DirectSmallTransport<SocketTransport>;
/// @brief Full direct raw variant (no WebSocket framing).
using SocketDirectRawTransport   = DirectRawTransport<SocketTransport>;

namespace detail {

/// @brief Shared implementation for socket_wss_connect / socket_ws_connect.
///
/// Resolves SocketConfig (using provided or deriving from TransportConfig),
/// validates, builds a TcpFactory, and delegates to Transport::create().
///
/// @tparam MaxPayload  Maximum WebSocket payload size in bytes.
/// @tparam QueueDepth  SPSC queue depth for RX/TX.
/// @param config     Transport configuration (host, port, TLS, WS settings).
/// @param sock_cfg   Optional socket-level config override.
/// @return Connected Transport or ConnectionErrorInfo on failure.
template <size_t MaxPayload, size_t QueueDepth>
[[nodiscard]] inline auto
socket_connect_impl(
    const TransportConfig& config,
    std::optional<SocketConfig> sock_cfg)
    -> std::expected<std::unique_ptr<Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>>,
                     ConnectionErrorInfo>
{
    // Use caller-provided SocketConfig or derive sensible defaults from TransportConfig
    SocketConfig sc = sock_cfg.value_or(SocketConfig{
        .host         = config.remote_host,
        .port         = config.remote_port,
        .tcp_nodelay  = true,
    });

    // Validate SocketConfig early for actionable error messages
    if (auto err = sc.validate(); !err.empty()) {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kInvalidConfig,
            std::format("SocketConfig: {}", err)});
    }

    auto tcp_timeout = config.tcp_timeout;

    auto tcp_factory = [sc, tcp_timeout]()
        -> std::expected<std::unique_ptr<SocketTransport>, std::string> {
        auto tcp = std::make_unique<SocketTransport>(sc);
        auto result = tcp->connect(tcp_timeout);
        if (!result) return std::unexpected(result.error());
        return tcp;
    };

    return Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>::create(
        std::move(tcp_factory), config);
}

} // namespace detail

/// @brief Convenience factory: creates a fully connected SocketWssTransport
/// from just a TransportConfig, eliminating the TcpFactory boilerplate.
///
/// Optionally accepts a SocketConfig for fine-grained TCP tuning;
/// if omitted, sensible defaults are derived from the TransportConfig.
///
/// @tparam MaxPayload  Maximum WebSocket payload size in bytes (default 512).
/// @tparam QueueDepth  SPSC queue depth for RX/TX (default 1024).
/// @param config     Transport configuration (host, port, TLS, WS settings).
/// @param sock_cfg   Optional socket-level config (TCP_NODELAY, keepalive, etc.).
/// @return Connected SocketWssTransport or ConnectionErrorInfo on failure.
template <size_t MaxPayload = 512, size_t QueueDepth = 1024>
[[nodiscard]] inline auto
socket_wss_connect(
    const TransportConfig& config,
    std::optional<SocketConfig> sock_cfg = std::nullopt)
    -> std::expected<std::unique_ptr<Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>>,
                     ConnectionErrorInfo>
{
    return detail::socket_connect_impl<MaxPayload, QueueDepth>(config, std::move(sock_cfg));
}

/// @brief Convenience factory for plain WebSocket (ws://) connections.
///
/// Creates a Transport with use_tls=false.
///
/// @tparam MaxPayload  Maximum WebSocket payload size in bytes (default 512).
/// @tparam QueueDepth  SPSC queue depth for RX/TX (default 1024).
/// @param config     Transport configuration (host, port, WS settings).
/// @param sock_cfg   Optional socket-level config (TCP_NODELAY, keepalive, etc.).
/// @return Connected plain WS Transport or ConnectionErrorInfo on failure.
template <size_t MaxPayload = 512, size_t QueueDepth = 1024>
[[nodiscard]] inline auto
socket_ws_connect(
    TransportConfig config,
    std::optional<SocketConfig> sock_cfg = std::nullopt)
    -> std::expected<std::unique_ptr<Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>>,
                     ConnectionErrorInfo>
{
    config.use_tls = false;
    return detail::socket_connect_impl<MaxPayload, QueueDepth>(config, std::move(sock_cfg));
}

/// @brief Create a socket-based WebSocket transport from a URL string.
///
/// Combines TransportConfig::from_url() with the appropriate connect
/// function (wss or ws based on scheme), eliminating boilerplate.
///
/// Usage:
///   // Minimal:
///   auto t = eph::net::connect("wss://example.com/ws");
///
///   // With config customization:
///   auto t = eph::net::connect("wss://example.com/ws", [](auto& cfg) {
///       cfg.max_reconnect_attempts = 5;
///       cfg.on_message = [](auto* data, uint16_t len, uint8_t) { ... };
///   });
///
/// @tparam MaxPayload      Maximum WebSocket payload size in bytes (default 512).
/// @tparam QueueDepth      SPSC queue depth for RX/TX (default 1024).
/// @tparam ConfigModifier  Callable type for customizing TransportConfig (auto-deduced).
/// @param url       WebSocket URL (ws:// or wss://).
/// @param modifier  Optional callback to customize TransportConfig before connecting.
/// @param sock_cfg  Optional socket-level config (TCP_NODELAY, keepalive, etc.).
/// @return Connected transport, or ConnectionErrorInfo on failure.
template <size_t MaxPayload = 512, size_t QueueDepth = 1024,
          typename ConfigModifier = std::nullptr_t>
[[nodiscard]] inline auto
connect(std::string_view url,
        ConfigModifier modifier = nullptr,
        std::optional<SocketConfig> sock_cfg = std::nullopt)
    -> std::expected<std::unique_ptr<Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>>,
                     ConnectionErrorInfo>
{
    auto cfg_result = TransportConfig::from_url(url);
    if (!cfg_result) {
        return std::unexpected(ConnectionErrorInfo{
            ConnectionError::kInvalidConfig,
            std::format("Invalid URL: {}", cfg_result.error())});
    }

    auto cfg = std::move(*cfg_result);

    // Apply user customizations if provided
    if constexpr (!std::is_null_pointer_v<ConfigModifier>) {
        modifier(cfg);
    }

    if (cfg.use_tls) {
        return socket_wss_connect<MaxPayload, QueueDepth>(cfg, sock_cfg);
    } else {
        return socket_ws_connect<MaxPayload, QueueDepth>(cfg, sock_cfg);
    }
}

} // namespace eph::net
