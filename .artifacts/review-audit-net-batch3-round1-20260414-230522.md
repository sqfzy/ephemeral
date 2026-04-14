# Net Audit — Batch 3, Round 1 (2026-04-14 23:05 CST)

**Dimension**: Kernel backend details — epoll LT semantics, EAGAIN loop
completeness, FD_CLOEXEC / SIGPIPE / TCP_NODELAY / SO_KEEPALIVE, connect race,
handshake leftover pump.

**Scope**: `eph-net-kernel/include/eph/net/kernel/*.hpp` and
`detail/byte_socket.hpp`.

**Ground rules**: don't duplicate any finding from batch1/batch2 reports:

- batch1 R2 already logged setsockopt WARN on failure for TCP_NODELAY, SO_REUSEADDR,
  SO_RCVBUF/SO_SNDBUF.
- batch1 R2 covered `ByteSocket::connect` getsockopt SO_ERROR distinguishing from
  timeout.
- batch2 R1 fixed FakeStream/FakeDatagram PacketView and `close_gracefully`
  SHUT_WR + ENOTCONN path.

## Findings

### HIGH-1 — Post-handshake WS/CONNECT leftover is never drained without a
subsequent kernel recv event

- **File**: `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:463-483` (WS
  upgrade) and `:370-386` (post-CONNECT plaintext).
- **Defect**: after `perform_ws_handshake` (or HTTP CONNECT) succeeds, the code
  seeds any over-read bytes into `reasm_` via `commit_write(leftover.size())`
  and moves on. `state_` becomes `Established` at the end of `create()`. The
  user then calls `poller->add(stream)` and begins `poll()`.

  With EPOLLIN level-triggered, `epoll_wait` fires only while the socket has
  *kernel-buffered* readable data. Because the handshake `recv` already drained
  the kernel buffer down to the boundary where the first application frame
  sat, no EPOLLIN will fire until *another* network packet arrives. Until that
  happens, the seeded leftover bytes sit in `reasm_` and the codec never sees
  them.

  Impact: if the first post-handshake frame arrives in the same TLS/TCP segment
  as the upgrade response (common for bookTicker subscribes, where the server
  pushes an initial snapshot immediately after the 101), the application
  observes a silent stall — N-1 frames are held back until frame N triggers
  the next epoll notification.
- **Severity**: HIGH — latency footgun on every WS stream that experiences an
  over-read at handshake time. Silent correctness-adjacent.
- **Fix**: after state_ is set to `Established`, if `reasm_.readable() > 0`,
  call `drain_codec_()` once on the first successful `poll_once_()` OR simply
  drain inside `poll_once_()` before the sock recv (so any pre-seeded data is
  processed before we hit EPOLLIN).

  A minimal fix: change `poll_once_()` so that we always try `drain_codec_()`
  on existing buffered data first, then recv, then drain again. This is
  idempotent — `drain_codec_()` on an empty buffer is a no-op — and avoids the
  head-of-line stall.

### HIGH-2 — `ByteSocket::send` EAGAIN fallback uses hard-coded 1000 ms poll;
not configurable and not observable

- **File**: `eph-net-kernel/include/eph/net/kernel/detail/byte_socket.hpp:286-301`
- **Defect**: on EAGAIN/EWOULDBLOCK the code does `::poll(&pfd, 1, 1000)`.
  Magic number `1000` hard-coded with no constant, no config field, no per-
  call deadline. For HFT send paths this is 1000× too long — a stalled write
  burns the entire 1-second budget waiting for TCP backpressure to clear
  when the caller's policy may be "fail after 10 ms".
- **Severity**: HIGH — silent tail latency injection under load; also no
  context in the timeout log (no `fd`, no `remain` bytes, no elapsed time).
- **Fix**: extract `constexpr std::chrono::milliseconds
  kSendBackpressurePollMs{20}` and WARN-log with fd + remaining bytes when we
  hit the fallback path. A full StreamConfig field is ideal but the minimal
  fix is simply a constexpr + better log context so operators can tune it
  without hunting through source.

### MEDIUM-1 — `saturate_u16` silently truncates frame length on dispatch

- **File**: `eph-net/include/eph/net/concepts.hpp:47-49`, called from
  `tcp_stream.hpp:776, 837` and `udp_socket.hpp:317`.
- **Defect**: `on_message(frame.data(), saturate_u16(frame.size()))` clamps
  any frame >65535 bytes to 65535 and dispatches silently. The caller sees a
  frame of length 65535 but the backing data may actually be larger (codec
  returned a bigger Frame view). There is NO log, no error return.
- **Scenarios**: WS text/binary frames up to 125 bytes, extended-length up
  to 64 KiB, extended-length up to 2^63. Exchange market-data frames
  *rarely* cross 64 KiB but JSON snapshots of full order books (Deribit,
  Bybit) absolutely do.
- **Severity**: MEDIUM — silent data corruption path. Behavior is correct
  for the 99% case, pathological for the 1%.
- **Fix**: when `frame.size() > 0xFFFF`, WARN-log once with a
  `static thread_local bool warned = false;` guard, then dispatch with the
  clamped length. Long term the callback signature should take `uint32_t` or
  `std::size_t` — but that's a concept change, out of scope for this audit.
  Minimal improvement: at least emit a TRACE or WARN log so the loss is
  visible in operators' dashboards.

