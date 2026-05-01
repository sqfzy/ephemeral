# eph-net

Header-only C++23 module defining the networking concepts and shared types
used by every backend in the `eph` stack. `eph-net` is the **narrow waist**
between codecs (`eph-codec`, `eph-fix`, `eph-itch`, `eph-json`, `eph-book`)
and the two sibling backend modules (`eph-net-kernel`, `eph-net-dpdk`). It
contains **no I/O implementation of its own** — every public type here is
either a concept, a pure value type, a pure-compute primitive (HTTP parser,
HMAC), or an in-memory test mock.

## What lives here

### Concepts (`include/eph/net/concepts.hpp`)

The four narrow concepts the backends satisfy:

- `Pollable<T>` — anything a `Poller` can drive. Requires an associated
  `PacketView` type and `poll_once_()` / `is_attached_()` / `native_handle()`
  (all `noexcept`).
- `Stream<T>` — TCP-style connection. Refines `Pollable` with `send()`,
  `close_gracefully()`, `state()`, and an `on_message` callback.
- `Datagram<T>` — UDP-style socket. Refines `Pollable` with `send_to()`,
  `join_multicast()` / `leave_multicast()`, and an `on_datagram` callback.
- `Poller<T>` — the I/O driver: `poll()` (required). The finer
  `PollerOf<T, Obj>` concept additionally requires `add(Obj*)` /
  `remove(Obj*)` for a specific Pollable type.

Also provides `saturate_u16()` / `saturate_u16_clamps()` helpers that clamp
frame lengths into the historical `uint16_t` callback signature.

### Value types

- `Ipv4Addr` / `SocketAddr` (`include/eph/net/socket_addr.hpp`) —
  `constexpr`-friendly IPv4 address with strict `parse()` that rejects
  legacy `inet_aton`-style short forms.
- `TcpState` (`include/eph/net/tcp_state.hpp`, re-exported from
  `eph/core/tcp_state.hpp`) — RFC 793 connection states.
- `ReconnectPolicy` / `ReconnectPolicyConfig`
  (`include/eph/net/reconnect_policy.hpp`) — exponential backoff with
  ±jitter. Owned by `KernelTcpStream` and `DpdkTcpStream`.

### Shared sub-configs and post-create snapshot

The two `StreamConfig`s (kernel + DPDK) embed these by value so a single
field cluster works against both backends:

- `WsConfig` (`include/eph/net/ws_config.hpp`) — `path` / `host` /
  `extra_headers` / `timeout` / `permessage_deflate`. Non-empty `path`
  triggers an RFC 6455 client handshake transparently inside
  `TcpStream::create()`. `extra_headers` views must outlive the
  `create()` call.
- `KeepaliveConfig` (`include/eph/net/keepalive_config.hpp`) —
  `interval` / `probes`. Default disabled. Kernel lowers to
  `setsockopt(SO_KEEPALIVE / TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT)`;
  DPDK lowers to `cfg.dpdk.tcp_low_level.keepalive_*` for the PMD's
  `TcpSession::tick_keepalive`.
- `StreamSnapshot` (`include/eph/net/stream_snapshot.hpp`) — read
  counterpart of `StreamConfig`. Returned by `Stream::snapshot()` /
  `Datagram::snapshot()` on every backend (kernel + DPDK, TCP + UDP).
  Sub-struct names mirror `StreamConfig` (`Tls` / `Ws` / `Keepalive` /
  `Dpdk`) so reading and writing share a vocabulary.

### HTTP client (`include/eph/net/http_client.hpp`)

`HttpClient<S>` is a backend-agnostic, single-request-at-a-time HTTP/1.1
client templated on any `eph::net::Stream`. Builds requests via
`build_http_request`, incrementally parses replies via
`parse_http_response` (so `Transfer-Encoding` / chunked / cookies /
redirect / `Expect: 100-continue` are all rejected — see
`.artifacts/phase-9-scope-decision.md` §D-1). Used by the JWT signing
adapters and exchange REST examples.

### Signed-request helper (`include/eph/net/signed_request.hpp`)

Venue-traits-driven HMAC `sign_into_headers(...)` helper — wraps
`HmacSha256Key` + `hmac_sha256_sign` with the per-venue conventions
(what gets signed, hex vs base64 tag rendering, header names) for
Binance / OKX / Bybit / Coinbase. Plug a venue trait into your
adapter; the helper writes the auth headers without per-venue glue
code in user space.

### Reconnect orchestrator (`include/eph/net/reconnect_orchestrator.hpp`)

`ReconnectOrchestrator<S>` composes `ReconnectPolicy` with
user-supplied callbacks (factory / on_disconnect / on_reconnect /
attach / detach) into the canonical multi-venue reconnect loop —
`disconnect_detected → backoff → factory → attach → resubscribe →
run`. Exposes 4 metrics (attempts / successes / failures /
last_backoff_ms). Backend-agnostic; works with both `KernelTcpStream`
and `DpdkTcpStream`.

