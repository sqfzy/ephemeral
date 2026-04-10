# eph-net changelog

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