### MEDIUM-2 — `KernelTcpStream::poll_once_` no-sink branch discards bytes
but never advances `state_` correctly on decode/resync

- **File**: `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:625-635`
- **Defect**: when `on_message` is unset, the drain branch allocates a 4096-
  byte stack sink and does `sock_.recv(sink, sizeof(sink))`. If the recv
  fails with WouldBlock it returns 0 (correct). If it fails with
  Disconnected, we flip `state_` (correct). If it returns N > 4096-bounded
  bytes, we just throw them away. This is fine as a drain *intent*, but:

  (a) we do not log anything — not even DEBUG — so operators cannot see
      that a stream is running without a sink and silently throwing
      data away;
  (b) we drain only once per `poll_once_()`, so for a bursty sender the
      kernel buffer keeps growing while we ignore it, and the next
      LT epoll event will re-fire.

- **Severity**: MEDIUM — latent developer-error footgun. If a user forgets to
  assign `on_message` before attach(), their stream silently drops messages
  with no diagnostic.
- **Fix**: at the top of `poll_once_()`, if `!on_message` AND the stream is
  attached, WARN-log once per stream instance (thread_local flag) that
  `on_message is unset — drain path is discarding bytes`.

### MEDIUM-3 — `KernelUdpSocket::poll_once_` swallows decode errors without
surfacing to caller

- **File**: `eph-net-kernel/include/eph/net/kernel/udp_socket.hpp:321-326`
- **Defect**: after `codec_.decode(view, out_sink, sink)`, if `!dr`, we log
  WARN and `return delivered`. The caller has no way to know the datagram
  was malformed — a poisoned Mold64 sequence silently advances the seq gap
  counter. At least for UDP, the codec is per-packet and a decode error on
  one datagram does NOT corrupt the next datagram's decode state, so
  returning `delivered` is not wrong, but the absence of any signal to the
  application about a corrupted datagram (e.g. via `on_datagram` with a
  sentinel, or a separate `on_decode_error` callback) is an observability
  gap.
- **Severity**: MEDIUM — market-data loss visibility gap.
- **Fix**: keep the current behavior but upgrade the log to ERROR with full
  context (src_addr, first 16 bytes of payload hex-dumped at TRACE level
  only so release builds are not chatty). A callback-based hook would be
  cleaner but is a public-API change.

### LOW-1 — `KernelPoller::poll_impl_` fallback on epoll_wait error returns 0
without state; caller cannot distinguish "idle" from "error"

- **File**: `eph-net-kernel/include/eph/net/kernel/poller.hpp:310-313`
- **Defect**: when `epoll_wait` returns a non-EINTR error, the code WARN-logs
  and returns 0 as if nothing was ready. The user loop
  (`while (running) poller.poll();`) can't distinguish a true idle tick from
  an epoll_fd that is permanently broken (EBADF / EFAULT). A persistently
  broken fd spins the loop.
- **Severity**: LOW — only fires in pathological cases (UB, test sabotage).
- **Fix**: set a `poll_broken_` flag after a non-recoverable error and make
  subsequent `poll()` calls return 0 without calling `epoll_wait` again.
  Add an accessor `is_broken()` so tests and long-running services can
  detect and rebuild. Left as future work.

### NIT-1 — `KernelPoller` destructor doesn't guard against `detach_fn` being
null (defensive only)

- **File**: `eph-net-kernel/include/eph/net/kernel/poller.hpp:130-134`
- The code already guards with `if (e.detach_fn != nullptr)` — this is fine
  in practice because `add()` always sets it. Left as documentation only.

## Action plan (fix order, 1 commit each)

1. **HIGH-1** — Drain seeded reasm buffer on first poll_once_() (or
   unconditionally before recv); covered by `test_kernel_tcp_stream` and
   `test_kernel_ws_upgrade`.
2. **HIGH-2** — Extract the 1000 ms magic number, add log context; no test
   change needed (behavior preserved; new constant + WARN log).
3. **MEDIUM-1** — Warn-once when saturate_u16 actually truncates; covered
   implicitly by existing tests that never exercise >64 KiB frames.
4. **MEDIUM-2** — Warn-once when `on_message` is unset on an attached stream;
   covered by existing poll tests.
5. **MEDIUM-3** — Upgrade udp_socket decode-error log to ERROR with context.

MEDIUM-3 and LOW-1 are the tail; keep total ≤ 6 per the batch rules.

## Out of scope for this round

- Keep-alive (SO_KEEPALIVE) — never set; user can add via socket options
  wrapper if needed. Not an HFT concern because idle connections are torn
  down by reconnect policy.
- FD_CLOEXEC / SOCK_CLOEXEC — already correct everywhere (all sockets use
  `SOCK_CLOEXEC`).
- SIGPIPE — already suppressed via `MSG_NOSIGNAL` at every `::send()`/
  `::sendto()` call.
- Non-blocking connect race — already correctly handled via poll(POLLOUT) +
  getsockopt(SO_ERROR).
