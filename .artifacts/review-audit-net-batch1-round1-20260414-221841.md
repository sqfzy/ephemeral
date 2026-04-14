# Net Audit — Batch 1 Round 1

- Dimension: **Concurrency / Memory safety & partial-write correctness**
- Date: 2026-04-14 (Asia/Shanghai)
- Scope: `eph-net/`, `eph-net-kernel/`, `eph-net-dpdk/` (plus the shared TLS /
  WS / HTTP-CONNECT handshake paths in `eph-net/include/eph/net/detail/`).

## Findings

### HIGH-1 — DPDK plaintext `DpdkTcpStream::send` does not chunk by MSS

**File:** `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:577-587`

`DpdkTcpStream::send` (the `EnableTls=false` branch) calls
`sess_.send(data.data(), data.size())` directly with the caller-supplied
payload length. However `eph::dpdk::TcpSession::send`
(`eph-net-dpdk/include/eph/dpdk/tcp.hpp:640-649`) hard-errors on any
`len > config_.mss`:

```cpp
if (len > config_.mss) {
    return std::unexpected(std::format(
        "Payload too large: {} > MSS {}", len, config_.mss));
}
```

with default `mss == kDefaultMss == 1460` (standard Ethernet). Any
user who calls `DpdkTcpStream::send` with a payload of 1461 bytes or more
gets a `Disconnected` error from the plaintext DPDK backend.

The TLS branch directly above it (lines 544-576) correctly chunks the
encrypted buffer by `sess_.mss()`, and the WS handshake sinks
(`PlainDpdkWsSink::send`, lines 155-176) also chunk. **Only the hot
plaintext path is missing the loop.**

This is a functional correctness bug in the DPDK backend for non-TLS
users — e.g. an ITCH client that writes a burst of order entries > 1460B
hits a hard send failure even though the session is healthy.

**Recommended action:** lift the same while-loop chunking that the TLS
branch uses into the plaintext branch, and aggregate the returned byte
counts so the public contract (`bytes_sent`) remains correct.

### HIGH-2 — `DpdkTcpStream::send` plaintext path exits on short write without error but loses bytes

**File:** `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:578-586`

Closely related to HIGH-1: even if the caller's payload happens to fit
within MSS, the plaintext path returns `*r` verbatim without verifying
that `*r == data.size()`. If `TcpSession::send` ever returned a partial
success (the current impl returns exactly `len` on success, but the
contract is typed as `expected<size_t, string>` and the type signature
does not pin down "all-or-nothing"), higher-level codecs that trust the
v3.3 Stream contract (`on success, all bytes are accepted or queued`)
would silently lose bytes.

The simplest mitigation — identical to the MSS-chunking fix — is to
loop until every byte of `data` is committed, so the public `send`
function becomes contractually all-or-nothing regardless of any future
TcpSession partial-send behaviour.

### MEDIUM-3 — `TlsWsSink::recv` spins up to 8 iterations, each blocking the whole recv(2) in a tight loop

**File:** `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:166-205`

The TLS WS handshake's inbound sink loops up to 8 times on a 4 KiB
`tmp` buffer, accumulating ciphertext into `rx_cipher`, then calls
`tls->process_records`. Each iteration calls `sock_.recv` which — in
the handshake window — is in non-blocking mode. On real networks the
loop spins until either (a) one full record arrives or (b) 8 iterations
elapse, at which point we return `WouldBlock`.

The bug is subtle: `sock->recv` on an empty socket returns an `Err`
with code `WouldBlock`. The current code at line 169-172:

```cpp
if (!rr) {
    // Propagate WouldBlock so the handshake driver can re-poll.
    return std::unexpected(rr.error());
}
```

unconditionally propagates any error — **including actual WouldBlock on
the very first iteration before any bytes were read**. That's actually
correct behaviour for the handshake driver (it will retry until its
deadline trips). But then the for-loop `iter < 8` becomes dead code,
because we exit on the first WouldBlock, never reaching iter=1..7.

This is not a safety bug — it just means the 8-iteration "bounded retry"
never fires as intended. Clean up to either (a) drop the bounded loop
and return WouldBlock directly, or (b) continue on WouldBlock inside
the loop.

Severity MEDIUM because the current behaviour happens to work (the
outer handshake driver loops on WouldBlock with its own deadline) but
the inner loop is misleading dead code that will confuse the next
reader.

### MEDIUM-4 — `KernelUdpSocket::send_to` error path lumps EMSGSIZE / EINVAL into `Disconnected`

**File:** `eph-net-kernel/include/eph/net/kernel/udp_socket.hpp:174-186`

`sendto` can fail with `EMSGSIZE` (payload > link MTU once a
PMTU-discovery event has fixed the path), `EINVAL`, `ENETUNREACH`,
`EHOSTUNREACH`, `EPERM` (firewall drops), and others. The current code
maps **every** non-`EAGAIN` errno to
`Error::Disconnected, "KernelUdpSocket::send_to: unexpected I/O error"`,
losing the actionable cause.

For HFT observability this conflates "the kernel said the payload is
too large" (a programming error the caller must fix) with "the peer
went away" (a reconnect event). The current behaviour hides the former
behind the latter, and the WARN log line at line 180-182 is the only
evidence operators see.

**Recommended action:** split out EMSGSIZE → `InvalidConfig`, ENETUNREACH
/ EHOSTUNREACH / EPERM → dedicated or generic `PeerUnreachable`-style
errors, keeping `Disconnected` for RST / shutdown. At minimum, include
the errno name in the detail string.

### LOW-5 — `PlainDpdkWsSink::recv` retries up to 16 times without a bound deadline

**File:** `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:193-219`

Same category as MEDIUM-3 but on the DPDK side. The 16-iteration bound
is a fixed loop-count budget, not a time budget. Under heavy inbound
pressure with lots of empty bursts this adds a few microseconds before
returning WouldBlock to the handshake driver. Acceptable but worth
documenting.

### NIT-6 — DPDK `DpdkTcpStream::~` logs at DEBUG but Kernel `~KernelTcpStream` logs at DEBUG with the fd

**File:** `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:510-513`

Observability parity: the kernel destructor logs the fd, the DPDK
destructor does not. For post-mortem debugging of attach/detach
races, include the identifier (e.g. `&sess_` or the tuple) in the
message.

## Scope decision

The two HIGH findings are both in one function and share one fix; I will
implement them as a single commit. MEDIUM-3 and MEDIUM-4 are
independent of each other and of the HIGH fixes, so one commit each.
LOW-5 and NIT-6 are deferred to a future round to stay within the
round budget.

Planned this-round fixes (3 fixes):
1. `fix(net-dpdk)`: chunk plaintext DpdkTcpStream::send by MSS + tight
   loop (addresses HIGH-1 and HIGH-2 jointly).
2. `refactor(net-kernel)`: collapse TlsWsSink bounded-retry into a
   clear WouldBlock propagation (addresses MEDIUM-3).
3. `fix(net-kernel)`: classify KernelUdpSocket::send_to errno into
   actionable Error codes + include errno in detail (addresses
   MEDIUM-4).
