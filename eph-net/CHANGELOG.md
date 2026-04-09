# Changelog

All notable changes to `eph-net` are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This project has not yet cut a tagged release; everything since the project's
inception lives under `[Unreleased]`.

## [Unreleased]

### Added

#### HTTP client and messages

- `HttpClient`: `put()` and `delete_request()` convenience methods plus a
  generic `request(method, path, body, content_type, extra_headers)` escape
  hatch for `PATCH`, `HEAD`, and similar — match the feature surface of
  popular REST client libraries so you don't need to reach for raw sockets
  for less-common verbs.
- `HttpClient::Config::from_url()` / `to_url()` so REST endpoints can be
  supplied as a URL string instead of manually assembling host + port +
  `use_tls`.
- `HttpClient::is_response_complete()` (and a free `is_http_response_complete()`
  in `http_message.hpp`) that understands `Content-Length`, chunked transfer
  encoding, and connection-close semantics, so partial reads can be detected
  without guessing.
- `HttpResponse::is_informational()`, `is_redirect()`, `is_client_error()`,
  `is_server_error()`, and `is_error()` — complete the status-classification
  helpers.
- `HttpResponse::header(name)` / `has_header(name)` for case-insensitive
  header lookup directly on the response object.
- `HttpResponse::to_json()` for monitoring integration (body is truncated
  and escaped to prevent log injection or oversized JSON).
- `find_header_opt()` alongside `find_header()` so callers can distinguish
  "header absent" from "header present with empty value".

#### HMAC

- `hmac_sha256_base64()` and `hmac_verify_base64()` — OKX expects base64
  signatures, and a dedicated API prevents the common bug of
  base64-encoding the **hex** digest instead of the raw bytes.
- Full logging of HMAC compute / verify paths at DEBUG / TRACE so failing
  auth is diagnosable without attaching a debugger.

#### Circuit breaker, rate limiter, kill switch

- `CircuitBreaker::is_tripped()`, `CircuitBreaker::time_until_half_open()`,
  `CircuitBreaker::Config` with `dump()` / `to_json()` / `warnings()` /
  `operator==`, and a defaulted-parameter `Config{}` so unconfigured
  breakers have sensible production defaults.
- `CircuitBreaker::dump()` / `to_json()` for live-state monitoring
  dashboards.
- `RateLimiter::Config` with the same validation / warnings / JSON
  treatment and a two-argument constructor that delegates to the
  config-based one.
- `RateLimiter::is_exhausted()`, `RateLimiter::available()` (now logically
  `const` via a mutable refill cache), `dump()`, and `to_json()`.
- `KillSwitch::running_count()`, `reset()`, `dump()`, `to_json()`, and a
  `std::formatter` specialization.
- `Stoppable` concept that constrains `KillSwitch::register_transport` and
  `unregister_transport`, and `GatewayManageable` concept that refines
  `Stoppable` with `start_threads()` / `reconnect_now()` so any
  Gateway-managed transport is guaranteed to also be KillSwitch-compatible.

#### Gateway

- Multi-connection `Gateway` with a background health monitor thread:
  `add()` / `remove()` / `remove_by_tag()`, `start_all()` / `stop_all()`,
  `reconnect()` / `reconnect_by_tag()`, `find_by_tag()`, `healthy_count()`,
  `is_all_healthy()`, `is_any_disconnected()`, `connection_tags()`,
  `for_each()` (templated — no `std::function` overhead), `check_health()`,
  `dump()` / `to_json()`, and a `Config::on_health_change` callback for
  driving alerting systems.
- Opportunistic `Degraded` detection: when a managed transport exposes
  `stats().rx_packets`, the health checker compares the current value to
  the previous check and marks the connection degraded if the delta stays
  zero for longer than `Config::degraded_threshold`.

#### Socket transport and configuration

- `SocketConfig::validate()`, `warnings()`, `dump()`, `to_json()`, defaulted
  `operator==`, and `std::formatter` specialization.
- `SocketConfig::from_url()` / `to_url()` supporting bracketed IPv6
  (`[::1]:443`) and the `tcp://` scheme.
