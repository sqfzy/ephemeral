# eph-net

Header-only C++23 module defining the networking concepts and shared types.
Does not contain any backend implementation — that lives in `eph-net-kernel` and
`eph-net-dpdk`. `eph-net` is the narrow-waist between codecs and backends.

## What lives here

### Concepts

`include/eph/net/concepts.hpp` defines the four networking concepts:

- `Pollable<T>` — any type a `Poller` can drive (private `poll_once_()`).
- `Stream<T>` — TCP-style connection. `send()`, `close_gracefully()`, `on_message`.
- `Datagram<T>` — UDP-style socket. `send_to()`, `join_multicast()`, `on_datagram`.
- `Poller<T>` — the I/O driver. `add()`, `remove()`, `poll()`.

### Value types

- `SocketAddr` / `Ipv4Addr` (`include/eph/net/socket_addr.hpp`)
- `TcpState` (re-exported from `eph/core/tcp_state.hpp`)
- `ReconnectPolicy` / `ReconnectPolicyConfig`
  (`include/eph/net/reconnect_policy.hpp`)

### Test mocks (`include/eph/net/test/`)

- `FakeStream` — in-memory `Stream` implementation for unit tests.
  `inject_rx()` / `collect_tx()` / `inject_disconnect()`.
- `FakeDatagram` — in-memory `Datagram` implementation.
- `TestPoller<P>` — drives registered pollables synchronously, no syscalls.

### Shared wire-level detail

`include/eph/net/detail/` contains the shared TLS / WebSocket / HTTP wire helpers
used by both the kernel and DPDK backends:

- `tls_session.hpp` — TLS 1.3 session wrapping `aws-lc::SSL*`. Driven by whichever
  byte-socket adapter the backend provides.
- `websocket.hpp` — RFC 6455 wire parsing and masking helpers.
- `http_request.hpp` / `http_response.hpp` — HTTP/1.1 minimal parser for the WS
  upgrade handshake.

These are `detail::` types — users don't touch them directly; they're pulled in
transparently by `KernelTcpStream` / `DpdkTcpStream` when `EnableTls=true`.

## Using the concepts

```cpp
#include "eph/net/concepts.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/codec/ws_codec.hpp"

using Stream = eph::net::kernel::KernelTcpStream<eph::codec::WsCodec>;

static_assert(eph::net::Stream<Stream>);
static_assert(eph::net::Pollable<Stream>);
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

## Dependencies

- `eph-core` (public) — concepts, `Error`, `ErrorInfo`, `OutputBuffer`, `PacketView`.
- `eph-utils` (public) — TSC timer for arrival timestamps, HDR histogram for metrics.
- `eph-containers` (public) — SPSC queues / ring buffers used by the TLS reassembly
  buffer.
- `aws-lc` (private-ish) — pulled in by `detail/tls_session.hpp`. Only link it in
  targets that instantiate a TLS-enabled stream.

## See also

- [`docs/architecture.md`](../docs/architecture.md) — the three concept layer
- [`docs/poller-guide.md`](../docs/poller-guide.md) — using `Poller` with streams
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — design spec
