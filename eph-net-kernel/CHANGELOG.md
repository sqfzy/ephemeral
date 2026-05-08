# eph-net-kernel changelog

## [Unreleased]

### Added (2026-05-08) — TLS 1.2 GCM/CHACHA20 transparent support

The kernel `TlsState::encrypt_for_send` now sizes its per-record
output buffer through the format-aware
`TlsRecordCrypto::encrypted_size(chunk)` instance method instead of
the old static helper. When the user opts into TLS 1.2 via
`cfg.tls.min_version = Tls12`, the kernel data-plane correctly
handles the AES-GCM-1.2 wire format (5B header + 8B explicit nonce +
ciphertext + 16B tag = N+29 bytes/record) and the CHACHA20-1.2 format
(5B header + ciphertext + 16B tag = N+21 bytes/record).

No public API change for the kernel backend itself — the visible
delta is buffer sizing math, which the user does not see. See
`eph-net/CHANGELOG.md` for the cross-module TLS 1.2 details.

### Added — InFlightStatus three-state classification on send paths (J series, 2026-05-05)

`KernelTcpStream::send` and `KernelUdpSocket::send_to` now populate
`eph::net::last_in_flight_detail()` with one of `Unsent` / `Sent` /
`Uncertain` on every error path (and in the partial-success TCP
branch). This brings the kernel backend to behavioural symmetry with
the DPDK backend's daemon-died classification (DpdkTcpStream commit
`3720e44e`), so HFT apps that switch backends — or use both in the
same process — share the same recovery state machine.

Classification rules:
  - NotAttached / fd_<0 / not-Established → Unsent (pre-burst)
  - sendto EAGAIN/EMSGSIZE/ENOBUFS/unreachable → Unsent (pre-enqueue)
  - non-TLS send error → Unsent (ByteSocket::send is all-or-nothing)
  - TLS encrypt error → Unsent (no socket call yet)
  - TLS sock send error → Uncertain (AEAD seq advanced; bytes might
    be partially on wire)
  - non-TLS partial return (`*sr < requested`) → Uncertain
  - successful sendto / sock_.send (full bytes) → Sent (kernel
    committed; do NOT retransmit on later EPIPE/ECONNRESET)

`eph::net::in_flight_status.hpp` (in eph-net) is the canonical shared
header. The DPDK-side `daemon_disconnected_hook.hpp` is now a thin
alias re-export for backwards compatibility.

New test: `tests/test_kernel_inflight_status.cpp` — 7 cases all
passing; verifies cross-backend thread_local sharing, every kernel
phase tag, and thread-isolation invariants.

Hot path cost: zero — populate calls fire only on error paths, which
are already cold. bench_kernel baselines preserved.

### Tests — symmetric concept conformance for WS / Mold64 codecs

Mirrors a parallel commit on the DPDK backend. The kernel-side
test files only compile-asserted concept conformance for the
`RawStreamCodec` / `RawDatagramCodec` instantiations, leaving
the production-canonical WS variant (binance / okx / coinbase
WS feeds, per CLAUDE.md) and the multi-frame Mold64Codec
example (Nasdaq ITCH over MoldUDP64) without compile-time
contract pins.

  * `tests/test_kernel_tcp_stream.cpp` adds `static_assert`s for
    `KernelTcpStream<WsCodec, false/true>` on Pollable / Stream /
    KernelPollable, plus the `CodecType` associated-type echo.
  * `tests/test_kernel_udp_socket.cpp` adds the same set of
    asserts for `KernelUdpSocket<Mold64Codec>`.

Pure additive — runtime tests are unchanged (8/8 + 12/12 PASS).
No public surface motion.

### Fixed — silent-error-log audit (batches 14-20)

Cold-path / control-plane functions previously returned typed errors
silently. Every fix preserves the return contract — only adds a
`SPDLOG_LOGGER_WARN` (caller-recoverable) or `ERROR` (programming
contract violation) with the offending value before the return. Sites
touched on the kernel side:

- `eph::net::kernel::detail::ByteSocket` — `fd<0` guard branches now
  WARN-log with the function name and fd value.
- `eph::net::kernel::detail::KernelUdpSocket` — cold-path guard
  branches now WARN-log.

Hot-path codec / parser silent-error branches were intentionally not
modified to keep steady-state throughput unchanged.

### Fixed — TLS desync latch on `encrypt_for_send` failure

