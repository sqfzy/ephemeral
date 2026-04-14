# Net Audit — Batch 1 Round 2

- Dimension: **Error handling / `std::expected<T, ErrorInfo>` contract**
- Date: 2026-04-14 (Asia/Shanghai)
- Scope: `eph-net/`, `eph-net-kernel/`, `eph-net-dpdk/` — silently swallowed
  errors, partial-operation handling, observability of error branches.

## Findings

### MEDIUM-1 — `ByteSocket::connect` silently drops the TCP_NODELAY setsockopt error

**File:** `eph-net-kernel/include/eph/net/kernel/detail/byte_socket.hpp:135-136`

```cpp
int one = 1;
(void)::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
```

For an HFT codebase this is a latency catastrophe waiting to happen:
if `setsockopt(TCP_NODELAY)` silently fails (e.g. because the protocol
returns EOPNOTSUPP on an unusual socket family or the syscall is
blocked by seccomp), Nagle's algorithm stays on and every small
send pays a 40 ms coalescing penalty. The caller has no way to know.

**Recommended action:** log a WARN with errno on failure. Keep the
result non-fatal (some unusual environments may legitimately reject
the option) but make the noise level match the consequence.

### MEDIUM-2 — `KernelUdpSocket::create` silently drops SO_REUSEADDR / SO_RCVBUF / SO_SNDBUF failures

**File:** `eph-net-kernel/include/eph/net/kernel/udp_socket.hpp:90-101`

```cpp
if (cfg.reuse_addr) {
    int one = 1;
    (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}
if (cfg.rcv_buf > 0) {
    int v = static_cast<int>(cfg.rcv_buf);
    (void)::setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
}
if (cfg.snd_buf > 0) {
    int v = static_cast<int>(cfg.snd_buf);
    (void)::setsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
}
```

Same shape as MEDIUM-1. Market-data consumers size `rcv_buf` to
absorb burst traffic; if SO_RCVBUF is silently rejected the kernel
may drop packets under load and the operator has no visibility.
SO_SNDBUF similarly affects order send paths.

**Recommended action:** log a WARN + errno on each failure. Continue
so that a rejected option does not tear down the whole UDP socket,
but surface the rejection.

### LOW-3 — `KernelTcpStream::close_gracefully` ignores `::shutdown()` return value

**File:** `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:556-563`

```cpp
if (sock_.fd() >= 0) {
    ::shutdown(sock_.fd(), SHUT_WR);
}
state_ = TcpState::FinWait1;
```

`::shutdown` can fail with `EBADF` (already closed from another thread —
unlikely here but possible in misuse), `ENOTCONN` (socket never
reached Established), or `EINVAL`. Current code unconditionally
flips the state to `FinWait1` as if the FIN was sent, even when the
kernel rejected the call. Not a safety bug — the next `send` will
re-detect the error — but the state machine misrepresents reality
for a short window.

Deferred to a future round: low enough that it can batch with other
close-path touchups.

### LOW-4 — `KernelTcpStream::set_no_delay` result ignored

**File:** `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:319`

```cpp
if (stream->cfg_.tcp_nodelay) {
    (void)stream->sock_.set_no_delay(true);
}
```

The call returns `std::expected<void, ErrorInfo>`. Dropping the
error is consistent with the byte-socket-level silent-drop but
loses even the WARN log that `set_no_delay` would emit — wait,
looking more closely, `set_no_delay` returns an error without
logging, so the only observation the user gets is the missed
optimization. Mirrors MEDIUM-1 on a slightly different axis.

Covered by the MEDIUM-1 fix once `set_no_delay` itself logs — this
call site can keep dropping the expected value without losing
visibility, so no separate fix needed in this round.

## Scope decision

Fixes in this round (2 fixes, 1 audit commit):

1. `fix(net-kernel)`: make `ByteSocket::connect` + `ByteSocket::set_no_delay`
   WARN-log errno on TCP_NODELAY failure (addresses MEDIUM-1 + LOW-4 in
   one touchpoint).
2. `fix(net-kernel)`: make `KernelUdpSocket::create` WARN-log errno on
   SO_REUSEADDR / SO_RCVBUF / SO_SNDBUF failures (addresses MEDIUM-2).

LOW-3 and other close-path nits are deferred to the next error-handling
round.
