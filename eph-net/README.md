# eph-net

Header-only C++23 networking library that provides the POSIX/kernel-side
backend of the eph trading ecosystem. It implements the `TcpTransport` concept
(from `eph-core`) on top of non-blocking sockets + `poll()` so any
`Transport<SocketTransport, Framer, ...>` combination from `eph-transport`
works without DPDK, and ships the exchange-facing building blocks that sit
around the raw transport: a synchronous HTTP/1.1 + TLS client, HMAC-SHA256
signing, circuit breakers, rate limiters, a multi-connection gateway, SOCKS5 /
HTTP CONNECT proxies, and an emergency kill switch.

The DPDK / kernel-bypass backend lives in a sibling subproject (`eph-dpdk`);
`eph-net` is the portable Linux-native path.

## Features

- `SocketTransport` — non-blocking POSIX TCP satisfying `eph::core::TcpTransport`,
  with DNS-timeout-protected connect, deadline-capped `send()`, callback-driven
  `poll_rx()` / `poll_rx_for()`, graceful `close()` (FIN) and forced `reset()`
  (RST via `SO_LINGER`), and optional `SO_TIMESTAMPING` for kernel RX/TX
  latency histograms.
- `connect(url, ...)` / `socket_wss_connect()` / `socket_ws_connect()` —
  one-liner factories that parse a URL, apply user-provided config
  modifications, validate, and hand back a fully-connected
  `Transport<SocketTransport, WsFramer, ...>`.
- Preset `Socket*Transport` aliases for WSS / WS / raw TCP, small/large/evict
  queue variants, plus direct-TX and fully-direct (zero background thread)
  modes.
- `HttpClient` — synchronous HTTP/1.1 client (`GET` / `POST` / `PUT` /
  `DELETE` / generic `request`) with aws-lc TLS, SNI + hostname verification,
  Content-Length **and** chunked transfer-encoding response detection, and a
  hardened request builder (rejects `\r\n\r\n` and bare-LF injection in
  extra headers).
- `hmac_sha256*` — HMAC-SHA256 with hex (Binance, Bybit), base64 (OKX), raw
  digest, and `CRYPTO_memcmp`-based constant-time verifiers for all three.
- `CircuitBreaker` — thread-safe three-state (Closed / Open / HalfOpen)
  breaker with a `Config` struct that carries `validate()` / `warnings()` /
  `dump()` / `to_json()`, plus `is_tripped()` and `time_until_half_open()`
  for monitoring dashboards.
- `RateLimiter` — token-bucket limiter with a `Config` struct, `try_acquire()`
  / `acquire(timeout)` (yield-spin, bounded), `is_exhausted()`, and a const
  `available()` that refills through `mutable` cache state.
- `Gateway` — multi-connection lifecycle manager with background health
  monitor thread, `ConnHealth` state machine (Healthy / Degraded /
  Disconnected / Stopped), `degraded_threshold` based on transport
  `stats().rx_packets` deltas (detected via `if constexpr`), atomic
  `remove_by_tag()` / `reconnect_by_tag()`, and a `for_each` template that
  avoids `std::function` overhead.
- `KillSwitch` — fixed-size (32-transport) signal-safe shutdown coordinator
  with `register_transport` / `unregister_transport` (constrained by
  `Stoppable` concept), `request_shutdown` (lock-free, signal-safe),
  graceful `shutdown()`, emergency `kill()`, `reset()`, `running_count()`,
  and `install_signal_handlers()` (SIGINT / SIGTERM, with second-ctrl-C
  escape hatch).
- `proxy::{ProxyConfig, socks5_handshake, http_connect_handshake,
  make_proxied_factory, parse_proxy_url}` — SOCKS5 (RFC 1928 / 1929) and
  HTTP CONNECT (RFC 7231 §4.3.6) tunneling. SOCKS5 uses ATYP=0x03
  (domain name) for remote-side DNS resolution — no DNS leak — and
  passwords are never logged.

All components use spdlog with `SPDLOG_ACTIVE_LEVEL` for compile-time log
filtering (`SPDLOG_LEVEL_TRACE` in debug, `SPDLOG_LEVEL_INFO` in release).
All `Config` structs expose `validate()`, `warnings()`, `dump()`, `to_json()`,
and defaulted `operator==` so they plug into monitoring and persistence
trivially.

## Layout

