# eph-net changelog

## [Unreleased]

### Added (2026-04-26 .. 2026-04-28)
- `include/eph/net/reconnect_orchestrator.hpp` — `ReconnectOrchestrator
  <Stream>` (~830 LOC, header-only). Owns the connect → connected →
  disconnected → backoff loop driven by `ReconnectPolicy`. Per-instance
  `alignas(64) std::atomic<uint64_t>` array exposes five
  `ReconnectMetric` counters/gauges
  (`net.reconnect.{count, failures, duration_ns, duration_ns.last,
  subscribe_replay_count}`) read either via `metric(ReconnectMetric)`
  or pushed in bulk via `publish_reconnect_metrics(orch, sink, tags)`
  — same shape as `publish_metrics` for streams. Optional richer event
  hook `on_reconnect_event(ReconnectEventKind, …)` (T2.14) emits typed
  state-transition events alongside the legacy `on_state_change`. The
  `note_subscribe_replay()` accessor (T2.11) is the user-asserted
  signal that a session has finished re-subscribing — pair with
  `kReconnectCount` to detect "reconnects without replay" drift.
  Test coverage: 18 cases in `tests/test_reconnect_orchestrator.cpp`
  (state machine, backoff progression, factory failure paths,
  publish_reconnect_metrics fan-out, event hook ordering, idempotent
  stop).
- `include/eph/net/signed_request.hpp` — `SignedRequest<Traits>` HMAC
  sign-into-header helper. Zero-heap, allocation-free; consumes a
  `HmacSha256Key` and writes the signature header into a
  caller-supplied buffer. The `Traits` parameter pins the venue's
  exact signing string layout (Bybit / OKX / Coinbase header
  positioning). Test coverage: `tests/test_signed_request.cpp`.
- `include/eph/net/jwt_signed_request.hpp` — `build_coinbase_jwt(...)`
  ES256 JWT builder (RFC 7519 + RFC 7515 `ES256` over P-256). Used by
  the Coinbase venue adapter for the new "Sign In With X" auth flow
  that replaced HMAC for retail exchange API access. Returns a
  `std::expected<std::string, ErrorInfo>`; aws-lc backs the EC sign +
  IEEE-P1363 → ASN.1 conversion. Tests verify against an in-process
  ES256 verifier; integration test exercises the full produced JWT
  against a mock Coinbase server (`tests/integration/test_coinbase_adapter.cpp`).

### Documentation
- Re-synced `README.md`, `summary.md`, `docs/ONBOARDING.md` against the
  actual header set under `include/eph/net/` (2026-04-24). Previous
  revisions still listed pre-Phase-9 `detail/http_request.hpp` /
  `http_response.hpp` (now replaced by the top-level `http.hpp`
  parser + `detail/ws_handshake.hpp` + `detail/http_connect.hpp`), and
  omitted `hmac.hpp`, `proxy.hpp`, `stream_metrics.hpp`,
  `posix_io.hpp`, `posix_listener.hpp`, and the split-out TLS
  primitives (`tls_record.hpp`, `tls_decryptor.hpp`,
  `tls_encryptor.hpp`, `tls_inplace.hpp`, `tls_constants.hpp`). The
  concept signatures shown in `summary.md` were also tightened to
  match the current `noexcept` / `PollerOf` shape.

## Phase 9 Recovery (2026-04-10)

### Added
- `include/eph/net/http.hpp` — incremental HTTP/1.1 parser subset.
  `parse_http_request` / `parse_http_response` return
  `std::expected<std::optional<ParseResult<T>>, ErrorInfo>`: `nullopt`
  means "need more bytes", value means "complete message parsed". Caller
  provides header storage (`std::span<HttpHeader>`), so the parser is
  zero-heap. `build_http_request` / `build_http_response` round-trip
  into a caller-owned buffer. HFT-pragmatic subset: rejects
  `Transfer-Encoding` / chunked / cookies / redirect /
  `Expect: 100-continue` / conflicting `Content-Length` / bare LF /
  CRLF+NUL injection with `Error::CodecBad` (see scope decision D-1 and
  `.artifacts/phase-9-scope-decision.md`).
