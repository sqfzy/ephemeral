# eph-net

Header-only C++23 networking library for low-latency WebSocket transport, HTTP REST clients, and connection infrastructure. Provides the POSIX socket backend for the `Transport<>` template, along with supporting components for exchange connectivity (rate limiting, circuit breaking, proxy tunneling, emergency shutdown).

## Key Components

All headers are under `include/eph/net/`:

- **socket_config.hpp** -- Configuration struct for POSIX socket TCP transport. Includes `SocketConfig` with fields for host, port, TCP_NODELAY, buffer sizes, keepalive tuning, SO_BINDTODEVICE, and send timeout. Provides `from_url()` URL parser (supports `tcp://host:port`, IPv6 brackets), `validate()` for early error checking, and `dump()` / `to_json()` serialization. Compile-time `kEnableSocketTimestamps` switch for SO_TIMESTAMPING support.
- **socket_transport.hpp** -- POSIX socket TCP transport satisfying the `TcpTransport` concept. Non-blocking sockets with `poll()` for I/O multiplexing. Provides `connect()` with DNS timeout protection (`std::async` + `wait_for`), non-blocking `send()` with deadline-capped retries, `poll_rx()` / `poll_rx_for()` callback-driven receive, graceful `close()` (FIN) and `reset()` (RST). Tracks MSS, resolved IP, DNS/connect latency. Optional kernel RX/TX timestamps via SO_TIMESTAMPING when `EPH_ENABLE_TIMESTAMPS=1`.
- **socket_connect.hpp** -- Convenience connect functions and preset-based type aliases for socket-backed transports. Provides `socket_wss_connect()`, `socket_ws_connect()`, and `connect(url)` one-liner factories. Defines type aliases: `SocketWssTransport` (512B/1024 depth), `SocketWssSmallTransport` (64B/256), `SocketWssLargeTransport` (4KB/512), `SocketWssEvictTransport` (evicting RX queue), `SocketRawTransport` (no WS framing), plus Direct TX and full Direct mode variants.
- **http_client.hpp** -- Minimal synchronous HTTP/1.1 client for REST API calls (order placement, balance queries, orderbook snapshots). One connection per request, POSIX sockets + aws-lc TLS. `HttpClient` class with `get()` and `post()` methods returning `HttpResponse` (status code, body, raw headers). Includes standalone `build_http_request()`, `parse_http_response()`, and `find_header()` utility functions. NOT for hot-path use.
- **hmac.hpp** -- HMAC-SHA256 signing for authenticated exchange REST APIs. Covers Binance/Bybit (hex signatures via `hmac_sha256_hex()`) and OKX (base64 signatures via `to_base64()`). Constant-time verification via `hmac_verify()` / `hmac_verify_hex()` using `CRYPTO_memcmp`. Uses aws-lc as the cryptographic backend.
- **circuit_breaker.hpp** -- Three-state circuit breaker (Closed/Open/HalfOpen) for exchange endpoint protection. Prevents hammering broken endpoints by tracking consecutive failures and backing off. Thread-safe via `std::mutex`. Configurable failure threshold, open duration, and half-open probe count.
- **rate_limiter.hpp** -- Token bucket rate limiter for exchange API request throttling. Configurable sustained rate and burst capacity. `try_acquire()` for non-blocking checks, `acquire()` for blocking waits. Thread-safe, nanosecond-precision refill via `steady_clock`.
- **gateway.hpp** -- Multi-connection lifecycle manager for coordinated Transport start/stop/reconnect. Type-erased connection storage with per-connection tagging and priority. Background health monitor thread with configurable check interval and health-change callbacks. `ConnHealth` enum: Healthy, Degraded, Disconnected, Stopped.
- **proxy.hpp** -- SOCKS5 (RFC 1928/1929) and HTTP CONNECT (RFC 7231) proxy tunneling for SocketTransport. `ProxyConfig` for proxy address, type, and optional auth. `socks5_handshake()` and `http_connect_handshake()` execute on established TCP connections. `make_proxied_factory()` builds a `TcpFactory` for `Transport::create()`. `parse_proxy_url()` parses `socks5://` and `http://` URLs. DNS resolution happens proxy-side (no DNS leak).
- **kill_switch.hpp** -- Centralized emergency shutdown coordinator. Registers up to 32 transports (fixed array, no heap). Two shutdown paths: graceful `shutdown()` (stop all transports, join threads) and emergency `kill()` (non-blocking flag set). Signal-safe `request_shutdown()` via lock-free atomic. `install_signal_handlers()` hooks SIGINT/SIGTERM. Double-Ctrl-C falls through to default handler for hard kill.

The convenience header `include/eph/net.hpp` includes the socket transport, connect functions, framer types, and transport engine.

## Dependencies

- **eph-core** -- `TcpTransport` concept (`tcp_concept.hpp`), framer concepts, length-prefix framer, JSON escape utility
- **eph-transport** -- Transport engine (`transport.hpp`), transport types/config/stats, WS framer, raw framer, presets, direct transport variants
- **eph-utils** -- HDR histogram, TSC timing
- **eph-containers** -- SPSC bounded queue (used by Transport)
- **aws-lc** (OpenSSL-compatible) -- TLS handshake/encryption (via Transport), HMAC-SHA256 (`hmac.hpp`), base64 encoding
- **spdlog** -- Leveled logging throughout all components

## Quick Start

```cpp
#include <eph/net.hpp>

// One-liner WSS connection from URL
auto result = eph::net::connect("wss://stream.binance.com:9443/ws/btcusdt@bookTicker",
    [](auto& cfg) {
        cfg.on_message = [](const uint8_t* data, uint16_t len, uint8_t opcode) {
            // Handle incoming message
        };
    });
if (!result) { /* handle error */ }
auto& transport = *result;

// Send a text message
transport->send_text(R"({"method":"SUBSCRIBE","params":["btcusdt@bookTicker"]})");

// Graceful shutdown
transport->stop();
```

```cpp
#include <eph/net/http_client.hpp>
#include <eph/net/hmac.hpp>

// REST API call with HMAC signing
eph::net::HttpClient client({.host = "api.binance.com", .port = 443});
auto sig = eph::net::hmac_sha256_hex(api_secret, query_string);
auto resp = client.get(std::format("/api/v3/account?{}&signature={}", query_string, *sig));
```

```cpp
#include <eph/net/kill_switch.hpp>

// Emergency shutdown coordination
eph::net::KillSwitch ks;
ks.register_transport(transport.get());
ks.install_signal_handlers();  // SIGINT, SIGTERM

while (!ks.is_shutdown_requested()) {
    // main loop
}
ks.shutdown();
```