### HTTP/1.1 parser subset (`include/eph/net/http.hpp`)

Incremental, zero-heap HTTP/1.1 parser for exchange REST and WebSocket
upgrade handshakes:

- `parse_http_request` / `parse_http_response` return
  `std::expected<std::optional<ParseResult<T>>, core::ErrorInfo>` —
  `nullopt` means "need more bytes", value means "complete message parsed".
- `build_http_request` / `build_http_response` write into a caller-owned
  buffer, no allocation.
- Headers land in a caller-supplied `std::span<HttpHeader>`; return views
  alias the input buffer.
- **Deliberate subset**: rejects `Transfer-Encoding` (including chunked),
  `Content-Length + Transfer-Encoding` combinations, multiple conflicting
  `Content-Length` values, non-3-digit status codes, bare LF line
  terminators, CR/LF/NUL injection in builder inputs — all with
  `Error::CodecBad`. Hard caps: `kMaxHeaderCount=64`,
  `kMaxStartLineLength=8192`, `kMaxHeaderLineLength=8192`,
  `kMaxBodySize=16 MiB`.

### HMAC-SHA256 (`include/eph/net/hmac.hpp`)

Typed primitive for exchange REST request signing (Binance / Bybit / OKX /
…) backed by aws-lc:

- `HmacSha256Key` — RAII wrapper around the 64-byte normalised key;
  non-copyable, movable; destructor runs `OPENSSL_cleanse` so the secret
  cannot linger in stack memory.
- `HmacSha256Tag` — typed 32-byte MAC with `to_hex(span<uint8_t, 64>)`
  zero-alloc hex encoding (lowercase, matches Binance/Bybit wire
  convention).
- `hmac_sha256_sign(key, msg)` — one-shot, `noexcept`, zero-alloc signing
  over `span<const uint8_t>` or `string_view`.

### ES256 JWT signing (`include/eph/net/jwt_signed_request.hpp`)

Coinbase Advanced Trade is the one HFT venue we ship support for that
requires asymmetric (ECDSA P-256) authentication rather than HMAC. This
header provides the JWT envelope:

