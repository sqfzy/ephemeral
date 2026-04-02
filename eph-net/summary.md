# eph-net Summary

## 1. Overview

eph-net is the network transport layer of the eph HFT ecosystem. It provides
a header-only C++23 library (~4000 lines) that bridges the protocol-agnostic
`Transport<>` engine (from eph-transport) with the Linux kernel TCP stack via
POSIX sockets. The library covers the full connectivity stack for crypto
exchange clients: TCP transport, HTTP REST, HMAC signing, and operational
infrastructure (rate limiting, circuit breaking, proxy tunneling, emergency
shutdown).

The core design separates the control plane from the data plane. Connection
establishment (DNS resolution, TCP handshake, socket option configuration)
executes synchronously during setup. Once connected, the `SocketTransport`
satisfies the `TcpTransport` concept, enabling it to be plugged into any
`Transport<SocketTransport, Framer, ...>` combination where the transport
engine handles threading, framing, and TLS.

Infrastructure components are designed for exchange-specific operational
requirements: rate limiters prevent IP bans from API overuse, circuit breakers
back off from broken endpoints, the gateway manages multi-connection lifecycles,
and the kill switch provides coordinated emergency shutdown with signal handling.

All components use spdlog for leveled logging with compile-time filtering via
`SPDLOG_ACTIVE_LEVEL`. The library is built as a headeronly xmake target.

## 2. Architecture

```
+-----------------------------------------------------------+
|                   Application Layer                       |
|  connect("wss://...")  HttpClient::get()  KillSwitch      |
+------+------------------+------------------+--------------+
       |                  |                  |
       v                  v                  v
+------+------+   +-------+-------+  +------+--------+
| Transport<> |   | HttpClient    |  | Infrastructure |
| (eph-trans) |   | (http_client) |  | CB / RL / GW   |
+------+------+   +-------+-------+  +------+--------+
       |                  |                  |
       v                  v                  |
+------+------------------+---------+        |
|         SocketTransport           |        |
|  (socket_config + socket_connect) |<-------+
+------+----------------------------+  (proxy tunneling)
       |
       v
+------+------+
| POSIX TCP   |
| (poll, send |
|  recv, etc) |
+-------------+
```

## 3. Module Map

| File | Responsibility | Key Types | Depends On |
|------|----------------|-----------|------------|
| `net.hpp` | Convenience umbrella header | -- | socket_config, socket_transport, socket_connect, eph-transport, eph-core |
| `socket_config.hpp` | TCP socket configuration and URL parsing | `SocketConfig`, `kEnableSocketTimestamps` | eph-core (json_escape), spdlog |
| `socket_transport.hpp` | POSIX non-blocking TCP transport | `SocketTransport` | socket_config, eph-core (tcp_concept), eph-transport (transport_types), eph-utils (HdrHistogram, TSC), spdlog |
| `socket_connect.hpp` | Connect factories and type aliases | `SocketWssTransport`, `connect()`, `socket_wss_connect()` | socket_transport, eph-transport (presets, transport) |
| `http_client.hpp` | Synchronous HTTP/1.1 REST client | `HttpClient`, `HttpResponse`, `build_http_request()`, `parse_http_response()`, `find_header()` | aws-lc (SSL), spdlog |
| `hmac.hpp` | HMAC-SHA256 signing and encoding | `hmac_sha256()`, `hmac_sha256_hex()`, `to_hex()`, `to_base64()`, `hmac_verify()` | aws-lc (HMAC, EVP), spdlog |
| `circuit_breaker.hpp` | Three-state circuit breaker | `CircuitBreaker`, `CircuitState` | spdlog |
| `rate_limiter.hpp` | Token bucket rate limiter | `RateLimiter` | spdlog |
| `gateway.hpp` | Multi-connection lifecycle manager | `Gateway`, `GatewayConnection`, `ConnHealth` | spdlog |
| `proxy.hpp` | SOCKS5/HTTP CONNECT proxy tunneling | `ProxyConfig`, `ProxyType`, `socks5_handshake()`, `http_connect_handshake()`, `make_proxied_factory()`, `parse_proxy_url()` | socket_transport, spdlog |
| `kill_switch.hpp` | Emergency shutdown coordinator | `KillSwitch`, `TransportHandle`, `kKillSwitchMaxTransports` | spdlog |

## 4. Data Flow

### WebSocket Connection (via connect())

