# eph-net-kernel

Header-only C++23 kernel networking backend. Implements the `eph::net::Stream` /
`Datagram` / `Poller` concepts on top of POSIX sockets + `epoll`.

## Types

| Class | Header | Concept |
|---|---|---|
| `eph::net::kernel::KernelTcpStream<C, EnableTls>` | `eph/net/kernel/tcp_stream.hpp` | `eph::net::Stream` |
| `eph::net::kernel::KernelUdpSocket<C>` | `eph/net/kernel/udp_socket.hpp` | `eph::net::Datagram` |
| `eph::net::kernel::KernelPoller` | `eph/net/kernel/poller.hpp` | `eph::net::Poller` |

All three live in `namespace eph::net::kernel` and use `eph::net::kernel::detail::SpanView`
as their `PacketView` associated type — a contiguous `span<uint8_t>` over the
reassembly buffer.

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
    .remote_host = "fstream.binance.com",
    .remote_port = 443,
    .ws_path     = "/ws/btcusdt@bookTicker",
    .use_tls     = true,
}).value();

stream->on_message = [](const uint8_t* data, uint16_t len) {
    /* handle frame */
};

poller->add(stream.get()).value();

while (running) {
    poller->poll(100ms);      // epoll_wait(timeout)
}
```

## What's in each file

- `include/eph/net/kernel/config.hpp` — `StreamConfig`, `UdpConfig`, `PollerConfig`.
- `include/eph/net/kernel/tcp_stream.hpp` — `KernelTcpStream<C, EnableTls>`.
  Combines `KernelByteSocket` + codec + optional TLS state + reassembly buffer.
- `include/eph/net/kernel/udp_socket.hpp` — `KernelUdpSocket<C>`. Same shape
  but UDP, with multicast helpers.
- `include/eph/net/kernel/poller.hpp` — `KernelPoller`. Owns an `epoll_fd_` and a
  `vector<PollableEntry>`. `add()` calls `epoll_ctl(EPOLL_CTL_ADD)`; `poll()` calls
  `epoll_wait` and dispatches to the matching entry's `poll_fn(void*)`.
- `include/eph/net/kernel/detail/` — `KernelByteSocket`, `SpanView`,
  `TlsState<ByteSocket>`, `ReassemblyBuffer`.

## Building and testing

```bash
xmake build eph-net-kernel
xmake build -g tests
xmake run test_kernel_tcp_stream
xmake run test_kernel_poller
xmake run test_kernel_udp_socket
```

Per-file targets are auto-globbed from `tests/test_*.cpp`.

## Threading model

`KernelPoller` is single-threaded. Whatever thread calls `poll()` runs the codec
decode, TLS decrypt, and `on_message` callback in that same thread. For
multi-threaded apps, hand off work to other threads via `eph-containers` SPSC
queues. See `docs/multi-connection.md`.

`send()` is also synchronous on the caller's thread — TLS encrypt happens on your
stack, `write(2)` is called directly. There is no TX worker queue.

## Dependencies

- `eph-net` (public) — concepts, `SocketAddr`, `ReconnectPolicy`, TLS detail
- `eph-core` (transitive)
- `eph-utils` (transitive) — TSC timer, HDR histogram
- `eph-containers` (transitive)

## See also

- [`docs/poller-guide.md`](../docs/poller-guide.md) — using the Poller
- [`docs/architecture.md`](../docs/architecture.md) — concept model
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — design spec
