# eph-net onboarding

## What's in this module

`eph-net` is the narrow-waist between codecs and networking backends. It contains:

- The four networking concepts (`Pollable`, `Stream`, `Datagram`, `Poller`).
- Shared value types: `SocketAddr`, `TcpState`, `ReconnectPolicy`.
- In-memory test mocks: `FakeStream`, `FakeDatagram`, `TestPoller`.
- The shared TLS + WebSocket + HTTP wire detail (`detail/`) used by both the
  kernel and DPDK backends.

`eph-net` does NOT contain any backend implementation — that lives in
`eph-net-kernel` and `eph-net-dpdk`.

## How to read the code

1. `include/eph/net/concepts.hpp` — the four concepts. Start here.
2. `include/eph/net/socket_addr.hpp` — trivial value type.
3. `include/eph/net/reconnect_policy.hpp` — how reconnection backoff is
   computed.
4. `include/eph/net/test/fake_stream.hpp` — the simplest `Stream` implementation.
   Read it to internalise what "satisfies the concept" means.
5. `include/eph/net/test/test_poller.hpp` — the simplest `Poller`. Drives
   registered pollables synchronously.
6. `include/eph/net/detail/tls_session.hpp` — TLS 1.3 wrapper. Only read if you're
   working on the TLS path.

## Running the tests

```bash
xmake build -g tests
xmake run test_fake_stream
xmake run test_reconnect_policy
xmake run test_socket_addr
```

Per-file targets are auto-globbed from `tests/test_*.cpp`.

## Common tasks

### I want to test my application with a fake stream

Use `FakeStream` + `TestPoller`. See `test/fake_stream.hpp` for the full API.

```cpp
#include "eph/net/test/fake_stream.hpp"
#include "eph/net/test/test_poller.hpp"

namespace ent = eph::net::test;

auto poller = ent::TestPoller<ent::FakeStream>::create();
auto fake   = ent::FakeStream::create();
poller->add(fake.get());
fake->on_message = [&](const uint8_t* d, uint16_t n) { … };
fake->inject_rx(bytes);
poller->poll();
```

### I want to use my own TLS implementation

Don't — but if you must, look at `detail/tls_session.hpp` for the adapter
contract. `TlsSession` is templated on a byte-socket type that the backend
supplies (`KernelByteSocket` in `eph-net-kernel`,
`ByteSocketTcpAdapter` around `DpdkTcpSession` in `eph-net-dpdk`).

### I want to add a new test mock

Drop a new header under `include/eph/net/test/`. It must satisfy `Stream` or
`Datagram`. Add a unit test under `tests/` that proves concept satisfaction via
`static_assert`.

## See also

- `README.md` — module overview
- `summary.md` — public API surface
- `CHANGELOG.md` — change history
- `../docs/architecture.md` — whole-project concept model
- `../docs/poller-guide.md` — using `Poller` in application code