- `include/eph/net/hmac.hpp` — typed HMAC-SHA256 with `HmacSha256Key`
  and `HmacSha256Tag` wrappers. The key is normalised per RFC 2104 into
  an `alignas(64)` 64-byte buffer and cleared by the destructor via
  `OPENSSL_cleanse` (RAII, non-copyable, noexcept-movable). One-shot
  `hmac_sha256_sign(key, msg)` is zero-alloc and `noexcept`;
  `tag.to_hex(span<uint8_t, 64>)` is zero-alloc lowercase hex. aws-lc
  backs the primitive.
- `include/eph/net/proxy.hpp` + `include/eph/net/detail/http_connect.hpp`
  — HTTP CONNECT proxy support. `StreamConfig::proxy` is honoured by
  the kernel backend; `ProxyConfig::validate()` enforces non-empty
  host, non-zero port, XOR'd basic-auth fields, and positive timeout.
  The DPDK backend rejects any non-empty `proxy` with
  `Error::InvalidConfig` because there is no kernel TCP path available
  for the CONNECT tunnel.
- `include/eph/net/detail/ws_handshake.hpp` — RFC 6455 client
  handshake over a generic ByteSink. Called transparently by the
  kernel / DPDK backends inside `TcpStream::create()` when
  `StreamConfig::ws_path` is set (decision D-2: config-driven over a
  separate `connect_async`-style entry point).
- `include/eph/net/stream_metrics.hpp` — `StreamMetric` enum +
  `kStreamMetricNames` (OTel `net.stream.*`) + `publish_metrics` reader
  that forwards counters into any `eph::core::MetricsSink`.
- `include/eph/net/posix_io.hpp`, `include/eph/net/posix_listener.hpp`
  — server-side `send_all` / `recv_exact` / `tcp_bind_listen` / UDP
  bind / poll-based accept helpers (`eph::net::posix`), promoted out of
  the benchmark tree so tests no longer reverse-include benchmarks.
- TLS in-place primitives split out of `detail/tls_session.hpp`:
  `tls_record.hpp`, `tls_decryptor.hpp`, `tls_encryptor.hpp`,
  `tls_inplace.hpp`, `tls_constants.hpp`. This lets the DPDK backend
  decrypt AES-GCM straight into the mbuf (zero-copy `PacketView`)
  without pulling the full session state machine.
- Three new `eph::core::Error` values are consumed here:
  `ProxyConnectFailed`, `ProxyHandshakeFailed`, `ProxyAuthRequired`.

## v3.3 (2026-04-10) — module repurposed as the narrow-waist

Pre-v3.3, `eph-net` was the POSIX socket backend (with `SocketTransport`, HTTP
client, gateway, circuit breaker, rate limiter, etc.). v3.3 moved all backend code
to `eph-net-kernel` and repurposed `eph-net` as the concept + shared-types module.

### Added
- `include/eph/net/concepts.hpp` — `Pollable` / `Stream` / `Datagram` / `Poller`
  concepts. The v3.3 replacement for `eph::net::TcpTransport`.
- `include/eph/net/test/fake_stream.hpp`, `test/fake_datagram.hpp`,
  `test/test_poller.hpp` — in-memory test mocks. Replaces the legacy
  `eph::core::FakeTcpTransport`.
- `include/eph/net/detail/tls_session.hpp` — v3.3 TLS session wrapper. Shared by
  kernel and DPDK backends.
- `include/eph/net/detail/websocket.hpp` — RFC 6455 wire helpers.
- `include/eph/net/detail/http_request.hpp`, `http_response.hpp` — minimal
  HTTP/1.1 parser for the WS upgrade handshake.

### Moved (v3.3 Phase 3 → eph-net-kernel)
- `socket_transport.hpp`, `socket_config.hpp`, `socket_connect.hpp`
- `gateway.hpp`, `http_client.hpp`, `http_message.hpp`
- `circuit_breaker.hpp`, `kill_switch.hpp`, `rate_limiter.hpp`
- `proxy.hpp`, `hmac.hpp`
- `net.hpp` umbrella header

Everything moved ended up either inlined into `KernelTcpStream` or deleted — see
commit `c2a0ca4` (Phase 7) for the full list.

### Removed
- `posix_io.hpp` / `posix_listener.hpp` internal detail headers — absorbed into
  `eph-net-kernel/include/eph/net/kernel/detail/`.

### Retained
- `socket_addr.hpp`, `reconnect_policy.hpp`, `tcp_state.hpp` — unchanged. Both
  kernel and DPDK backends use them.