```
eph-net/
├── include/eph/
│   ├── net.hpp                     # umbrella: socket backend + Transport templates + framers
│   └── net/
│       ├── socket_config.hpp       # SocketConfig (URL parse, validate, warnings, JSON)
│       ├── socket_transport.hpp    # SocketTransport (TcpTransport concept impl)
│       ├── socket_connect.hpp      # connect() / socket_wss_connect() + Socket*Transport aliases
│       ├── http_message.hpp        # pure HttpResponse + build_http_request + parse_http_response
│       ├── http_client.hpp         # synchronous HTTP/1.1 + aws-lc TLS client
│       ├── hmac.hpp                # HMAC-SHA256 hex/base64/verify (aws-lc)
│       ├── circuit_breaker.hpp     # three-state CircuitBreaker
│       ├── rate_limiter.hpp        # token-bucket RateLimiter
│       ├── gateway.hpp             # multi-connection Gateway + ConnHealth monitor
│       ├── kill_switch.hpp         # signal-safe KillSwitch
│       └── proxy.hpp               # SOCKS5 + HTTP CONNECT tunneling
├── tests/                          # 15 GoogleTest test files (~600 KB source)
├── benchmarks/                     # 13 google-benchmark binaries
├── fuzzers/                        # libFuzzer harnesses (currently: fuzz_http_parse)
└── xmake.lua                       # headeronly target + per-file test/bench targets
```

## Build

`eph-net` is a sub-project of the `ephemeral_dev` monorepo. Build from the
repo root:

```bash
# Debug (SPDLOG_LEVEL_TRACE):
xmake f -m debug
xmake build eph-net

# Release (SPDLOG_LEVEL_INFO):
xmake f -m release
xmake build eph-net

# Sanitizer modes (from top-level xmake.lua):
xmake f -m asan    # AddressSanitizer + UBSan
xmake f -m tsan    # ThreadSanitizer
```

Because the target is `set_kind("headeronly")`, `xmake build eph-net` only
installs headers and validates metadata — the actual compilation happens when
a dependent test, benchmark, or example pulls the headers in.

## Test

Each `tests/test_*.cpp` file becomes its own xmake target via the `eph-test`
rule. Run the whole suite or a single test:

```bash
xmake build                              # build everything (tests + benches)
xmake run test_socket_transport
xmake run test_gateway
xmake run test_rate_limiter
```

Coverage highlights (by file):

| File | Focus |
|---|---|
| `test_socket_transport.cpp` | Non-blocking connect, send/recv, close, reset, MSS query |
| `test_http.cpp` / `test_http_client.cpp` | Builder injection guards, parser, TLS path, chunked / CL detection |
| `test_tls_record.cpp` | TLS record-layer edge cases |
| `test_hmac.cpp` | HMAC determinism, large messages, hex/base64, constant-time verify |
| `test_circuit_breaker.cpp` | State transitions, concurrent access, warnings |
| `test_rate_limiter.cpp` | Refill precision, const-correctness, exhaustion, burst |
| `test_gateway.cpp` | Add/remove, health transitions, monitor thread, `dump()` deadlock regression |
| `test_kill_switch.cpp` | Capacity limits, signal-safe flag, formatter, register/unregister race |
| `test_proxy.cpp` | SOCKS5 handshake phases, HTTP CONNECT auth, URL parser |
| `test_websocket.cpp` / `test_framer.cpp` | WS framing + length-prefix framer |
| `test_transport.cpp` / `test_transport_types.cpp` | Transport<> integration with SocketTransport |
| `test_tcp_concept.cpp` | `TcpTransport` concept compliance |

## Benchmark

```bash
xmake run bench_socket_config
xmake run bench_hmac
xmake run bench_circuit_breaker
xmake run bench_rate_limiter
xmake run bench_gateway
xmake run bench_kill_switch
xmake run bench_http_client
xmake run bench_proxy
xmake run bench_ws
xmake run bench_tls
xmake run bench_rx_pipeline
xmake run bench_transport_pipeline
xmake run bench_control_plane
```

Per the project-wide benchmarking discipline, establish a baseline before
modifying any benchmarked hot path and re-run after changes to catch
regressions.

## Examples

### One-liner WebSocket connection

