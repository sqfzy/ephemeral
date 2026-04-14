# Net audit — batch 2, round 1

**Dimension:** Concept 契约合规 (Stream / Datagram / Poller / PacketView)

**Scope:** `eph-net/include/eph/net/concepts.hpp`,
`eph-net/include/eph/net/test/*.hpp`, `eph-net-kernel/include/eph/net/kernel/*.hpp`,
`eph-net-dpdk/include/eph/net/dpdk/*.hpp`, `eph-core/include/eph/core/packet_view.hpp`.

## Findings

### HIGH-1 — `FakeStream::PacketView` does not satisfy `core::PacketView`
`eph-net/include/eph/net/test/fake_stream.hpp:55-61`

The nested `FakeStream::PacketView` struct exposes only `data()` and `length()`.
The formal `eph::core::PacketView` concept (eph-core/include/eph/core/packet_view.hpp:58)
additionally requires **`writable_data()`, `trim_front(n)`, `trim_back(n)`, and
`arrival_tsc()`**, all of which the real kernel `SpanView` and DPDK `MbufView`
implementations provide (and statically verify). Consequence: any test that
composes a real `StreamCodec<C>` over `FakeStream` and passes
`FakeStream::PacketView` into `C::decode(view)` will fail to compile because
the codec expects to call `trim_front` / `writable_data` on the view.

This is a **semantic drift between mock and real backends** — exactly the class
of drift Round 1 is scoped to catch. Fix by promoting `FakeStream::PacketView`
to a proper `core::PacketView`-conforming cursor (head/tail indices, TSC field).

### HIGH-2 — `FakeDatagram::PacketView` does not satisfy `core::PacketView`
`eph-net/include/eph/net/test/fake_datagram.hpp:39-45`

Same drift as HIGH-1 on the Datagram side. `FakeDatagram::PacketView` is a
2-field stub; real backends (`SpanView`, `MbufView`) satisfy the full
`core::PacketView` contract. Fix by sharing the same conforming type as HIGH-1
or providing an independent but equally conforming implementation.

### MED-1 — `KernelTcpStream::close_gracefully` ignores `shutdown(2)` result
`eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:556-564`

```cpp
[[nodiscard]] std::expected<void, core::ErrorInfo>
close_gracefully() noexcept {
    if (sock_.fd() >= 0) {
        ::shutdown(sock_.fd(), SHUT_WR);
    }
    state_ = TcpState::FinWait1;
    ...
    return {};
}
```

`::shutdown` can fail with `ENOTCONN` (peer already closed), `EBADF`, `EINVAL`,
etc. The current code:
  - ignores the return value entirely;
  - does not log the errno on failure;
  - unconditionally flips `state_` to `FinWait1` even on hard failure.

Per CLAUDE rules ("Log all error branches"), the failure path needs at least a
WARN log with the errno. `ENOTCONN` is the expected "peer already closed"
case and should be tolerated silently (or DEBUG-logged), but other errnos
(EBADF / EINVAL) indicate a bug or a race and deserve WARN + error return.

### MED-2 — `KernelTcpStream::close_gracefully` does not flush TLS `close_notify`
`eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:554-564`

For TLS streams, RFC 5246 §7.2.1 (and TLS 1.3 §6.1) require sending a
`close_notify` alert before shutting the TCP half. The current implementation
issues `SHUT_WR` directly, which means the peer sees a truncated TLS session
and may log a "peer closed without close_notify" warning. `close_gracefully`
claims to do a clean shutdown — it should emit `close_notify` via the TLS
layer before SHUT_WR when TLS is enabled.

Scope note: fixing this properly requires exposing a `close_notify` emitter on
`TlsSession`, which is out of scope for a concept-compliance round. Record as
a known gap and escalate to a TLS-focused round (dimension 4).

### LOW-1 — Real backend types lack file-scope concept `static_assert`s
`eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp`,
`eph-net-kernel/include/eph/net/kernel/udp_socket.hpp`,
`eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp`,
`eph-net-dpdk/include/eph/net/dpdk/udp_socket.hpp`

The static_asserts that `KernelTcpStream<WsCodec>` satisfies `Stream` live
only in the **test files**. This means a signature drift in the stream header
will compile fine until the test is built. The fake backends (`FakeStream`,
`FakeDatagram`) carry header-local static_asserts and do catch drift on
inclusion.

Mitigating: in practice the tests are always built before merge, so this is
only a developer-experience drawback (lag between break and error). Fix would
be to add a header-local `static_assert` that instantiates the template with a
minimal codec (e.g. `RawStreamCodec`). Downside: would force the kernel
header to pull in `eph-codec`, breaking the deliberate module-dependency
layering. Leave as-is; document as a low-priority NIT.

### NIT-1 — `TestPoller::poll` is O(n²) in registered pollable count
`eph-net/include/eph/net/test/test_poller.hpp:97-112`

For each snapshot entry, `TestPoller::poll` does a linear search through
`registered_` to check whether the pollable is still registered. Not a bug
(n is expected to be small in tests), but unnecessary work. A scoped
`iterating_` flag + deferred erase would match `KernelPoller`'s shape.

Leave alone — NIT only.

## Remediation plan this round

Fix HIGH-1, HIGH-2, MED-1. Skip MED-2 (out of scope, document for a future
TLS round). Skip LOW-1 / NIT-1 (design trade-offs).

DPDK path is **not** touched this round — HIGH-1/2 are eph-net test headers,
MED-1 is kernel-only — so no DPDK-build risk.