- `Es256PrivateKey` — RAII wrapper over an `EVP_PKEY*` holding a P-256
  private key; loaded from PEM (PKCS#8 or legacy SEC1) via `from_pem()`.
  Move-only. Validates that the loaded key is actually EC P-256 — RSA /
  Ed25519 / P-384 are rejected with `Error::InvalidConfig`.
- `CoinbaseJwtParams { key_id, api_key_name, method, uri, now_unix_secs,
  ttl_secs, nonce_override }` — the fields Coinbase requires for the
  `kid` / `sub` / `iss` / `nbf` / `exp` / `uri` claims. `nonce_override`
  defaults to fresh CSPRNG bytes; tests pass a fixed value for
  determinism.
- `build_coinbase_jwt(key, params)` — produces the
  `header.payload.signature` JWT string. Signature is the IEEE P-1363
  r||s form (NOT DER) per JOSE / RFC 7518 §3.4 — getting that conversion
  wrong is the most common JWT-against-Coinbase bug.
- `tests/integration/test_coinbase_adapter.cpp` end-to-end exercises
  this against an in-process TLS WS server with `EVP_DigestVerify` on
  the server side — both a unit-level signature self-verify and a
  round-trip "the JWT survived TLS + WS unchanged" gate.

### HTTP CONNECT proxy (`include/eph/net/proxy.hpp`)

- `ProxyConfig { host, port, basic_auth_user, basic_auth_pass, timeout }`
  with a `noexcept` `validate()` that enforces non-empty host, non-zero
  port, XOR'd basic-auth fields, positive timeout.
- Consumed by the **kernel backend only** via `StreamConfig::proxy`. The
  DPDK backend rejects any non-empty `proxy` with `Error::InvalidConfig`
  because it has no kernel TCP path to tunnel through.
- Wire implementation: `include/eph/net/detail/http_connect.hpp` (generic
  over a ByteSink adapter).

### Stream metrics (`include/eph/net/stream_metrics.hpp`)

Two-layer observability shared by all four Stream/Datagram backends:

- `StreamMetric` enum (currently 25 entries: bytes / frames / codec /
  TLS handshake + resume + cross-record / TCP session + reorder + dup /
  ICMP / RX-checksum split / UDP drops / WS deflate in/out) with
  matching `kStreamMetricNames` (`net.stream.*` OTel-style).
- `publish_metrics<Stream, Sink>(source, sink, tags)` — `noexcept`
  alloc-free reader that pushes every counter into any
  `eph::core::MetricsSink` (NullSink / ConsoleSink / user-supplied).
- Hot-path writers (`inc_<M>()` templates) live in the backend types, not
  here — this module only defines the index and reader.

### POSIX server helpers (`include/eph/net/posix_io.hpp`, `include/eph/net/posix_listener.hpp`)

Small kernel-socket server primitives (`send_all`, `recv_exact`,
`tcp_bind_listen`, poll-based accept) that test fixtures and benchmark
mocks reuse. Namespace `eph::net::posix`. They are **server-side only**;
the client-side lives in `eph-net-kernel`. Promoted out of the bench tree
so tests no longer reverse-include benchmarks.

### Test mocks (`include/eph/net/test/`)

- `FakeStream` — in-memory `Stream` implementation. `inject_rx(span)` /
  `collect_tx()` / `clear_tx()` / `inject_disconnect()`.
- `FakeDatagram` — in-memory `Datagram` implementation.
- `TestPoller<P>` — drives registered pollables synchronously, no syscalls,
  no threads. Use with the two fakes for pure-logic unit tests.

### Shared wire-level detail (`include/eph/net/detail/`)

Implementation details used by both the kernel and DPDK backends when
`EnableTls=true` or `cfg.ws.path` is non-empty. Users never reference these directly.

- `tls_session.hpp` — TLS 1.3 session wrapping `aws-lc::SSL*`, templated on
  the byte-socket adapter the backend supplies.
- `tls_record.hpp` / `tls_decryptor.hpp` / `tls_encryptor.hpp` /
  `tls_inplace.hpp` / `tls_constants.hpp` — split out of `tls_session.hpp`
  so the zero-copy DPDK in-place AES-GCM decrypt path can be used
  independently of the full session state machine.
- `websocket.hpp` — RFC 6455 frame encode/decode + masking pool.
- `ws_handshake.hpp` — client-side `Upgrade: websocket` handshake over a
  generic ByteSink. Called transparently from
  `{Kernel,Dpdk}TcpStream::create()` when `StreamConfig::ws.path` is
  non-empty.
- `http_connect.hpp` — HTTP CONNECT proxy handshake (kernel backend only).

## Using the concepts

```cpp
#include "eph/net/concepts.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/codec/ws_codec.hpp"

using Stream = eph::net::kernel::KernelTcpStream<eph::codec::WsCodec>;

static_assert(eph::net::Pollable<Stream>);
static_assert(eph::net::Stream<Stream>);
```

## Writing tests with the fakes

```cpp
#include "eph/net/test/fake_stream.hpp"
#include "eph/net/test/test_poller.hpp"

namespace ent = eph::net::test;

TEST(MyApp, ReactsToIncomingBytes) {
    auto poller = ent::TestPoller<ent::FakeStream>::create();
    auto fake   = ent::FakeStream::create();
    poller->add(fake.get());

    fake->inject_rx({'h','i'});
    poller->poll();          // drives fake->poll_once_() synchronously

    auto tx = fake->collect_tx();
    EXPECT_EQ(tx.size(), 2);
}
```

No sockets, no network, no threading variance.

## Signing an exchange REST request

For HMAC venues (Binance / Bybit / OKX):

```cpp
#include "eph/net/hmac.hpp"

using namespace eph::net;

HmacSha256Key key{std::string_view{api_secret}};   // zero-on-destroy
HmacSha256Tag tag = hmac_sha256_sign(key, canonical_query);

uint8_t hex[64];
tag.to_hex(std::span<uint8_t, 64>{hex});           // zero-alloc hex
```

For Coinbase Advanced Trade (ES256 JWT):

```cpp
#include "eph/net/jwt_signed_request.hpp"

using namespace eph::net;

auto key = Es256PrivateKey::from_pem(pem_text).value();   // PKCS#8 or SEC1
CoinbaseJwtParams params{
    .key_id        = "organizations/.../apiKeys/...",
    .api_key_name  = "<api-key-name>",
    .method        = "GET",
    .uri           = "api.coinbase.com/api/v3/brokerage/accounts",
    .now_unix_secs = static_cast<uint64_t>(std::time(nullptr)),
};
auto jwt = build_coinbase_jwt(key, params).value();        // header.payload.sig
```

## Dependencies

- `eph-core` (public) — concepts, `Error`, `ErrorInfo`, `OutputBuffer`,
  `PacketView`, `TcpState`, `MetricsSink`.
- `eph-utils` (public) — TSC timer for arrival timestamps, HDR histogram
  primitives.
- `eph-containers` (public) — SPSC queues / ring buffers used inside the
  TLS reassembly buffer.
- `aws-lc` (pulled in by `detail/tls_*.hpp` and by `hmac.hpp`). Only link
  it in targets that instantiate a TLS-enabled stream or use
  `HmacSha256Key`.

## See also

- [`CHANGELOG.md`](CHANGELOG.md) — change history
- [`summary.md`](summary.md) — full public API surface reference
- [`docs/ONBOARDING.md`](docs/ONBOARDING.md) — new-contributor tour
- [`../docs/architecture.md`](../docs/architecture.md) — whole-project
  concept model
- [`../docs/poller-guide.md`](../docs/poller-guide.md) — using `Poller`
  with streams
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — design spec
- `.artifacts/phase-9-scope-decision.md` — rationale for the HTTP /
  proxy / WS feature scope