`KernelTcpStream<C, EnableTls=true>::send` now latches `tls_corrupt_`
and bumps `kTlsSendDesyncs` when the in-stream `TlsState::
encrypt_for_send` returns an error. Previously the latch fired only
on the downstream `sock_.send` failure; a failure inside
`encrypt_for_send` (e.g. AEAD seq exhaustion mid-payload on a
multi-chunk plaintext) returned the error verbatim without latching,
so a subsequent `send()` could slip past the desync guard, encrypt
with the partially-advanced seq, hit the wire, and silently desync
the peer. Symmetric with the same fix on the DPDK backend (see
`eph-net-dpdk/CHANGELOG.md`).

### BREAKING CHANGES — StreamConfig reshape (2026-04-29, T3.19)

`KernelTcpStream::StreamConfig` field paths changed to a backend-symmetric
shape with shared `WsConfig` / `KeepaliveConfig` sub-configs and a
`Kernel` sub-struct for kernel-only knobs. Migration:

| Old field                       | New field                          |
|---------------------------------|------------------------------------|
| `cfg.ws_path`                   | `cfg.ws.path`                      |
| `cfg.ws_host`                   | `cfg.ws.host`                      |
| `cfg.ws_extra_headers`          | `cfg.ws.extra_headers`             |
| `cfg.ws_timeout`                | `cfg.ws.timeout`                   |
| `cfg.ws_permessage_deflate`     | `cfg.ws.permessage_deflate`        |
| `cfg.tcp_nodelay`               | `cfg.kernel.tcp_nodelay`           |
| `cfg.local`                     | `cfg.kernel.local_bind`            |
| (new)                           | `cfg.keepalive` — `KeepaliveConfig`|

**TCP keepalive** is now a public knob on the kernel surface
(previously DPDK-only). `KernelTcpStream::create` wires
`SO_KEEPALIVE / TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT` when
`cfg.keepalive.interval > 0`; `interval` is rounded up to seconds for
the syscall (kernel TCP keepalive is second-grained). Default
disabled.

`KernelTcpStream::create` validation now delegates to
`cfg.ws.validate()` and `cfg.keepalive.validate()` for sub-config
checks; the resulting `ErrorInfo::detail` strings come from those
sub-configs (e.g. "WsConfig: timeout must be > 0 when path is set"
instead of the legacy "ws_timeout must be > 0"). One assertion in
`tests/test_stream_config_validation.cpp` was updated to match.

## [Unreleased earlier] — Cross-module additions catch-up (2026-04-28)

### Added (cross-module work that landed on `KernelTcpStream`)
- **`KernelTcpStream::drain(std::chrono::milliseconds)`** (commit
  `3e853cdd`, 2026-04-28). Synchronous orderly-shutdown API: sends our
  FIN via `shutdown(fd, SHUT_WR)`, then `poll(POLLIN)` + `recv()` loop
  until peer FIN-ACK (recv() == 0). On timeout bumps
  `StreamMetric::kRxSessionResets`, falls back to `shutdown(SHUT_RDWR)`,
  returns `Err(Timeout)`. Pre-condition: state == Established (else
  `Err(InvalidConfig)` without touching the socket). Caller-owned —
  no Poller may concurrently drive the same stream.

- **TLS 1.3 session resumption (PSK ticket)** (commit `49fb08ef`,
  2026-04-28). Cross-cuts kernel + DPDK via the shared
  `eph::net::detail::TlsSession`. Kernel surface:
  - `StreamConfig::tls_resumption_ticket` accepts opaque DER-encoded
    `SSL_SESSION` bytes from a prior connection. Empty (default) =
    full handshake.
  - `KernelTcpStream::tls_resumption_ticket()` move-out the captured
    server-issued NewSessionTicket bytes after a successful handshake.
  - `KernelTcpStream::tls_was_resumed()` returns the
    `SSL_session_reused` boolean for the just-completed handshake.
  - `StreamMetric::kTlsResumeCount` / `kTlsHandshakeCount` counters
    fire at handshake completion. Pair to compute the resumption hit
    rate `resume / (resume + full)`.

- **`KernelTcpStream::metric` exposes WS deflate counters**
  (commit `4976af92`, 2026-04-28). Pulls
  `StreamMetric::kWsDeflateBytesIn` / `kWsDeflateBytesOut` from the
  decoder so callers see the achieved compression ratio. Stays at 0
  for non-WS streams or when `permessage-deflate` was not negotiated.

- **`StreamConfig::ws_permessage_deflate` auto-negotiation**
  (commit `c24ddac1`, 2026-04-26). RFC 7692 advertised in the upgrade
  request; on `Sec-WebSocket-Extensions: permessage-deflate` (with our
  parameters honored) in the response, the codec switches to the
  inflate path. README + ONBOARDING describe the wire-side handshake.

