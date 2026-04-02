# eph-net

Header-only C++23 networking library for low-latency WebSocket transport, HTTP REST clients, and connection infrastructure. Provides the POSIX socket backend for the `Transport<>` template, along with supporting components for exchange connectivity (rate limiting, circuit breaking, proxy tunneling, emergency shutdown).

## Overview

`eph-net` is the network layer of the eph ecosystem. It bridges the protocol-agnostic `Transport<>` engine (from `eph-transport`) with the Linux kernel TCP stack, providing:

- A `SocketTransport` class satisfying the `TcpTransport` concept, so any `Transport<SocketTransport, Framer, ...>` combo works out of the box.
- One-liner connect functions (`connect()`, `socket_wss_connect()`) that eliminate factory boilerplate.
- A synchronous `HttpClient` for off-hot-path REST API calls (order placement, balance queries, snapshots).
- HMAC-SHA256 signing for authenticated exchange APIs (Binance, Bybit, OKX).
- Infrastructure components: circuit breaker, rate limiter, multi-connection gateway, proxy tunneling, and emergency kill switch.

All headers live under `include/eph/net/`. The convenience header `include/eph/net.hpp` pulls in the socket transport, connect functions, framer types, and transport engine.

## Key Components

### Socket Transport

- **socket_config.hpp** -- `SocketConfig` struct for POSIX socket TCP connections. Fields for host, port, TCP_NODELAY, SO_RCVBUF/SO_SNDBUF, keepalive tuning, SO_BINDTODEVICE, and send timeout. URL parser (`from_url()`), early validation (`validate()`), and serialization (`dump()`, `to_json()`, `to_url()`). Compile-time `kEnableSocketTimestamps` switch for SO_TIMESTAMPING support.
- **socket_transport.hpp** -- `SocketTransport` class implementing the `TcpTransport` concept with non-blocking POSIX sockets and `poll()` for I/O multiplexing. DNS resolution with timeout via `std::async`, non-blocking `send()` with deadline-capped retries, callback-driven `poll_rx()` / `poll_rx_for()`, graceful `close()` (FIN), and forced `reset()` (RST). Tracks MSS, resolved IP, DNS/connect latency, and optional kernel RX/TX timestamps.
- **socket_connect.hpp** -- Convenience connect functions and preset-based type aliases. `socket_wss_connect()`, `socket_ws_connect()`, and `connect(url)` one-liner factories. Type aliases for all transport variants.

### HTTP and Crypto

- **http_client.hpp** -- Synchronous HTTP/1.1 client for REST API calls. One connection per request, POSIX sockets + aws-lc TLS. `HttpClient` class with `get()` and `post()`. Standalone `build_http_request()`, `parse_http_response()`, and `find_header()` utilities.
- **hmac.hpp** -- HMAC-SHA256 signing for authenticated exchange REST APIs. Hex output (`hmac_sha256_hex()`) for Binance/Bybit, base64 output (`to_base64()`) for OKX. Constant-time verification (`hmac_verify()`, `hmac_verify_hex()`). Uses aws-lc as the cryptographic backend.

### Infrastructure

- **circuit_breaker.hpp** -- Three-state circuit breaker (Closed/Open/HalfOpen) for endpoint protection. Prevents hammering broken endpoints with configurable failure threshold and backoff. Thread-safe via `std::mutex`.
- **rate_limiter.hpp** -- Token bucket rate limiter for exchange API request throttling. Configurable sustained rate and burst capacity. Non-blocking `try_acquire()` and blocking `acquire()`. Thread-safe, nanosecond-precision refill.
- **gateway.hpp** -- Multi-connection lifecycle manager. Type-erased connection storage with per-connection tagging and priority. Background health monitor thread with configurable check interval and health-change callbacks.
- **proxy.hpp** -- SOCKS5 (RFC 1928/1929) and HTTP CONNECT (RFC 7231) proxy tunneling. `ProxyConfig` struct, handshake functions, `make_proxied_factory()` for `Transport::create()`, and `parse_proxy_url()` URL parser. DNS resolution on the proxy side (no DNS leak).
- **kill_switch.hpp** -- Centralized emergency shutdown coordinator. Fixed-size registration (up to 32 transports, no heap). Graceful `shutdown()` and emergency `kill()`. Signal-safe `request_shutdown()` via lock-free atomic. `install_signal_handlers()` for SIGINT/SIGTERM.

## Public API Reference

### `eph::net` namespace

#### SocketConfig (`socket_config.hpp`)