```
connect("wss://stream.binance.com:9443/ws/btcusdt")
  |
  v
TransportConfig::from_url(url)
  |
  v
SocketConfig{host, port, tcp_nodelay=true}
  |
  v
SocketTransport::connect(timeout)
  |  DNS resolve (std::async + wait_for)
  |  socket() + setsockopt (NODELAY, keepalive, ...)
  |  non-blocking connect() + poll()
  v
Transport<SocketTransport, WsFramer>::create(factory, cfg)
  |  TLS handshake (aws-lc)
  |  WebSocket HTTP Upgrade
  |  Start TX/RX threads
  v
[Connected — app calls send_text() / recv()]
```

### REST API Call (HttpClient)

```
HttpClient::get("/api/v3/ticker")
  |
  v
build_http_request("GET", host, path)
  |
  v
tcp_connect()  -->  DNS + poll() + connect()
  |
  v
tls_handshake()  -->  SSL_CTX_new + SSL_connect
  |
  v
ssl_send_all(request)
  |
  v
ssl_recv_all()  -->  poll() + SSL_read loop
  |
  v
parse_http_response(raw_data)
  |
  v
HttpResponse{status_code, headers_raw, body}
```

## 5. Key Components

### SocketTransport (socket_transport.hpp)

POSIX non-blocking TCP transport satisfying the `TcpTransport` concept.

```cpp
class SocketTransport {
    explicit SocketTransport(const SocketConfig& config) noexcept;
    std::expected<void, std::string>
        connect(std::chrono::milliseconds timeout = 3000ms);
    std::expected<size_t, std::string>
        send(const void* data, size_t len);
    template <typename F>
    std::expected<uint16_t, std::string> poll_rx(F&& callback);
    template <typename F, typename Rep, typename Period>
    std::expected<uint16_t, std::string>
        poll_rx_for(F&& callback, duration<Rep, Period> timeout);
    std::expected<void, std::string> close();
    void reset() noexcept;
    TcpState state() const noexcept;
    uint16_t mss() const noexcept;
};
```

### connect() (socket_connect.hpp)

One-liner URL-based connection factory.

```cpp
template <size_t MaxPayload = 512, size_t QueueDepth = 1024,
          typename ConfigModifier = std::nullptr_t>
auto connect(std::string_view url,
             ConfigModifier modifier = nullptr,
             std::optional<SocketConfig> sock_cfg = std::nullopt)
    -> std::expected<std::unique_ptr<Transport<...>>, ConnectionErrorInfo>;
```

### HttpClient (http_client.hpp)

Synchronous HTTP/1.1 client with TLS support.

```cpp
class HttpClient {
    explicit HttpClient(Config config);
    std::expected<HttpResponse, std::string>
        get(std::string_view path, std::string_view extra_headers = {});
    std::expected<HttpResponse, std::string>
        post(std::string_view path, std::string_view body,
             std::string_view content_type = "application/json",
             std::string_view extra_headers = {});
};
```

### hmac_sha256_hex() (hmac.hpp)

HMAC-SHA256 with hex output for exchange API signing.

```cpp
std::expected<std::string, std::string>
hmac_sha256_hex(std::string_view key, std::string_view message) noexcept;
```

### CircuitBreaker (circuit_breaker.hpp)

Thread-safe three-state circuit breaker.

```cpp
class CircuitBreaker {
    explicit CircuitBreaker(Config config = Config{}) noexcept;
    bool allow() noexcept;
    void record_success() noexcept;
    void record_failure() noexcept;
    CircuitState state() const noexcept;
};
```

### RateLimiter (rate_limiter.hpp)

Token bucket rate limiter with nanosecond-precision refill.

```cpp
class RateLimiter {
    explicit RateLimiter(double rate_per_sec, std::size_t burst) noexcept;
    bool try_acquire(std::size_t n = 1) noexcept;
    void acquire(std::size_t n = 1) noexcept;
};
```

### Gateway (gateway.hpp)

Multi-connection lifecycle manager with health monitoring.

```cpp
class Gateway {
    template <typename Transport>
    size_t add(std::string tag, Transport* tp, uint8_t priority = 128);
    void start_all() noexcept;
    void stop_all() noexcept;
    void start_monitor() noexcept;
    void check_health() noexcept;
};
```

### KillSwitch (kill_switch.hpp)

Emergency shutdown coordinator with signal handling.

```cpp
class KillSwitch {
    template <typename Transport>
    bool register_transport(Transport* tp) noexcept;
    bool is_shutdown_requested() const noexcept;
    void shutdown() noexcept;
    void kill() noexcept;
    void install_signal_handlers() noexcept;
};
```

### socks5_handshake() (proxy.hpp)

RFC 1928 SOCKS5 proxy tunnel establishment.

```cpp
std::expected<void, std::string>
socks5_handshake(SocketTransport& tcp,
                 std::string_view target_host, uint16_t target_port,
                 const ProxyConfig& cfg);
```

