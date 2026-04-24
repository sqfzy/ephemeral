# eph-net-kernel onboarding

## What's in this module

The kernel-side implementation of the networking concepts. Three
user-facing types:

- `KernelTcpStream<C, EnableTls>` — per-connection TCP stream with optional TLS.
- `KernelUdpSocket<C>` — per-connection UDP socket with multicast helpers.
- `KernelPoller` — epoll-based I/O driver.

Everything else (`ByteSocket`, `SpanView`, `TlsState`, `ReassemblyBuffer`)
is internal detail under `include/eph/net/kernel/detail/`. All detail types
live in `namespace eph::net::kernel::detail`.

## How to read the code

1. `include/eph/net/kernel/config.hpp` — config structs. Start here; they show
   what you can configure.
2. `include/eph/net/kernel/tcp_stream.hpp` — `KernelTcpStream`. Read
   `create()`, `poll_once_()`, `send()` in that order. The TLS path uses
   `detail::TlsState`.
3. `include/eph/net/kernel/poller.hpp` — `KernelPoller`. Watch how `add()` erases
   the Pollable type into a function-pointer entry and how `poll()` dispatches
   from `epoll_wait` output.
4. `include/eph/net/kernel/udp_socket.hpp` — `KernelUdpSocket`. Simpler than the
   TCP variant; a good place to see the Datagram concept in practice.
5. `include/eph/net/kernel/detail/` — detail types. Read only if you're fixing
   bugs or adding features at the byte-socket / reassembly layer.

## Running the tests

```bash
xmake build -g tests
xmake run test_kernel_tcp_stream
xmake run test_kernel_poller
xmake run test_kernel_udp_socket
xmake run test_kernel_ws_upgrade
xmake run test_kernel_tls_state
xmake run test_kernel_proxy_integration
```

Per-file targets are auto-globbed from `tests/test_*.cpp`. Cross-module
integration tests (`test_kernel_udp`, `test_transport_e2e`, `test_binance_adapter`,
…) live under `../../tests/integration/` — they are **not** defined by
`eph-net-kernel/xmake.lua`.

## Common tasks

### Using a different codec

Change the template parameter:

```cpp
using Raw = eph::net::kernel::KernelTcpStream<eph::codec::RawStreamCodec, false>;
using Ws  = eph::net::kernel::KernelTcpStream<eph::codec::WsCodec,       true >;
```

The stream instantiations share no code generation — each is a distinct
monomorphised class.

### Running without TLS

Set `EnableTls=false` in the template parameter. When TLS is disabled, the
`detail::TlsState` member is `std::monostate` and the encrypt/decrypt steps are
`if constexpr`-ed out entirely. No runtime cost.

```cpp
using PlainTcp = eph::net::kernel::KernelTcpStream<eph::codec::RawStreamCodec, false>;
```

### Debugging a stuck connection

1. Check `stream->state()` — it should be `TcpState::Established`.
2. Enable `SPDLOG_LEVEL_TRACE` — all non-trivial paths log via `tracing`-style
   macros. Look for `KernelTcpStream::poll_once_` / `send` lines.
3. Run the test suite with `xmake f -m debug && xmake build -g tests` so the
   debug-level logs compile in.
4. Check the internal reassembly buffer fullness via the metrics sink (if you've
   wired one in).

### Adding a feature to the byte socket

Edit `include/eph/net/kernel/detail/byte_socket.hpp`. Keep the API narrow —
`KernelTcpStream` (and the `PlainWsSink` / `TlsWsSink` handshake adapters)
rely on `connect` / `send` / `recv` / `set_no_delay` / `fd`. The socket
closes itself on destruction; there is no public `close` method.

## See also

- `README.md`
- `summary.md`
- `CHANGELOG.md`
- `../docs/poller-guide.md`
- `../docs/architecture.md`
