# eph-net-kernel

Header-only C++23 kernel networking backend. Implements the `eph::net::Stream` /
`Datagram` / `Poller` concepts on top of POSIX sockets + Linux `epoll`.

## Types

| Class | Header | Satisfies |
|---|---|---|
| `eph::net::kernel::KernelTcpStream<C, EnableTls>` | `eph/net/kernel/tcp_stream.hpp` | `eph::net::Stream` |
| `eph::net::kernel::KernelUdpSocket<C>`            | `eph/net/kernel/udp_socket.hpp` | `eph::net::Datagram` |
| `eph::net::kernel::KernelPoller`                  | `eph/net/kernel/poller.hpp`     | `eph::net::Poller` |

All three live in `namespace eph::net::kernel` and expose
`eph::net::kernel::detail::SpanView` as their `PacketView` associated type — a
contiguous `span<uint8_t>` over the reassembly / receive buffer.

TLS is **compile-time** opt-in via the `EnableTls` template parameter on
`KernelTcpStream`. There is no runtime `use_tls` bool; disabling TLS elides
the `detail::TlsState` member via `std::monostate` + `if constexpr` so the
plaintext path has zero TLS cost.

## Usage

```cpp
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/codec/ws_codec.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

auto poller = en::KernelPoller::create({}).value();

auto stream = en::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>::create({
    .remote = en::SocketAddr{ /* resolved ip */, 443 },
    .tls    = { .hostname = "fstream.binance.com" },
    .ws_path = "/ws/btcusdt@bookTicker",
}).value();

stream->on_message = [](std::span<const uint8_t> app_frame) {
    // Post-TLS-decrypt, post-codec application bytes. The span aliases the
    // stream's reassembly buffer and is valid only for this callback.
};

poller->add(stream.get()).value();

while (running) {
    poller->poll(100ms);      // epoll_wait(timeout)
}
```

`KernelTcpStream::create()` runs — in order — the TCP `connect(2)`, the
optional HTTP CONNECT proxy handshake (when `cfg.proxy` is set), the TLS 1.3
handshake (when `EnableTls=true`), and finally the optional WebSocket RFC 6455
upgrade (when `cfg.ws_path` is non-empty). Any of those may return an error
inside the `std::expected`; there is no half-initialised stream.

When `cfg.ws_permessage_deflate` is true (the default), the WS upgrade
offers RFC 7692 `permessage-deflate`. If the server accepts, the
`WsCodec` instance attached to this stream is configured to inflate
inbound RSV1 frames automatically — application code sees the
plaintext and only the `kWsDeflateBytesIn`/`kWsDeflateBytesOut`
metrics reveal the compressed-bytes ratio. Set the flag false to
suppress the offer for venues that mis-implement the extension.

## What's in each file

- `include/eph/net/kernel/config.hpp` — `StreamConfig`, `UdpConfig`,
  `PollerConfig`. Plain aggregates; see per-field doc comments for semantics.
- `include/eph/net/kernel/tcp_stream.hpp` — `KernelTcpStream<C, EnableTls>`.
  Combines `detail::ByteSocket` + codec + optional TLS + reassembly buffer +
  pull-model metric counters.
- `include/eph/net/kernel/udp_socket.hpp` — `KernelUdpSocket<C>`. Datagram
  equivalent with `send_to` / `join_multicast` / `leave_multicast` /
  `connect_to`.
- `include/eph/net/kernel/poller.hpp` — `KernelPoller` + the `KernelPollable`
  concept. `add()` calls `epoll_ctl(EPOLL_CTL_ADD)`; `poll()` / `poll(timeout)`
  call `epoll_wait` and dispatch readable fds to each entry's type-erased
  `poll_fn` function pointer (no virtual dispatch, no `std::function`).
- `include/eph/net/kernel/detail/` — `ByteSocket` (non-blocking socket
  wrapper), `SpanView` (the `PacketView` implementation), `TlsState`
  (wires `eph::net::detail::TlsSession` to `ByteSocket`),
  `ReassemblyBuffer`.

## Building and testing

```bash
xmake build eph-net-kernel                   # header-only target
xmake build -g tests                         # build every test in the group
xmake run test_kernel_tcp_stream             # run a specific test
xmake run test_kernel_poller
xmake run test_kernel_udp_socket
```

Per-file targets are auto-globbed from `tests/test_*.cpp`. Cross-module
integration tests (`test_kernel_udp`, `test_transport_e2e`, …) live under
`../tests/integration/`.

## Threading model

`KernelPoller` is single-threaded. Whatever thread calls `poll()` runs the
codec decode, TLS decrypt, and `on_message` callback in that same thread. For
multi-threaded apps, hand off work to other threads via `eph-containers` SPSC
queues.

`send()` is also synchronous on the caller's thread — TLS encrypt happens on
your stack and `write(2)` is called directly. There is no TX worker queue.

Reconnection is not performed inside the stream. `KernelTcpStream` surfaces
hard errors via `std::expected` / `TcpState::Closed` and expects the caller
to drive the retry loop (e.g. via `eph::net::ReconnectPolicy`). See
`../eph-net-kernel/CHANGELOG.md` for the rationale behind removing the
stream-local reconnect field in 2026-04-14.

## Observability

Each stream carries an `alignas(64) std::atomic<uint64_t>` array of metric
counters (one slot per `eph::net::StreamMetric`). Readers:

- Direct: `stream->metric(StreamMetric::kBytesSent)` — bounds-checked; OOB
  values return 0.
- Push: `eph::net::publish_metrics(*stream, sink, tags)` forwards every
  counter into any `MetricsSink`.

UDP backends wire `kBytesSent` / `kBytesRecv` / `kFramesDecoded` /
`kCodecErrors`; TCP wires those plus `kReasmOverflows` (TLS-only metrics
stay at 0 on the kernel path — see `eph/net/stream_metrics.hpp`).

## Dependencies

- `eph-net` (public) — concepts, `SocketAddr`, `ReconnectPolicy`, TLS detail
- `eph-core` (public, transitive via `eph-net`)
- `eph-utils` (transitive) — TSC timer, HDR histogram
- `eph-containers` (transitive)
- `spdlog`, `aws-lc` (public) — logging + TLS 1.3 primitives

## See also

- `summary.md` — public API surface (compact)
- `CHANGELOG.md` — version history
- `docs/ONBOARDING.md` — reading order for this module
- `../docs/poller-guide.md` — using the Poller across backends
- `../docs/architecture.md` — concept model
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — frozen design spec