- **`ReconnectOrchestrator` integration smoke** (commit `c2ee84ff`,
  2026-04-28). End-to-end test
  `tests/test_kernel_reconnect_orchestrator.cpp` drives
  `ReconnectOrchestrator<KernelTcpStream<RawStreamCodec, false>>`
  against an in-process loopback echo server: peer-close detection
  via `auto_detect_via_state`, factory-driven reconnect after backoff,
  reconnect_count assertion. The orchestrator itself lives in
  `eph-net`; this entry records the kernel-side wiring proof.

### Changed
- No source changes to existing API. The four additions above are
  purely additive on the kernel surface.

### Doc sync (2026-04-24)

### Docs
- `README.md`, `summary.md`, `docs/ONBOARDING.md` re-aligned with the
  current public API:
  - `StreamConfig` uses `SocketAddr remote` / `SocketAddr local`, not
    `host` / `port`; TLS selection is the `EnableTls` template parameter
    (no runtime `use_tls` bool); the dead `bind_device` /
    `ReconnectPolicyConfig` fields are gone (the latter was dropped on
    2026-04-14, see entry below). New fields are now documented:
    `ws_host`, `ws_extra_headers`, `ws_timeout`, `proxy`,
    `connect_timeout`, `reasm_capacity`, `tcp_nodelay`.
  - `UdpConfig` uses `bind` / `connect_to` / `rcv_buf` / `snd_buf` /
    `reuse_addr`, not `bind_addr` / `rcvbuf` / `bind_device`.
  - `PollerConfig` uses `initial_capacity` + `max_events_per_wait`, not
    the old `max_events`.
  - Callback signatures updated to
    `OnMessage  = std::function<void(std::span<const uint8_t>)>` and
    `OnDatagram = std::function<void(std::span<const uint8_t>,
                                     const SocketAddr&)>` (the
    `(uint8_t*, uint16_t)` form was already gone from the code).
  - `KernelPoller::add`/`remove` template constraint is the local
    `KernelPollable` concept (not plain `Pollable`); internals note the
    `detach_fn` notification thunk and the 256-event burst cap in
    `epoll_wait`.
  - Detail namespace class is `detail::ByteSocket`, not the ghost
    `KernelByteSocket` name; ONBOARDING's reading list reflects that.
  - ONBOARDING's "running the tests" list no longer claims
    `xmake run test_kernel_udp` is a module-local target — that binary is
    built from `tests/integration/` and belongs to the root `xmake.lua`.
  - README's dead link to `docs/multi-connection.md` (never present in
    this module) is removed; observability section added pointing at the
    pull-model `metric(StreamMetric)` accessor and
    `eph::net::publish_metrics`.

No code, build, or test changes.

### Drop dead reconnect field (2026-04-14)

### Changed — BREAKING
- Removed `StreamConfig::reconnect` (`ReconnectPolicyConfig`) and the
  corresponding `KernelTcpStream::reconnect_policy_` member. The field
  was carried through the stream object but **never read** by any
  production code path; it was a leftover from the v3.3 architecture
  sketch. Keeping it created a "configured it, must be working"
  illusion for users.

  Migration: construct an `eph::net::ReconnectPolicy` in caller code
  and drive the reconnect loop there. See
  `examples/session_reconnect.cpp` for the canonical pattern and
  `examples/production_client.cpp` for a TLS + signal-driven variant.

  Rationale: session recovery (FIX Logon seq resync, ITCH snapshot
  replay, kill-switch gating, primary/backup routing) is inherently
  protocol-layer; a stream-local retry cannot see that state. The
  stream between `create()` and `poller.add()` is also unobservable
  to any supervisor, so internal retry loops run in a blind spot.

  `eph::net::ReconnectPolicy` itself (pure backoff math, no I/O) is
  unchanged and is now the recommended caller-side primitive.

### Phase 9 Recovery (2026-04-10)

### Added
- `StreamConfig` grew four new fields (all optional, defaulted to
  empty):
  - `std::string ws_path` — non-empty activates RFC 6455 client
    handshake inside `KernelTcpStream::create()` (decision D-2).
  - `std::vector<std::pair<std::string,std::string>> ws_extra_headers`
    — arbitrary headers the caller wants injected into the upgrade
    request.
  - `std::chrono::milliseconds ws_timeout` — deadline for the upgrade
    exchange; hits `Error::Timeout` on expiry.
  - `std::optional<eph::net::HttpConnectConfig> proxy` — non-empty
    routes the initial TCP connect through an HTTP CONNECT proxy via
    `eph::net::detail::perform_http_connect`.
- `KernelTcpStream::create()` now transparently performs the proxy
  CONNECT (if configured) before TLS, and the WS upgrade (if
  configured) after TLS. Behaviour with all four fields empty is
  identical to pre-Phase 9.

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
