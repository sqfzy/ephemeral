# eph-net onboarding

## What's in this module

`eph-net` is the narrow-waist between codecs and networking backends.
It contains only **concepts**, **pure value types**, **pure-compute
primitives**, and **test mocks** — no I/O implementation of its own.
The two backends (`eph-net-kernel`, `eph-net-dpdk`) instantiate the
concepts; application code templates on them.

Public surface, grouped:

- Concepts: `Pollable`, `Stream`, `Datagram`, `Poller` (+ the finer
  `PollerOf<T, Obj>` refinement).
- Value types: `Ipv4Addr`, `SocketAddr`, `TcpState`, `ReconnectPolicy`.
- HTTP/1.1: `parse_http_request` / `parse_http_response` /
  `build_http_request` / `build_http_response` (incremental, zero-heap).
- HMAC-SHA256: `HmacSha256Key` (RAII, zero-on-destroy), `HmacSha256Tag`,
  `hmac_sha256_sign`.
- HTTP CONNECT proxy: `ProxyConfig` (validated, kernel-backend only).
- Metrics: `StreamMetric` enum, `kStreamMetricNames`, `publish_metrics`.
- POSIX server helpers (`eph::net::posix`): `send_all`, `recv_exact`,
  `tcp_bind_listen`, UDP bind, poll-based accept. Server-side only;
  used by mocks and bench fixtures.
- Test mocks (`eph::net::test`): `FakeStream`, `FakeDatagram`,
  `TestPoller<P>`.
- Shared wire detail (`eph::net::detail`): `TlsSession`, the TLS
  record / encrypt / decrypt / in-place primitives, WebSocket framing,
  WS-handshake helper, HTTP CONNECT helper.

## How to read the code

1. `include/eph/net/concepts.hpp` — the four concepts. Start here.
2. `include/eph/net/socket_addr.hpp` — trivial value type.
3. `include/eph/net/reconnect_policy.hpp` — how reconnection backoff is
   computed.
4. `include/eph/net/test/fake_stream.hpp` — the simplest `Stream`
   implementation. Read it to internalise what "satisfies the concept"
   means.
5. `include/eph/net/test/test_poller.hpp` — the simplest `Poller`.
   Drives registered pollables synchronously.
6. `include/eph/net/http.hpp` — only if you're touching the HTTP
   parser / builder or the WS-upgrade response path.
7. `include/eph/net/hmac.hpp` — only if you're signing exchange REST
   requests.
8. `include/eph/net/stream_metrics.hpp` — only if you're adding a new
   metric or wiring a sink.
9. `include/eph/net/detail/tls_session.hpp` — only if you're working on
   the TLS path. The supporting primitives (`tls_record.hpp`,
   `tls_decryptor.hpp`, `tls_encryptor.hpp`, `tls_inplace.hpp`,
   `tls_constants.hpp`) exist so the DPDK backend can reach into the
   in-place AES-GCM decrypt without pulling the full session.
10. `include/eph/net/detail/ws_handshake.hpp` and
    `include/eph/net/detail/http_connect.hpp` — the wire handshakes
    the backends invoke transparently from `TcpStream::create()` when
    `StreamConfig::ws.path` / `StreamConfig::proxy` are set.

## Running the tests

```bash
xmake build -g tests
xmake run test_fake_stream
xmake run test_net_reconnect_policy
xmake run test_socket_addr
xmake run test_http
xmake run test_hmac
xmake run test_tls_record
xmake run test_tls_in_place_decrypt
xmake run test_ws_handshake
xmake run test_http_connect
```

Per-file targets are auto-globbed from `tests/test_*.cpp`.

Notable test categories under `tests/`:

- Concepts / mocks: `test_concepts`, `test_fake_stream`,
  `test_fake_datagram`, `test_test_poller`.
- Value types: `test_socket_addr`, `test_tcp_state`,
  `test_net_reconnect_policy`.
- HTTP parser poison pills: `test_http`, `test_http_client`,
  `test_http_connect`, `test_http_cl_te_injection`,
  `test_http_crlf_injection`, `test_http_te_edge`,
  `test_http_whitespace`, `test_ws_upgrade_injection`,
  `test_proxy_url`.
- TLS: `test_tls_record`, `test_tls_config`,
  `test_tls_in_place_decrypt`, `test_tls_no_close_notify_after_extract`.