| Member | Description |
|--------|-------------|
| `std::string host` | Target hostname or IP address |
| `uint16_t port` | Target port number |
| `bool tcp_nodelay` | Disable Nagle's algorithm (default `true`) |
| `int recv_buf_size` | SO_RCVBUF size in bytes (0 = OS default) |
| `int send_buf_size` | SO_SNDBUF size in bytes (0 = OS default) |
| `bool tcp_keepalive` | Enable TCP keepalive probes |
| `int keepalive_idle` | Seconds before first probe (default 60) |
| `int keepalive_interval` | Seconds between probes (default 10) |
| `int keepalive_count` | Probes before declaring dead (default 3) |
| `int send_timeout_ms` | Timeout for individual send() poll waits in ms (default 1000) |
| `std::string bind_device` | SO_BINDTODEVICE NIC name (requires CAP_NET_RAW) |
| `static from_url(string_view) -> expected<SocketConfig, string>` | Parse `tcp://host:port` or `host:port` into a SocketConfig |
| `to_url() -> string` | Serialize as `tcp://host:port` |
| `validate() -> string_view` | Return error description or empty on valid config |
| `dump() -> string` | Multi-line human-readable format |
| `to_json() -> string` | Compact JSON serialization |

Compile-time constant: `inline constexpr bool kEnableSocketTimestamps` (set via `-DEPH_ENABLE_TIMESTAMPS=1`).

#### SocketTransport (`socket_transport.hpp`)

| Method | Description |
|--------|-------------|
| `SocketTransport(const SocketConfig&)` | Construct (does not connect) |
| `connect(timeout) -> expected<void, string>` | Establish TCP connection with DNS timeout protection |
| `send(data, len) -> expected<size_t, string>` | Non-blocking send with deadline-capped retries |
| `poll_rx(callback) -> expected<uint16_t, string>` | Non-blocking receive, delivers data via callback |
| `poll_rx_for(callback, timeout) -> expected<uint16_t, string>` | Timed receive with poll() wait |
| `close() -> expected<void, string>` | Graceful shutdown (send FIN) |
| `reset()` | Forceful reset (send RST via SO_LINGER) |
| `state() -> TcpState` | Current TCP state |
| `mss() -> uint16_t` | Negotiated Maximum Segment Size |
| `is_established() -> bool` | Check if connected |
| `config() -> const SocketConfig&` | Access socket configuration |
| `fd() -> int` | Underlying file descriptor |
| `resolved_ip() -> string_view` | IP address from last connect |
| `dns_latency_ns() -> uint64_t` | DNS resolution time |
| `connect_latency_ns() -> uint64_t` | Total connect time (DNS + TCP handshake) |
| `rx_latency() -> RttStats` | Kernel RX stack latency histogram (timestamps mode) |
| `tx_latency() -> RttStats` | Kernel TX stack latency histogram (timestamps mode) |
| `last_rx_burst_tsc() -> uint64_t` | TSC captured after recvmsg returns data |
| `local_port() -> uint16_t` | Ephemeral port number of connected socket |

#### Connect Functions (`socket_connect.hpp`)

| Function | Description |
|----------|-------------|
| `connect(url, modifier?, sock_cfg?) -> expected<unique_ptr<Transport<...>>, ConnectionErrorInfo>` | One-liner: parse URL, create transport, connect |
| `socket_wss_connect(config, sock_cfg?) -> expected<unique_ptr<Transport<...>>, ConnectionErrorInfo>` | Create WSS transport from TransportConfig |
| `socket_ws_connect(config, sock_cfg?) -> expected<unique_ptr<Transport<...>>, ConnectionErrorInfo>` | Create plain WS transport (no TLS) |

#### Transport Type Aliases (`socket_connect.hpp`)

| Alias | Description |
|-------|-------------|
| `SocketWssTransport` | Standard WSS, 512B payload, 1024-deep queues |
| `SocketWssSmallTransport` | Small WSS, 64B payload, 256-deep queues |
| `SocketWssLargeTransport` | Large WSS, 4KB payload, 512-deep queues |
| `SocketWssEvictTransport` | WSS with evicting RX queue (drops stale) |
| `SocketWsTransport` | Plain WS (no TLS) |
| `SocketRawTransport` | Raw TCP (no WebSocket framing) |
| `SocketDirectTxTransport` | Direct TX mode (send on caller thread) |
| `SocketDirectTxSmallTransport` | Direct TX, small-payload variant |
| `SocketDirectTxRawTransport` | Direct TX, raw variant |
| `SocketDirectTransport` | Full direct mode (no background threads) |
| `SocketDirectSmallTransport` | Full direct, small-payload variant |
| `SocketDirectRawTransport` | Full direct, raw variant |

#### HttpClient (`http_client.hpp`)