## 6. Entry Points & APIs

| Entry Point | Type | Description |
|-------------|------|-------------|
| `eph::net::connect(url, modifier?, sock_cfg?)` | Factory function | One-liner WSS/WS connection from URL |
| `eph::net::socket_wss_connect(config, sock_cfg?)` | Factory function | WSS connection from TransportConfig |
| `eph::net::socket_ws_connect(config, sock_cfg?)` | Factory function | Plain WS connection from TransportConfig |
| `eph::net::SocketWssTransport` | Type alias | `Transport<SocketTransport, WsFramer, 512, 1024>` |
| `eph::net::SocketWssLargeTransport` | Type alias | `Transport<SocketTransport, WsFramer, 4096, 512>` |
| `eph::net::SocketDirectTransport` | Type alias | Full direct mode (no background threads) |
| `eph::net::HttpClient` | Class | Synchronous HTTP/1.1 REST client |
| `eph::net::hmac_sha256_hex()` | Free function | HMAC-SHA256 hex signing |
| `eph::net::CircuitBreaker` | Class | Three-state endpoint protection |
| `eph::net::RateLimiter` | Class | Token bucket rate limiter |
| `eph::net::Gateway` | Class | Multi-connection manager |
| `eph::net::KillSwitch` | Class | Emergency shutdown coordinator |
| `eph::net::proxy::make_proxied_factory()` | Free function | Proxy-tunneled TcpFactory builder |
| `eph::net::proxy::parse_proxy_url()` | Free function | Proxy URL parser |

## 7. Dependencies

### Internal Module Graph

```
net.hpp (convenience header)
  +---> socket_config.hpp -----> eph-core (json_escape)
  +---> socket_transport.hpp --> eph-core (tcp_concept)
  |                          --> eph-transport (transport_types)
  |                          --> eph-utils (HdrHistogram, TSC)
  +---> socket_connect.hpp ---> eph-transport (presets, transport)

http_client.hpp --> aws-lc (SSL API)
hmac.hpp ---------> aws-lc (HMAC, EVP)
proxy.hpp --------> socket_transport.hpp

circuit_breaker.hpp  (standalone, spdlog only)
rate_limiter.hpp     (standalone, spdlog only)
gateway.hpp          (standalone, spdlog only)
kill_switch.hpp      (standalone, spdlog only)
```

### External Packages

| Package | Purpose | Used By |
|---------|---------|---------|
| aws-lc | TLS handshake (SSL API), HMAC-SHA256, base64 (EVP) | http_client, hmac |
| spdlog | Leveled logging with compile-time filtering | All modules |
| eph-core | TcpTransport concept, framer concepts, json_escape | socket_transport, socket_config, net.hpp |
| eph-transport | Transport engine, WsFramer, presets, transport types | socket_connect, net.hpp |
| eph-utils | HdrHistogram (latency stats), TSC timing | socket_transport |
| eph-containers | SPSC bounded queue (used by Transport engine) | Indirect via eph-transport |

## 8. Testing

The eph-net subproject does not currently have its own `tests/` directory.
Testing is covered at higher integration levels:

- **eph-core tests** cover the `TcpTransport` concept, `TcpState` enum, and
  framer interfaces that SocketTransport implements.
- **eph-transport tests** cover the Transport engine, WebSocket framing,
  transport types/config/stats, and the preset type aliases.
- **Integration testing** is performed via example programs (e.g., WebSocket
  echo clients) that exercise the full eph-net stack against real endpoints.

Unit-testable pure functions in eph-net that could benefit from dedicated tests:

| Module | Testable Function | Coverage Opportunity |
|--------|-------------------|---------------------|
| `socket_config.hpp` | `SocketConfig::from_url()`, `validate()`, `to_url()`, `to_json()` | URL parsing edge cases, validation boundaries |
| `http_client.hpp` | `build_http_request()`, `parse_http_response()`, `find_header()`, `is_response_complete()` | Malformed responses, header injection, chunked encoding |
| `hmac.hpp` | `hmac_sha256()`, `hmac_sha256_hex()`, `to_hex()`, `to_base64()`, `hmac_verify()` | Known test vectors, empty input, large keys |
| `circuit_breaker.hpp` | `CircuitBreaker` state machine | State transitions, threshold boundary, reset |
| `rate_limiter.hpp` | `RateLimiter::try_acquire()` | Burst exhaustion, refill timing, zero rate |
| `proxy.hpp` | `parse_proxy_url()`, `ProxyConfig::validate()` | Scheme parsing, auth extraction, port validation |
| `kill_switch.hpp` | `KillSwitch::register_transport()`, capacity limit | Max transports, double-register, signal safety |