- Optional `SO_TIMESTAMPING`-based kernel RX/TX latency histograms in
  `SocketTransport` (opt-in via `-DEPH_ENABLE_TIMESTAMPS=1`), plus
  per-call `last_kernel_rx_delay_ns()` / `last_kernel_tx_delay_ns()` and
  an always-available `last_rx_burst_tsc()` for pipeline latency anchoring.
- DNS resolution with a hard wall-clock timeout via `std::async` +
  `wait_for()`, using a `shared_ptr<addrinfo, freeaddrinfo>` so the
  still-running async thread frees the struct even if the caller times
  out — no leak, no dangling pointer.
- `SocketConfig::bind_device` (SO_BINDTODEVICE) for pinning traffic to a
  specific NIC; validated for control characters to prevent log injection.
- `SocketTransport` TCP keepalive support (`tcp_keepalive`, `keepalive_idle`,
  `keepalive_interval`, `keepalive_count`).
- One-liner `connect(url, modifier?, sock_cfg?)` that parses the URL,
  applies optional user-supplied config edits, validates, and returns a
  ready-to-use `Transport<SocketTransport, WsFramer, ...>`.
- Preset type aliases: `SocketWssTransport`, `SocketWssSmallTransport`,
  `SocketWssLargeTransport`, `SocketWssEvictTransport`, `SocketWsTransport`,
  `SocketRawTransport`, plus `SocketDirectTx*Transport` (RX thread only,
  app sends on caller thread) and `SocketDirect*Transport` (no background
  threads at all) variants.

#### Proxy

- SOCKS5 (RFC 1928 + 1929) and HTTP CONNECT (RFC 7231 §4.3.6) tunneling:
  `ProxyConfig`, `socks5_handshake()`, `http_connect_handshake()`,
  `make_proxied_factory()`, `parse_proxy_url()`, and `ProxyType` with
  `dump()` / `to_json()` / `warnings()` / `operator==` / formatter.
- SOCKS5 uses `ATYP=0x03` (domain name) so DNS resolution happens on the
  proxy side — no DNS leak from the client.

### Changed

- `http_client.hpp` split: pure HTTP value types (`HttpResponse`),
  `build_http_request()`, `parse_http_response()`, and the
  response-completion predicate moved to `http_message.hpp`. This removes
  the OpenSSL / POSIX dependency from the pure-logic side so unit tests
  can build without linking aws-lc.
- `RateLimiter::available()`, `is_exhausted()`, and `dump()` are now
  `const`. The refill cache (`tokens_`, `last_refill_`) is `mutable`
  because refill is a logically-const cache update, not an observable
  state change.
- `Gateway::for_each()` is a template now — previously it used
  `std::function`, adding heap allocation and an indirect call per
  iteration, which hurt dashboards that scan all connections per tick.
- `find_header()` is implemented on top of `find_header_opt()` (single
  source of truth, and uses `std::ranges::equal` + case-insensitive
  projection instead of a hand-rolled comparison).
- `SocketTransport::connect()` DNS hints standardized with
  `AI_ADDRCONFIG` so only locally-reachable address families are returned.
- `Gateway::dump()`, `Gateway::stop_all()`, `Gateway::start_all()`, and
  `Gateway::reconnect()` call user callbacks **outside** the mutex — a
  pattern every new Gateway method must preserve.
- `Gateway::stop_all()` / `start_all()` match connections by tag rather
  than by index, so concurrent `remove()` calls between the snapshot and
  the update phase can't corrupt the health state.
- `build_http_request()` rejects `extra_headers` that contain `\r\n\r\n`
  (would terminate the header block early) or a bare `\n` not preceded by
  `\r` (some proxies treat lone LF as a line terminator).
- `parse_proxy_url()` splits on the **last** `@` so passwords containing
  `@` survive the round-trip.
- SOCKS5 and HTTP CONNECT error messages never include the auth material.
  `ProxyConfig::dump()` / `to_json()` redact the password field.