| Type / Function | Description |
|----------------|-------------|
| `HttpResponse` | Struct: `int status_code`, `string body`, `string headers_raw` |
| `HttpClient(Config)` | Construct with host, port, TLS, timeout, max_response_size |
| `HttpClient::get(path, extra_headers?) -> expected<HttpResponse, string>` | Send GET request |
| `HttpClient::post(path, body, content_type?, extra_headers?) -> expected<HttpResponse, string>` | Send POST request |
| `build_http_request(method, host, path, body?, content_type?, extra_headers?) -> expected<string, string>` | Build raw HTTP/1.1 request string |
| `parse_http_response(data) -> expected<HttpResponse, string>` | Parse raw HTTP response bytes |
| `find_header(headers_raw, name) -> string` | Case-insensitive header value lookup |

#### HMAC (`hmac.hpp`)

| Function | Description |
|----------|-------------|
| `hmac_sha256(key, message) -> expected<array<uint8_t, 32>, string>` | Raw 32-byte HMAC-SHA256 digest |
| `hmac_sha256_hex(key, message) -> expected<string, string>` | HMAC-SHA256 as 64-char lowercase hex (Binance/Bybit) |
| `hmac_verify(key, message, expected) -> bool` | Constant-time HMAC verification (raw bytes) |
| `hmac_verify_hex(key, message, expected_hex) -> bool` | Constant-time HMAC verification (hex string) |
| `to_hex(bytes) -> string` | Encode byte span as lowercase hex |
| `to_base64(bytes) -> expected<string, string>` | Encode byte span as base64 (OKX) |

#### CircuitBreaker (`circuit_breaker.hpp`)

| Type / Method | Description |
|--------------|-------------|
| `CircuitState` | Enum: `Closed`, `Open`, `HalfOpen` |
| `CircuitBreaker(Config)` | Construct with failure_threshold, open_duration, half_open_max_calls |
| `allow() -> bool` | Check if a call is permitted |
| `record_success()` | Record successful call (may close circuit) |
| `record_failure()` | Record failed call (may trip circuit) |
| `state() -> CircuitState` | Query current state |
| `failure_count() -> size_t` | Consecutive failure count |
| `reset()` | Force-reset to Closed |

#### RateLimiter (`rate_limiter.hpp`)

| Method | Description |
|--------|-------------|
| `RateLimiter(rate_per_sec, burst)` | Construct with sustained rate and burst capacity |
| `try_acquire(n?) -> bool` | Non-blocking token consumption |
| `acquire(n?)` | Blocking token consumption (spins with yield) |
| `available() -> double` | Approximate available tokens |
| `reset()` | Refill to full burst capacity |

#### Gateway (`gateway.hpp`)

| Type / Method | Description |
|--------------|-------------|
| `ConnHealth` | Enum: `Healthy`, `Degraded`, `Disconnected`, `Stopped` |
| `conn_health_name(ConnHealth) -> string_view` | Human-readable health status |
| `GatewayConnection` | Type-erased connection with tag, health, priority |
| `Gateway(Config?)` | Construct with health_check_interval, degraded_threshold, on_health_change callback |
| `add(tag, transport*, priority?) -> size_t` | Register a transport, returns connection index |
| `connection_count() -> size_t` | Number of managed connections |
| `health(id) -> ConnHealth` | Query connection health by index |
| `tag(id) -> string` | Query connection tag by index |
| `start_all()` | Start all stopped connections |
| `stop_all()` | Stop all running connections |
| `reconnect(id)` | Force reconnect a specific connection |
| `start_monitor()` | Start background health monitor thread |
| `stop_monitor()` | Stop health monitor thread |
| `check_health()` | Run one health check cycle |
| `dump() -> string` | Formatted status of all connections |

### `eph::net::proxy` namespace (`proxy.hpp`)

| Type / Function | Description |
|----------------|-------------|
| `ProxyType` | Enum: `kSocks5`, `kHttpConnect` |
| `ProxyConfig` | Struct: host, port, type, username, password, timeout, `validate()` |
| `socks5_handshake(tcp, target_host, target_port, cfg) -> expected<void, string>` | Execute SOCKS5 tunnel on connected socket |
| `http_connect_handshake(tcp, target_host, target_port, cfg) -> expected<void, string>` | Execute HTTP CONNECT tunnel on connected socket |
| `make_proxied_factory(sock_cfg, proxy_cfg, host, port) -> function<...>` | Build a TcpFactory that connects through a proxy |
| `parse_proxy_url(url) -> expected<ProxyConfig, string>` | Parse `socks5://` or `http://` proxy URL |

#### KillSwitch (`kill_switch.hpp`)

| Type / Method | Description |
|--------------|-------------|
| `kKillSwitchMaxTransports` | Compile-time constant: max 32 registered transports |
| `TransportHandle` | Type-erased transport wrapper (ptr + stop_fn + is_running_fn) |
| `KillSwitch()` | Construct (one per application) |
| `register_transport(tp*) -> bool` | Register transport for coordinated shutdown |
| `unregister_transport(tp*)` | Remove transport before destruction |
| `transport_count() -> size_t` | Number of registered transports |
| `is_shutdown_requested() -> bool` | Check shutdown flag (main loop condition) |
| `request_shutdown()` | Set shutdown flag (signal-safe, lock-free) |
| `shutdown()` | Graceful: stop all transports, block until done |
| `kill()` | Emergency: set flag without blocking |
| `install_signal_handlers()` | Hook SIGINT/SIGTERM to trigger shutdown |

