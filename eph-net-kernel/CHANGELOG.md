# eph-net-kernel changelog

## v3.3 (2026-04-10) — module introduced

`eph-net-kernel` was created during v3.3 Phase 3 to host the kernel-side
networking backend. Before v3.3 the corresponding code lived inside `eph-net`
as `SocketTransport` + friends.

### Added
- `eph/net/kernel/tcp_stream.hpp` — `KernelTcpStream<C, EnableTls>`. Successor to
  `eph::net::SocketTransport` folded into `eph::transport::Transport`. Combines
  socket + codec + TLS into one monomorphised class; satisfies `eph::net::Stream`.
- `eph/net/kernel/udp_socket.hpp` — `KernelUdpSocket<C>`. Brand new in v3.3; there
  was no kernel UDP abstraction before. Satisfies `eph::net::Datagram`.
- `eph/net/kernel/poller.hpp` — `KernelPoller`. epoll-based driver with
  `poll()` / `poll(timeout)` overloads. Heterogeneous streams via P2 function-pointer
  type erase.
- `eph/net/kernel/config.hpp` — `StreamConfig` / `UdpConfig` / `PollerConfig`.
- `eph/net/kernel/detail/` — `KernelByteSocket` (non-blocking socket wrapper),
  `SpanView` (the `PacketView` implementation), `TlsState` (wires the shared
  `eph::net::detail::TlsSession` to `KernelByteSocket`), `ReassemblyBuffer`.

### Notes
- The pre-v3.3 threading model had a TX worker thread and an RX worker thread
  managed by `eph::transport::Transport`. v3.3 collapsed that into a single Poller
  thread that runs both codec and TLS work on the caller. See `README.md` and
  `docs/poller-guide.md` for the new model.
- Reconnection backoff uses `eph::net::ReconnectPolicy` (unchanged from pre-v3.3).
- TLS path uses the shared `eph::net::detail::TlsSession` wired through
  `KernelByteSocket` so the same TLS 1.3 code backs both kernel and DPDK.