- All `Config::to_json()` implementations now escape string fields per
  RFC 8259 §7 to prevent malformed JSON from hostnames, tags, or CA paths
  containing quotes, backslashes, or control characters.
- Token refill in `RateLimiter` uses a single floating-point
  multiplication in nanosecond space (`rate_per_ns_` is cached in the
  constructor).

### Fixed

- **Gateway::dump() deadlock**: earlier versions called
  `is_running_fn()` under the Gateway mutex. If a transport's
  `is_running()` ever called back into Gateway (even transitively), this
  deadlocked. Callbacks are now invoked outside the lock, with
  regression tests locking the behavior in.
- **Gateway::start_all() index/pointer races**: identify connections by
  tag, not index, between the snapshot and the health-update phase.
- **Gateway noexcept contracts**: `stop_all()` / `start_all()` /
  `reconnect*()` catch per-transport exceptions and log-and-continue
  instead of propagating through `noexcept` and terminating.
- **RateLimiter const-correctness**: `available()` and related query
  methods are const now (see "Changed").
- **Bare-LF header injection**: `build_http_request()` rejects lone `\n`
  in user-supplied headers.
- **HttpResponse::to_json() injection**: body preview is JSON-escaped.
- **HTTP response completeness** handles chunked transfer encoding in
  addition to `Content-Length` and connection close; also rejects
  `Content-Length > 256 MiB` to prevent OOM from a malicious server.
- **TLS hostname verification**: the HttpClient now calls `SSL_set1_host()`
  in addition to `SSL_set_tlsext_host_name()`. SNI alone only selects the
  cert to present; it does **not** cause the client to verify the cert
  matches the target hostname.
- **Proxy URL empty host**: `parse_proxy_url()` rejects URLs with an
  empty host component.
- **Proxy URL `@` in password**: `parse_proxy_url()` splits on the last
  `@`, handling passwords that contain `@`.
- **`SO_BINDTODEVICE` control-char injection**: `SocketConfig::validate()`
  rejects control characters in `bind_device`.
- **`SocketConfig::dump()` / `to_json()`**: include `bind_device` (was
  previously omitted).
- **SocketTransport close/reset state machine**: `close()` keeps the fd
  open so the RX loop can drain the peer's FIN; `reset()` uses
  `SO_LINGER{1, 0}` to actually send an RST.
- **SocketTransport `setsockopt` / `getsockopt`**: return values are
  checked and logged at WARN (previously silently ignored).
- **WebSocket close handshake race**: fixed (pre-dates current source
  layout; see commit `5cce249`).
- **`SCM_TIMESTAMPING` delay attribution**: burst-mode TX reuses the
  first send's realtime timestamp for all packets in the burst —
  documented inline as a known attribution quirk.
- **`to_hex()` throughput**: rewritten to be roughly 4× faster (cached
  hex character table, avoids `std::format` per byte).
- **`Gateway::to_json()`**: escape the tag field per RFC 8259 to prevent
  malformed JSON.
- **`HttpClient::Config::to_json()`** and **`ProxyConfig::to_json()`**:
  escape string fields for the same reason.

### Removed

- **`kEnableSocketTimestamps`** is now a `[[deprecated]]` alias for
  `kEnableTimestamps` from `transport_types.hpp`. It is retained only so
  existing downstream code compiles — prefer `kEnableTimestamps` in new
  code.

### Infrastructure

- Build reorganization: `eph-net` is its own xmake sub-project
  (`set_kind("headeronly")`) with per-file test and benchmark targets
  generated from `os.files("tests/**.cpp")` and `os.files("benchmarks/**.cpp")`.
- `fuzzers/fuzz_http_parse.cpp` libFuzzer harness for the HTTP parser.
- Benchmark binaries: `bench_socket_config`, `bench_hmac`,
  `bench_circuit_breaker`, `bench_rate_limiter`, `bench_gateway`,
  `bench_kill_switch`, `bench_http_client`, `bench_proxy`, `bench_ws`,
  `bench_tls`, `bench_rx_pipeline`, `bench_transport_pipeline`,
  `bench_control_plane`.