## Dependencies

- **eph-core** -- `TcpTransport` concept (`tcp_concept.hpp`), framer concepts, length-prefix framer, JSON escape utility
- **eph-transport** -- Transport engine (`transport.hpp`), transport types/config/stats, WS framer, raw framer, presets, direct transport variants
- **eph-utils** -- HDR histogram, TSC timing
- **eph-containers** -- SPSC bounded queue (used by Transport)
- **aws-lc** (OpenSSL-compatible) -- TLS handshake/encryption (via Transport), HMAC-SHA256 (`hmac.hpp`), base64 encoding
- **spdlog** -- Leveled logging throughout all components

## Usage Examples

### One-liner WebSocket connection

```cpp
#include <eph/net.hpp>

auto result = eph::net::connect("wss://stream.binance.com:9443/ws/btcusdt@bookTicker",
    [](auto& cfg) {
        cfg.on_message = [](const uint8_t* data, uint16_t len, uint8_t opcode) {
            // Handle incoming WebSocket message
        };
    });
if (!result) { /* handle error */ }
auto& transport = *result;

transport->send_text(R"({"method":"SUBSCRIBE","params":["btcusdt@bookTicker"]})");

// Graceful shutdown
transport->stop();
```

### REST API call with HMAC signing

```cpp
#include <eph/net/http_client.hpp>
#include <eph/net/hmac.hpp>

// Create an HTTP client for Binance REST API
eph::net::HttpClient client({.host = "api.binance.com", .port = 443});

// Sign the query string
std::string query = "timestamp=1234567890000&recvWindow=5000";
auto sig = eph::net::hmac_sha256_hex(api_secret, query);
if (!sig) { /* handle error */ }

// Send authenticated request
auto resp = client.get(std::format("/api/v3/account?{}&signature={}", query, *sig));
if (resp && resp->status_code == 200) {
    // Parse resp->body (JSON)
}
```

### Circuit breaker + rate limiter

```cpp
#include <eph/net/circuit_breaker.hpp>
#include <eph/net/rate_limiter.hpp>

// 5 failures trips the breaker, 30s cooldown, 1 probe call
eph::net::CircuitBreaker breaker({5, std::chrono::seconds{30}, 1});

// 20 requests/sec sustained, burst of 5
eph::net::RateLimiter limiter(20.0, 5);

void send_order(/* ... */) {
    if (!breaker.allow()) { /* endpoint is down, skip */ return; }
    if (!limiter.try_acquire()) { /* rate limited, back off */ return; }

    auto resp = client.post("/api/v3/order", order_json);
    if (resp) {
        breaker.record_success();
    } else {
        breaker.record_failure();
    }
}
```

### Emergency shutdown coordination

```cpp
#include <eph/net/kill_switch.hpp>

eph::net::KillSwitch ks;
ks.register_transport(transport1.get());
ks.register_transport(transport2.get());
ks.install_signal_handlers();  // SIGINT, SIGTERM

while (!ks.is_shutdown_requested()) {
    // Main event loop
}
ks.shutdown();  // Graceful: stops all transports and joins threads
```

### Multi-connection gateway

```cpp
#include <eph/net/gateway.hpp>

eph::net::Gateway gw({
    .health_check_interval = std::chrono::milliseconds{5000},
    .on_health_change = [](std::string_view tag, auto old_h, auto new_h) {
        spdlog::warn("Connection '{}' health: {} -> {}",
                     tag, conn_health_name(old_h), conn_health_name(new_h));
    },
});

auto id1 = gw.add("binance-btcusdt", transport1.get());
auto id2 = gw.add("binance-ethusdt", transport2.get(), /*priority=*/1);
gw.start_all();
gw.start_monitor();

// ... run application ...

gw.stop_monitor();
gw.stop_all();
```

### Proxy tunneling (SOCKS5)

```cpp
#include <eph/net/proxy.hpp>

auto proxy_cfg = eph::net::proxy::parse_proxy_url("socks5://user:pass@proxy.example.com:1080");
if (!proxy_cfg) { /* handle error */ }

auto factory = eph::net::proxy::make_proxied_factory(
    eph::net::SocketConfig{.tcp_nodelay = true},
    *proxy_cfg,
    "stream.binance.com", 9443);

// Use factory with Transport::create()
auto result = eph::net::SocketWssTransport::create(std::move(factory), transport_config);
```
