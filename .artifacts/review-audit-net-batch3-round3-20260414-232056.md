# Net Audit — Batch 3, Round 3 (2026-04-14 23:21 CST)

**Dimension**: Testing gaps + FakeStream/FakeDatagram semantic alignment with
real backends.

**Scope**: `eph-net/tests/test_fake_stream.cpp`,
`eph-net/include/eph/net/test/fake_stream.hpp`, `fake_datagram.hpp`,
`eph-net-kernel/tests/test_kernel_proxy_integration.cpp`,
`test_kernel_ws_upgrade.cpp`.

## Findings

### MEDIUM-1 — `FakeStream::poll_once_` ignores `state_`, diverges from
real backends on closed streams

- **File**: `eph-net/include/eph/net/test/fake_stream.hpp:186-198`
- **Defect**: when `state_ == TcpState::Closed` (either via explicit
  `set_state(Closed)` or via a successful `close_gracefully()`), the fake's
  `poll_once_()` still drains `rx_buf_` and fires `on_message`. The real
  backends (`KernelTcpStream::poll_once_:625`, `DpdkTcpStream::process_burst_`)
  early-return 0 when the state is not `Established`.

  Impact: any test that asserts reconnect / cleanup behavior after
  `close_gracefully()` gets bogus deliveries because the mock does not
  honor the state transition.
- **Severity**: MEDIUM — correctness-adjacent for mock-driven tests.
- **Fix**: add an early-return at the top of `FakeStream::poll_once_` that
  mirrors the kernel backend: `if (state_ != TcpState::Established) return 0;`.

### MEDIUM-2 — `FakeStream` has no `inject_error()` / `inject_disconnect()`
helper — tests cannot simulate mid-stream failure cleanly

- **File**: `eph-net/include/eph/net/test/fake_stream.hpp` (missing API)
- **Defect**: tests that want to simulate a mid-stream disconnect have to
  call `set_state(TcpState::Closed)` manually, but there is no helper to
  simulate the typical real-backend "peer FIN → state = Closed → poll_once_
  returns 0" sequence. Tests end up open-coding the combination (set_state
  + clear rx_buf), which drifts from reality.

  The kernel backend's `poll_once_` sets `state_ = Closed` on
  `ByteSocket::recv()` returning `Disconnected`. A mock should expose a
  single `inject_disconnect()` method that captures the same effect.
- **Severity**: MEDIUM — test ergonomics + correctness drift.
- **Fix**: add `inject_disconnect(eph::core::Error code = Disconnected)`
  that sets `state_ = TcpState::Closed` and (optionally) fires `on_message`
  on any buffered bytes before the disconnect — matching the real backend
  sequence "drain → close".

### MEDIUM-3 — No WS handshake timeout test exists (ws_timeout field
validation gap)

- **File**: `eph-net-kernel/tests/test_kernel_ws_upgrade.cpp`
- **Defect**: the six existing tests exercise: happy path, connect fail,
  wrong accept, empty ws_path, post-handshake echo, missing upgrade. There
  is no test that:
  - binds a TCP listener that `accept()`s but never responds (stalled
    server); asserts `ws_timeout` causes `Error::Timeout`.
  - verifies the server can return an HTTP 400 and the client maps it to
    `WsHandshakeFailed`.

  The `ws_timeout` field is exposed in `StreamConfig` and documented in
  `docs/custom-codec.md` but is literally untested.
- **Severity**: MEDIUM — field is shipped without test coverage. Unknown
  whether `ws_handshake.hpp` actually observes the timeout.
- **Fix**: add `test_kernel_ws_upgrade.cpp::WsHandshakeTimeoutIsEnforced`
  test that wires an `accept()`-but-silent server and asserts the client
  fails with `Error::Timeout` within `ws_timeout + slack` milliseconds.
  Skip the 400 case — it's adequately covered by the WrongAccept /
  MissingUpgrade pairs.

### LOW-1 — `FakeStream::send` returns the whole `data.size()` even when
`state_ != Established`

- **File**: `eph-net/include/eph/net/test/fake_stream.hpp:150-159`
- **Defect**: `send()` checks `attached_` but not `state_`. Real backend:
  `KernelTcpStream::send:529` returns `Error::Disconnected` when
  `state_ != Established`. Mock should match.
- **Severity**: LOW — rarely tripped.
- **Fix**: mirror the kernel check.

## Action plan

1. MEDIUM-1: FakeStream state-aware poll_once_.
2. MEDIUM-2: FakeStream inject_disconnect() helper + test.
3. MEDIUM-3: test_kernel_ws_upgrade WsHandshakeTimeoutIsEnforced.
4. LOW-1: FakeStream::send state check (bundled with #1 since it's 2
   lines in the same class).