```cpp
#include <eph/net.hpp>

auto result = eph::net::connect(
    "wss://stream.binance.com:9443/ws/btcusdt@bookTicker",
    [](auto& cfg) {
        cfg.on_message = [](const uint8_t* data, uint16_t len, uint8_t) {
            // handle frame
        };
    });
if (!result) {
    spdlog::error("connect failed: {}", result.error().detail);
    return 1;
}
auto& transport = *result;
transport->send_text(
    R"({"method":"SUBSCRIBE","params":["btcusdt@bookTicker"]})");
transport->stop();
```

### Signed REST call (Binance)

```cpp
#include <eph/net/http_client.hpp>
#include <eph/net/hmac.hpp>

eph::net::HttpClient client(
    eph::net::HttpClient::Config{.host = "api.binance.com", .port = 443});

std::string query = "timestamp=1234567890000&recvWindow=5000";
auto sig = eph::net::hmac_sha256_hex(api_secret, query);
if (!sig) { /* handle error */ }

auto resp = client.get(
    std::format("/api/v3/account?{}&signature={}", query, *sig));
if (resp && resp->is_success()) {
    // resp->body has the JSON
}
```

### Circuit breaker + rate limiter

```cpp
#include <eph/net/circuit_breaker.hpp>
#include <eph/net/rate_limiter.hpp>

eph::net::CircuitBreaker breaker({
    .failure_threshold   = 5,
    .open_duration       = std::chrono::seconds{30},
    .half_open_max_calls = 1,
});

eph::net::RateLimiter limiter(
    eph::net::RateLimiter::Config{.rate_per_sec = 20.0, .burst = 5});

if (!breaker.allow())            { return; }
if (!limiter.try_acquire())      { return; }

auto resp = client.post("/api/v3/order", order_json);
if (resp && resp->is_success()) breaker.record_success();
else                            breaker.record_failure();
```

### Multi-connection gateway

```cpp
#include <eph/net/gateway.hpp>

eph::net::Gateway gw({
    .health_check_interval = std::chrono::milliseconds{5000},
    .degraded_threshold    = std::chrono::milliseconds{30000},
    .on_health_change      = [](std::string_view tag, auto old_h, auto new_h) {
        spdlog::warn("{}: {} -> {}", tag,
                     eph::net::conn_health_name(old_h),
                     eph::net::conn_health_name(new_h));
    },
});

auto id1 = gw.add("binance-btcusdt", transport1.get());
auto id2 = gw.add("binance-ethusdt", transport2.get(), /*priority=*/1);
gw.start_all();
gw.start_monitor();
// ...
gw.stop_monitor();
gw.stop_all();
```

### Emergency shutdown

```cpp
#include <eph/net/kill_switch.hpp>

eph::net::KillSwitch ks;
(void)ks.register_transport(transport1.get());
(void)ks.register_transport(transport2.get());
ks.install_signal_handlers();

while (!ks.is_shutdown_requested()) {
    // main loop
}
ks.shutdown();  // graceful; blocks until every registered transport stops
```

### Proxy tunneling

```cpp
#include <eph/net/proxy.hpp>

auto proxy_cfg = eph::net::proxy::parse_proxy_url(
    "socks5://user:pass@proxy.example.com:1080");
if (!proxy_cfg) { /* handle error */ }

auto factory = eph::net::proxy::make_proxied_factory(
    eph::net::SocketConfig{.tcp_nodelay = true},
    *proxy_cfg,
    "stream.binance.com", 9443);

auto result = eph::net::SocketWssTransport::create(
    std::move(factory), transport_config);
```

## Dependencies

Internal (from the `ephemeral_dev` monorepo):

- **eph-core** — `TcpTransport` concept, framer concepts, length-prefix
  framer, json_escape / base64 / string-check detail helpers
- **eph-transport** — `Transport<>`, `DirectTxTransport<>`, `DirectTransport<>`,
  `TransportConfig`, `ConnectionErrorInfo`, WS framer, raw framer, presets,
  `RttStats`, `TcpState`
- **eph-utils** — HDR histogram, TSC timing
- **eph-containers** — SPSC bounded queue (consumed through `Transport<>`)

External:

- **aws-lc** (OpenSSL-compatible) — TLS handshake / encryption in
  `HttpClient`, HMAC-SHA256 + base64 in `hmac.hpp`
- **spdlog** — leveled logging everywhere, `SPDLOG_ACTIVE_LEVEL` filtered at
  compile time

## License

See the repository root for the project-wide license.
