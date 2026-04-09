# Project: eph-net

> Header-only C++23 kernel-side networking library for the eph trading
> stack — POSIX socket transport, HTTP/1.1 + TLS client, HMAC signing, and
> operational primitives (circuit breaker, rate limiter, gateway,
> kill switch, proxy).

**Language**: C++23 | **Build**: xmake (`set_kind("headeronly")`) |
**Logging**: spdlog (`SPDLOG_ACTIVE_LEVEL`) | **Crypto / TLS**: aws-lc

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Module Map](#module-map)
4. [Data Flow](#data-flow)
5. [Key Components](#key-components)
6. [Entry Points & APIs](#entry-points--apis)
7. [Dependencies](#dependencies)
8. [Testing](#testing)

---

## Overview

`eph-net` is the kernel-side half of the eph transport layer. Its job is to
provide an in-kernel POSIX backend for the `eph::core::TcpTransport` concept
so the protocol-agnostic `Transport<>` engine in `eph-transport` (which
handles threading, framing, TLS offload, SPSC queues) can be instantiated
against real Linux sockets. The sibling `eph-dpdk` subproject provides the
equivalent kernel-bypass backend; everything that lives **above** the
transport concept — WS framing, TLS, reconnection, statistics — is shared.

On top of that raw transport, `eph-net` also houses the exchange-facing
operational primitives that every real exchange client needs but aren't
"transport" in the tight sense: a synchronous HTTP/1.1 + TLS client for REST
calls, HMAC-SHA256 signing for authenticated endpoints, a token-bucket rate
limiter to stay under exchange quotas, a three-state circuit breaker so
repeated failures back off exponentially rather than hammering a dead venue,
a multi-connection `Gateway` with a health-monitoring thread, a SOCKS5 / HTTP
CONNECT proxy module, and a fixed-size signal-safe `KillSwitch` that
coordinates emergency teardown.

The whole library is header-only (`set_kind("headeronly")` in `xmake.lua`),
leans heavily on C++23 features (`std::expected`, `std::format`, `concepts`,
`if constexpr` template-detection of `stats().rx_packets`), and treats every
`Config` struct as a first-class value with `validate()` / `warnings()` /
`dump()` / `to_json()` / defaulted `operator==`. All runtime paths log
through spdlog with `SPDLOG_ACTIVE_LEVEL` filtering (`SPDLOG_LEVEL_TRACE`
under `-m debug`, `SPDLOG_LEVEL_INFO` under `-m release`). The data plane
itself (the `send()` / `poll_rx()` hot loop inside `SocketTransport`) is
allocation-free.

---

## Architecture

Layered, with two orthogonal concerns — (1) plug a real kernel transport
into the shared `Transport<>` engine, and (2) wrap that transport with
exchange-specific control-plane primitives.

### Component Diagram

```
                        Application (strategy / client)
  |                 |                  |                   |
  v                 v                  v                   v
 connect()/     HttpClient        CircuitBreaker /    KillSwitch /
 socket_wss_    (REST)            RateLimiter /       signal handlers
 connect()                        Gateway
  |                 |                                      |
  v                 v                                      v
 +---------------------------------+    +--------------------------------+
 | Transport<SocketTransport, WS,  |    | Gateway monitors is_running()  |
 |   MaxPayload, QueueDepth>       |    | + stats().rx_packets delta     |
 | (from eph-transport)            |    | and calls stop()/start_threads |
 +------+--------------------------+    +-------+------------------------+
        |                                       |
        v                                       |
 +------+---------------------------------------+-----+
 | SocketTransport (satisfies TcpTransport concept)   |
 | - connect() async DNS + non-blocking TCP handshake |
 | - send()    deadline-capped, EAGAIN/poll retry     |
 | - poll_rx() recvmsg() + optional SO_TIMESTAMPING   |
 | - close()   SHUT_WR -> FIN -> drain peer FIN       |
 | - reset()   SO_LINGER(0) -> RST                    |
 +------+---------------------------------------------+
        |
        v
 Linux kernel TCP / TLS (aws-lc via HttpClient only; WSS
 handled by Transport<> on top of this raw TCP socket)
```

`proxy::make_proxied_factory()` injects itself between `connect()` and the
SocketTransport constructor — it returns a `TcpFactory` that connects to the
proxy first, runs SOCKS5 / HTTP CONNECT, and hands the tunneled socket to
`Transport<>::create()` exactly as if it were a direct connection.

---

## Module Map

| Module / File | Responsibility | Key Types / Functions | Depends On |
|---|---|---|---|
| `include/eph/net.hpp` | Umbrella header (kernel backend + transport templates + framers) | — | `socket_config`, `socket_transport`, `socket_connect`, `eph-core::tcp_concept`, `eph-transport::{transport,transport_types,direct_tx_transport,direct_transport,ws_framer,raw_framer}`, `eph-core::length_prefix_framer` |
| `include/eph/net/socket_config.hpp` | Socket target + TCP options + URL parser | `SocketConfig`, `kEnableSocketTimestamps` (deprecated alias) | `eph-core::detail::{json_escape,string_checks}`, spdlog |
| `include/eph/net/socket_transport.hpp` | Non-blocking POSIX TCP, optional `SO_TIMESTAMPING` histograms | `SocketTransport` | `socket_config`, `eph-core::tcp_concept`, `eph-transport::transport_types`, `eph-utils::{HdrHistogram,TSC}`, spdlog |
| `include/eph/net/socket_connect.hpp` | One-liner URL factories + preset type aliases | `connect()`, `socket_wss_connect()`, `socket_ws_connect()`, `Socket{Wss,Ws,Raw,DirectTx,Direct}*Transport` | `socket_transport`, `eph-transport::{presets,transport}` |
| `include/eph/net/http_message.hpp` | Pure HTTP/1.1 value types and stateless parsers (no network I/O) | `HttpResponse`, `build_http_request()`, `parse_http_response()`, `find_header[_opt]()`, `is_http_response_complete()` | `eph-core::detail::json_escape`, spdlog |
| `include/eph/net/http_client.hpp` | Synchronous HTTP/1.1 + TLS client (one connection per request) | `HttpClient`, `HttpClient::Config` | `http_message`, aws-lc (`SSL_*`), spdlog |
| `include/eph/net/hmac.hpp` | HMAC-SHA256 signing + constant-time verification | `hmac_sha256`, `hmac_sha256_hex`, `hmac_sha256_base64`, `hmac_verify[_hex,_base64]`, `to_hex`, `to_base64` | aws-lc (`HMAC`, `EVP_EncodeBlock`, `CRYPTO_memcmp`), spdlog |
| `include/eph/net/circuit_breaker.hpp` | Three-state circuit breaker | `CircuitBreaker`, `CircuitBreaker::Config`, `CircuitState`, `circuit_state_name()` | spdlog |
| `include/eph/net/rate_limiter.hpp` | Token bucket rate limiter with mutable-cache refill | `RateLimiter`, `RateLimiter::Config` | spdlog |
| `include/eph/net/gateway.hpp` | Multi-connection lifecycle manager + health monitor | `Gateway`, `Gateway::Config`, `GatewayConnection`, `ConnHealth`, `GatewayManageable` concept, `conn_health_name()` | `kill_switch` (for `Stoppable`), `eph-core::detail::json_escape`, spdlog |
| `include/eph/net/kill_switch.hpp` | Signal-safe shutdown coordinator | `KillSwitch`, `TransportHandle`, `kKillSwitchMaxTransports`, `Stoppable` concept | spdlog |
| `include/eph/net/proxy.hpp` | SOCKS5 + HTTP CONNECT tunneling | `ProxyConfig`, `ProxyType`, `socks5_handshake`, `http_connect_handshake`, `make_proxied_factory`, `parse_proxy_url`, `proxy_type_name` | `socket_transport`, `eph-core::detail::{base64,json_escape,string_checks}`, spdlog |

All headers are installed under the `eph/net/` include prefix. Every
`Config` struct in this table exposes `validate()` (constexpr where
possible), `warnings()`, `dump()`, `to_json()`, and a defaulted
`operator==`. `std::formatter` specializations are provided for
`SocketConfig`, `CircuitState`, `CircuitBreaker`, `CircuitBreaker::Config`,
`ConnHealth`, `Gateway::Config`, `HttpResponse`, `HttpClient::Config`,
`KillSwitch`, `ProxyType`, `ProxyConfig`, `RateLimiter`, and
`RateLimiter::Config`.

---

## Data Flow

### WSS connection via `connect(url, ...)`

```
connect("wss://host/path", modifier, sock_cfg_opt)
  |
  |-- TransportConfig::from_url(url)      (eph-transport)
  |-- modifier(cfg)                       (optional user hook)
  |-- branch on cfg.use_tls:
  |     true  -> socket_wss_connect<MP,QD>(cfg, sock_cfg_opt)
  |     false -> socket_ws_connect<MP,QD>(cfg, sock_cfg_opt)
  v
detail::socket_connect_impl
  |-- SocketConfig sc = sock_cfg.value_or({host, port, tcp_nodelay=true})
  |-- sc.validate() -> early ConnectionErrorInfo{kInvalidConfig, ...}
  |-- build TcpFactory = [sc, timeout] {
  |       auto tcp = make_unique<SocketTransport>(sc);
  |       tcp->connect(timeout);   // DNS (async+timeout), TCP handshake, options
  |       return tcp;
  |     }
  v
Transport<SocketTransport, WsFramer, MaxPayload, QueueDepth>::create(
    factory, cfg)
  |-- factory() once
  |-- [eph-transport] TLS handshake (if cfg.use_tls) via aws-lc
  |-- [eph-transport] WebSocket Upgrade handshake
  |-- [eph-transport] spawn RX thread + optional TX thread
  v
unique_ptr<Transport<...>>  (or std::unexpected<ConnectionErrorInfo>)
```

### RX hot path (SocketTransport::poll_rx<F>)

```
Transport RX thread loops:
  tcp.poll_rx([&](const uint8_t* data, uint16_t len) {
      framer.feed(data, len) -> zero or more frames
      for each frame: queue.push(frame)  (or evict+push)
  })
    |
    |-- if kEnableTimestamps:
    |     recvmsg(MSG_DONTWAIT) with CMSG buffer
    |     on n>0: last_rx_burst_tsc = TSC::now()
    |     extract SCM_TIMESTAMPING cmsg -> rx_stack_histogram
    |     poll_tx_error_queue() -> tx_stack_histogram
    |   else:
    |     recv(MSG_DONTWAIT); last_rx_burst_tsc = TSC::now()
    |
    |-- n>0  -> callback(buf, len); return 1
    |-- n==0 -> state transition (FIN or peer close)
    |-- n<0 && EAGAIN -> return 0
    |-- n<0 other     -> state=Closed; unexpected(error)
```

### Signed REST call (HttpClient + hmac)

```
hmac_sha256_hex(secret, query)  ->  64-char hex
  |
  v
build_http_request("GET", host, "/api/v3/account?...&signature=...",
                   body="", content_type="", extra_headers="")
  |  (rejects extra_headers containing \r\n\r\n or bare LF)
  v
HttpClient::execute(request):
  tcp_connect()    -- async DNS + poll() connect with timeout
  tls_handshake()  -- SSL_CTX + SSL_set1_host (hostname verify) + SNI
  ssl_send_all()
  ssl_recv_all()   -- reads until is_http_response_complete() or peer close
  parse_http_response() -> HttpResponse { status_code, body, headers_raw }
```

---

## Key Components

### `SocketTransport`

**File**: `include/eph/net/socket_transport.hpp`
**Purpose**: Non-blocking POSIX TCP backend satisfying `eph::core::TcpTransport`.
**Interface (public)**:
```cpp
explicit SocketTransport(const SocketConfig&) noexcept;
std::expected<void,    std::string> connect(std::chrono::milliseconds = 3000ms);
std::expected<size_t,  std::string> send(const void* data, size_t len);
template <typename F>
  std::expected<uint16_t, std::string> poll_rx(F&& cb);
template <typename F, typename Rep, typename Period>
  std::expected<uint16_t, std::string> poll_rx_for(F&& cb, duration<Rep, Period>);
std::expected<void,    std::string> close();
void                                 reset() noexcept;
TcpState state() const;  uint16_t mss() const;  bool is_established() const;
int fd() const;  uint16_t local_port() const;
std::string_view resolved_ip() const;
uint64_t dns_latency_ns() const;  uint64_t connect_latency_ns() const;
RttStats rx_latency() const;      RttStats tx_latency() const;
uint64_t last_rx_burst_tsc() const;
#if EPH_ENABLE_TIMESTAMPS
uint64_t last_kernel_rx_delay_ns() const;
uint64_t last_kernel_tx_delay_ns() const;
#endif
```
**Notes**:
- DNS resolution uses `std::async` + `wait_for()` so `getaddrinfo()` cannot
  hang indefinitely; the addrinfo is wrapped in a `shared_ptr<addrinfo,
  freeaddrinfo>` so even on timeout the still-running async thread frees it
  without leaking.
- `send()` enforces a **total** deadline (`config_.send_timeout_ms`) across
  all partial writes, not per-call, so repeated EAGAIN poll waits can't
  accumulate unbounded latency.
- `close()` intentionally keeps the fd open after `SHUT_WR` so the RX path
  can drain pending data and observe the peer's FIN (`recv == 0` from
  `FinWait*`). The fd is finally closed by `poll_rx`'s FIN handling, by
  `reset()`, or by the destructor.
- `reset()` sets `SO_LINGER{1,0}` before `close()` to force an RST instead
  of a graceful FIN.
- `state_` is `std::atomic<TcpState>` with relaxed ordering — it guards
  fast-path checks in `send`/`poll_rx`/`close`, not cross-thread data
  publication.
- `if constexpr (kEnableTimestamps)` turns the RX path into `recvmsg()`
  with a cmsg buffer, extracts `SCM_TIMESTAMPING[0]` (software RX
  timestamp), records the delta in `rx_stack_histogram_`, and polls the
  error queue for TX timestamps. Burst-mode TX uses the **first** send's
  realtime timestamp for all packets in the burst — documented gotcha.
- A compile-time `static_assert(TcpTransport<SocketTransport>)` at the
  bottom of the header enforces concept conformance.

### `SocketConfig`

**File**: `include/eph/net/socket_config.hpp`
**Purpose**: Declarative TCP target + kernel options; URL-parsable.
**Notes**:
- `from_url` accepts `tcp://host:port`, `host:port`, and bracketed IPv6
  (`[::1]:443`). Rejects control characters in hostnames and in
  `bind_device` (both are passed to syscalls + logs).
- `bind_device` requires `CAP_NET_RAW` for `SO_BINDTODEVICE`.
- `kEnableSocketTimestamps` in this header is a **deprecated alias** for
  `kEnableTimestamps` in `transport_types.hpp`; it stays so downstream code
  that already referenced it still compiles.

### `HttpClient` / `http_message`

**File**: `include/eph/net/http_client.hpp`, `http_message.hpp`
**Purpose**: One-shot synchronous HTTP/1.1 REST client for off-hot-path calls
(order placement, balance queries, orderbook snapshots).
**Interface**:
```cpp
class HttpClient {
public:
    struct Config { host; port=443; use_tls=true; timeout=5s; ca_cert_path;
                    max_response_size=8MiB; /* + validate/dump/to_json/from_url/to_url/warnings */ };
    explicit HttpClient(Config);
    std::expected<HttpResponse, std::string> get   (path, extra_headers={});
    std::expected<HttpResponse, std::string> post  (path, body, ct="application/json", extra_headers={});
    std::expected<HttpResponse, std::string> put   (path, body, ct="application/json", extra_headers={});
    std::expected<HttpResponse, std::string> delete_request(path, extra_headers={});
    std::expected<HttpResponse, std::string> request(method, path, body={}, ct={}, extra_headers={});
    const Config& config() const;
    static bool is_response_complete(std::string_view buf);
};
```
Plus the pure free functions in `http_message.hpp`:
```cpp
build_http_request(method, host, path, body, content_type, extra_headers);
parse_http_response(data);
find_header(headers_raw, name);          // case-insensitive, trimmed
find_header_opt(headers_raw, name);      // distinguishes absent vs empty
is_http_response_complete(buf);          // CL + chunked + conn-close
```
And `HttpResponse` has `is_informational/is_success/is_redirect/
is_client_error/is_server_error/is_error/header(name)/has_header(name)`.
**Notes**:
- `build_http_request` **hardened** against header injection: rejects
  `\r\n\r\n` (ends header block early) and rejects bare `\n` without a
  preceding `\r` (some proxies treat lone LF as a line terminator).
- TLS path enables hostname verification via both `SSL_set_tlsext_host_name`
  (SNI) **and** `SSL_set1_host` — SNI alone doesn't verify the cert, it
  only tells the server which one to present.
- `is_http_response_complete` handles `Content-Length` **and** chunked
  transfer encoding, plus a 16 MiB cap for connection-close semantics and
  a 256 MiB ceiling on `Content-Length` to prevent OOM from a malicious
  server.
- Each request opens a fresh connection (`Connection: close` in the
  request) — no keep-alive, no pipelining, no redirect following.

### `RateLimiter`

**File**: `include/eph/net/rate_limiter.hpp`
**Purpose**: Token-bucket throttle for outbound exchange API calls.
**Notes**:
- `rate_per_ns_` is computed once in the ctor (`rate_per_sec / 1e9`) and
  refill is done in nanoseconds against `steady_clock` — avoids repeated
  floating-point division on the hot path.
- `available()` is logically const: `tokens_` and `last_refill_` are
  `mutable` and refill is a cache update, not an observable state change.
- `refill_locked` guards against clock rewind (`now <= last_refill_`)
  even though `steady_clock` is nominally monotonic — defensive coding
  for NTP-adjustment edge cases.
- `acquire()` spins with `std::this_thread::yield()` and a bounded
  timeout — deliberately unsuitable for the hot path; use `try_acquire`
  + explicit backoff there.

### `CircuitBreaker`

**File**: `include/eph/net/circuit_breaker.hpp`
**Purpose**: Stop hammering broken endpoints; fail fast with periodic probing.
**Notes**:
- Transitions: `Closed --(failures >= threshold)--> Open`, `Open --(open_duration
  elapsed, in allow())--> HalfOpen`, `HalfOpen --success--> Closed`,
  `HalfOpen --failure--> Open (timer reset)`.
- `state()` is slightly smart: if it finds the circuit in `Open` and the
  duration has already elapsed, it **reports** `HalfOpen` without actually
  performing the transition — the transition itself happens in `allow()`
  so the caller that's about to make a call is the one that consumes the
  probe slot.
- All mutation is behind `std::mutex`; correctness over hot-path speed.
  The rationale is identical to `RateLimiter` — these aren't on the
  data plane.

### `Gateway`

**File**: `include/eph/net/gateway.hpp`
**Purpose**: Central lifecycle + health monitor for a fleet of transports
across exchanges / symbols.
**Notes**:
- The `GatewayManageable` concept **refines** `Stoppable` so any type
  managed by a `Gateway` is automatically compatible with `KillSwitch`
  (which only needs `Stoppable`). That's a deliberate cross-component
  invariant encoded in the type system.
- `add<Transport>` uses `if constexpr (requires { tp->stats().rx_packets; })`
  to opportunistically capture a lambda that reads `rx_packets` — the
  health checker then uses the delta over `degraded_threshold` to flip a
  connection `Healthy -> Degraded` when traffic stalls without the
  connection actually dropping. Transports that don't expose
  `stats().rx_packets` fall back to `is_running()`-only checks.
- Every callback invocation (`stop_fn`, `is_running_fn`, `rx_packets_fn`,
  `reconnect_fn`, `on_health_change`) is invoked **outside** the mutex;
  this was a deliberate fix for a `dump()` deadlock (commit `fa9bbf9`) —
  any new Gateway method that invokes user callbacks must preserve this
  pattern.
- Health-check loop sleeps in 100 ms increments so `stop_monitor()` exits
  quickly.
- `for_each` is templated rather than using `std::function` to avoid
  per-call heap + virtual overhead in dashboards / metrics exporters.

### `KillSwitch`

**File**: `include/eph/net/kill_switch.hpp`
**Purpose**: Coordinated emergency teardown across all registered transports.
**Notes**:
- Handles are stored in a `std::array<TransportHandle, 32>` — no heap,
  safe to touch from a signal handler context.
- `register_transport` / `unregister_transport` go through a spinlock
  (`std::atomic<bool>`) with `__builtin_ia32_pause` / `yield` relaxation.
  The spinlock is **not** used on the signal path; `request_shutdown()`
  and `is_shutdown_requested()` are lock-free atomic load/store only.
- Signal handler re-registers `SIGINT` to `SIG_DFL` after the first hit
  so a second Ctrl-C triggers a hard kill.
- `shutdown()` snapshots handles under lock then calls `stop_fn` outside
  the lock, catches exceptions per-transport so one failing stop can't
  block the rest.
- `reset()` allows re-using the same KillSwitch after a shutdown cycle —
  intended for test harnesses and long-running supervisors.

### `proxy::{socks5_handshake, http_connect_handshake, make_proxied_factory}`

**File**: `include/eph/net/proxy.hpp`
**Purpose**: Tunnel a `SocketTransport` through SOCKS5 (RFC 1928 + 1929) or
HTTP CONNECT (RFC 7231 §4.3.6) proxies.
**Notes**:
- SOCKS5 sends `ATYP=0x03` (domain name) so **the proxy** resolves the
  target host — no DNS leak from the client.
- `HandshakeIO` buffers bytes beyond the requested `read_exact` length
  inside a small `std::vector` so a single `poll_rx_for` that delivers
  extra bytes doesn't lose them.
- SOCKS5 `BND.ADDR` for domain-type replies larger than 16 bytes is
  drained in 128-byte chunks; smaller replies fit in a stack buffer.
- `password` is **never** logged or included in `dump()` / `to_json()` /
  error messages (`ProxyConfig::dump` prints `pass=<redacted>`). SOCKS5
  and CONNECT error messages also exclude auth material.
- `parse_proxy_url` splits on the **last** `@` so passwords containing
  `@` round-trip correctly (hostnames never contain `@`).
- `make_proxied_factory` validates the `ProxyConfig` once at builder
  time; if invalid, it returns a closure that always fails with the
  cached validation error rather than re-checking on every reconnect.

### `hmac`

**File**: `include/eph/net/hmac.hpp`
**Purpose**: HMAC-SHA256 signing and verification for Binance / Bybit / OKX.
**Notes**:
- Verification uses `CRYPTO_memcmp` (constant-time) — never compare HMAC
  output with `==` or `memcmp` because those leak information through
  early-termination timing side channels.
- `hmac_sha256_base64` exists so callers don't accidentally base64-encode
  the hex string instead of the raw digest.
- Returns `std::unexpected` (not throw) on aws-lc failures, including a
  defensive check that `HMAC` returned exactly 32 bytes.

---

## Entry Points & APIs

| Entry point | Kind | Description |
|---|---|---|
| `eph::net::connect(url, modifier?, sock_cfg?)` | Free function | One-liner — parse URL, validate, create + connect `Transport<SocketTransport, WsFramer, ...>`. Branches on `ws://` vs `wss://`. |
| `eph::net::socket_wss_connect(cfg, sock_cfg?)` | Free function | Build a WSS Transport from an already-built `TransportConfig`. |
| `eph::net::socket_ws_connect(cfg, sock_cfg?)` | Free function | Plain WS version (`use_tls = false`). |
| `eph::net::SocketTransport(const SocketConfig&)` | Class | Use directly when you want a kernel TCP connection **without** the `Transport<>` machinery (e.g. for a custom framer loop). |
| `eph::net::HttpClient(Config)` | Class | Synchronous HTTP/1.1 + TLS REST client. |
| `eph::net::hmac_sha256*` | Free functions | Signing + verification. |
| `eph::net::CircuitBreaker(Config)` | Class | Failure-backoff primitive. |
| `eph::net::RateLimiter(Config)` | Class | Outbound-rate throttle. |
| `eph::net::Gateway(Config)` | Class | Multi-connection lifecycle + health. |
| `eph::net::KillSwitch` | Class | Emergency shutdown coordinator. |
| `eph::net::proxy::make_proxied_factory(...)` | Free function | Inject a proxy-tunneling `TcpFactory` into `Transport::create(...)`. |
| `eph::net::proxy::parse_proxy_url(...)` | Free function | URL -> `ProxyConfig`. |

---

## Dependencies

### Internal (ephemeral_dev monorepo)

```
eph-core        (concepts, length_prefix_framer, json_escape / base64 / string_checks helpers)
  ^
  |      +---- eph-utils      (HdrHistogram, TSC)
  |      |
  |      |     eph-containers (SPSC bounded queue — consumed transitively via Transport)
  |      |         ^
  |      |         |
  |      +---- eph-transport  (Transport<>, DirectTx/Direct variants, TransportConfig,
  |                            WsFramer, RawFramer, transport_types, presets)
  |                 ^
  |                 |
  +---------- eph-net  (this project)  -------------
                      ^
                      |
                sibling: eph-dpdk (kernel-bypass backend, independent)
```

`eph-net`'s `xmake.lua` declares `add_deps("eph-transport", { public = true })`
and `add_packages("spdlog", { public = true })`; everything else is pulled
transitively.

### External

| Package | Purpose |
|---|---|
| **aws-lc** (OpenSSL-compatible) | TLS handshake + encryption in `HttpClient`; `HMAC`, `EVP_EncodeBlock`, `CRYPTO_memcmp` in `hmac.hpp` |
| **spdlog** | Leveled logging, compile-time filtered via `SPDLOG_ACTIVE_LEVEL` |
| **google/benchmark** (bench only) | Benchmark harness (via the shared `eph-bench` rule) |
| **GoogleTest** (test only) | Unit / integration test harness (via the shared `eph-test` rule) |

---

## Testing

`tests/*.cpp` — 15 files, each becoming its own xmake target under the
`eph-test` rule. `benchmarks/*.cpp` — 13 files under `eph-bench`. One
libFuzzer harness: `fuzzers/fuzz_http_parse.cpp`.

| Test file | Coverage focus |
|---|---|
| `test_tcp_concept.cpp` | Static `TcpTransport` concept compliance and witness types |
| `test_socket_transport.cpp` | Non-blocking connect, send, poll_rx, close/reset, MSS query, state machine |
| `test_framer.cpp` | Length-prefix framer boundary + fragmentation |
| `test_http.cpp` | Pure `http_message` builder + parser + header extraction (no network) |
| `test_http_client.cpp` | Full TLS + chunked-encoding + bare-LF injection + `is_response_complete` edge cases |
| `test_tls_record.cpp` | TLS record-layer boundary and error paths |
| `test_hmac.cpp` | HMAC determinism, large messages, hex + base64, constant-time verify, `to_hex` edge cases |
| `test_circuit_breaker.cpp` | All state transitions, `Config` warnings, concurrent `allow` / `record_*` |
| `test_rate_limiter.cpp` | Refill precision, `const`-correctness, `is_exhausted`, burst, mutable refactoring regression |
| `test_gateway.cpp` | Add / remove / reconnect, health transitions, monitor thread, `dump()` deadlock regression |
| `test_kill_switch.cpp` | Capacity limit, register/unregister, `running_count`, formatter, signal-handler flag |
| `test_proxy.cpp` | SOCKS5 phases + auth + edge cases, HTTP CONNECT auth, URL parser |
| `test_websocket.cpp` | WS framer (RFC 6455) with Transport integration |
| `test_transport.cpp` | `Transport<SocketTransport, WsFramer, ...>` end-to-end |
| `test_transport_types.cpp` | `TransportConfig`, `RttStats`, `ConnectionErrorInfo`, etc. |

Key scenarios that the project-wide testing guidance calls out explicitly:

- Boundary + error paths (not just the happy path) — present throughout
  `test_http_client.cpp`, `test_tls_record.cpp`, `test_http.cpp`.
- Integration tests for I/O-heavy code — `test_socket_transport.cpp`,
  `test_transport.cpp`, `test_websocket.cpp` talk to real loopback sockets.
- Regression tests tied to specific commit fixes:
  - `test_gateway.cpp` guards `dump()` against the callback-inside-lock
    deadlock fixed in `fa9bbf9`.
  - `test_rate_limiter.cpp` locks in the `mutable`-based `const` refactor
    from `ec3648d`.
  - `test_http_client.cpp` covers the bare-LF header injection fix from
    `86d5ebe`.