- WS wire / handshake: `test_websocket_wire`, `test_ws_handshake`.
- POSIX server helpers: `test_posix_io`, `test_posix_listener`.

## Common tasks

### I want to test my application with a fake stream

Use `FakeStream` + `TestPoller`. See `test/fake_stream.hpp` for the full
API.

```cpp
#include "eph/net/test/fake_stream.hpp"
#include "eph/net/test/test_poller.hpp"

namespace ent = eph::net::test;

auto poller = ent::TestPoller<ent::FakeStream>::create();
auto fake   = ent::FakeStream::create();
poller->add(fake.get());
fake->on_message = [&](const uint8_t* d, uint16_t n) { /* ... */ };
fake->inject_rx(bytes);
poller->poll();
```

### I want to sign an exchange REST request

```cpp
#include "eph/net/hmac.hpp"

using namespace eph::net;

// Once per API secret: normalise + RAII-own the key bytes.
HmacSha256Key key{std::string_view{api_secret}};

// Per request: one-shot, zero-alloc, noexcept.
HmacSha256Tag tag = hmac_sha256_sign(key, canonical_query);

// Zero-alloc hex into a caller-supplied 64-byte buffer (hot-path style).
uint8_t hex[64];
tag.to_hex(std::span<uint8_t, 64>{hex});
```

The allocating `tag.to_hex()` overload exists for convenience; do not
call it on the hot path.

### I want to parse an HTTP response without allocating

```cpp
#include "eph/net/http.hpp"

using namespace eph::net;

HttpHeader hdrs[32];
auto r = parse_http_response(rx_buffer, hdrs);

if (!r)            { /* protocol violation — close the connection */ }
else if (!*r)      { /* incomplete — wait for more bytes */ }
else {
    auto& pr = **r;
    // pr.value.headers alias rx_buffer + hdrs; valid until rx_buffer moves
    drain_front(rx_buffer, pr.consumed);
}
```

### I want to route a TCP stream through an HTTP CONNECT proxy

Set `StreamConfig::proxy` on `KernelTcpStream::create()`. The kernel
backend dials the proxy, runs the CONNECT handshake (optionally with
`Proxy-Authorization: Basic base64(user:pass)`), and if TLS / WS are
also configured, continues from the tunnelled endpoint.

```cpp
#include "eph/net/proxy.hpp"
#include "eph/net/kernel/stream_config.hpp"

eph::net::kernel::StreamConfig cfg{ ... };
cfg.proxy = eph::net::ProxyConfig{
    .host = "proxy.example.com",
    .port = 3128,
    .basic_auth_user = std::string{"alice"},
    .basic_auth_pass = std::string{"s3cret"},
};
// cfg.proxy->validate() is called inside KernelTcpStream::create().
```

DPDK rejects a non-empty `proxy` with `Error::InvalidConfig`.

### I want to add a new test mock

Drop a new header under `include/eph/net/test/`. It must satisfy
`Stream` or `Datagram`. Add a unit test under `tests/` that proves
concept satisfaction via `static_assert(eph::net::Stream<MyMock>)`.

### I want to expose a new counter through `publish_metrics`

1. Add an entry to `StreamMetric` in
   `include/eph/net/stream_metrics.hpp` (before `kCount`).
2. Add the matching OTel-style name to `kStreamMetricNames` at the
   same index (the `static_assert` at the bottom of the header catches
   enum-vs-table drift).
3. In whichever backend owns the counter, wire `inc_<NewMetric>()` into
   the hot path and make the new index readable via
   `metric(StreamMetric)`.

### I want to use my own TLS implementation

Don't — but if you must, look at `detail/tls_session.hpp` for the
adapter contract. `TlsSession` is templated on a byte-socket type the
backend supplies (`KernelByteSocket` in `eph-net-kernel`,
`ByteSocketTcpAdapter` around `DpdkTcpSession` in `eph-net-dpdk`). The
supporting primitives in `tls_record.hpp` / `tls_decryptor.hpp` /
`tls_encryptor.hpp` / `tls_inplace.hpp` / `tls_constants.hpp` can be
re-used independently by a DPDK-style zero-copy adapter.

## See also

- `README.md` — module overview
- `summary.md` — full public API surface
- `CHANGELOG.md` — change history
- `../docs/architecture.md` — whole-project concept model
- `../docs/poller-guide.md` — using `Poller` in application code
- `../.artifacts/phase-9-scope-decision.md` — rationale for the HTTP
  parser / proxy / WS feature scope
