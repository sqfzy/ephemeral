# eph-net changelog

## [Unreleased] — Phase 9 Recovery (2026-04-10)

### Added
- `include/eph/net/http.hpp` — incremental HTTP/1.1 parser subset.
  `parse_http_request` / `parse_http_response` return
  `std::expected<std::optional<ParseResult<T>>, ErrorInfo>`: `nullopt`
  means "need more bytes", value means "complete message parsed". Caller
  provides header storage (`std::span<HeaderField>`), so the parser is
  zero-heap. HFT-pragmatic subset: rejects `Transfer-Encoding` /
  chunked / cookies / redirect / `Expect: 100-continue` with
  `Error::CodecBad` (see scope decision D-1 and
  `.artifacts/phase-9-scope-decision.md`).
- `include/eph/net/hmac.hpp` — typed HMAC-SHA256 with `Key` and `Tag`
  wrappers. `Key` clears its material in the destructor (RAII); one-shot
  `sign(key, msg)` is zero-allocation. aws-lc backs the primitive.
- `include/eph/net/proxy.hpp` + `include/eph/net/detail/http_connect.hpp`
  — HTTP CONNECT proxy support. `StreamConfig::proxy` is honoured by
  the kernel backend; `parse_proxy_url` reads
  `http://[user:pass@]host:port`. The DPDK backend rejects any
  non-empty `proxy` with `Error::InvalidConfig` because there is no
  kernel TCP path available for the CONNECT tunnel.
- `include/eph/net/detail/ws_handshake.hpp` — RFC 6455 client
  handshake. Called transparently by the kernel / DPDK backends inside
  `TcpStream::create()` when `StreamConfig::ws_path` is set (decision
  D-2: config-driven over `connect_async`-style separate entrypoint).
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
