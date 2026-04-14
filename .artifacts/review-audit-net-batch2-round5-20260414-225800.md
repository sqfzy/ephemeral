# Net audit — batch 2, round 5

**Dimension:** DPDK 特异性 (mbuf lifecycle / mempool / codec error handling)

**Static-audit only**: no DPDK build verification because the
`/tmp/gcc14-wrap/g++` wrapper required for the vcpkg-libssl vs aws-lc
-isystem reordering has been wiped by a WSL /tmp cleanup. DPDK TLS path
code changes in this round are not locally built. The specific changes
proposed here stay narrow and mechanical.

## Findings

### MED-1 — Codec decode error in DPDK TLS drain does not escalate
`eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:807-812`

Inside the `process_records_in_place` callback, when `codec_.decode()`
returns an error:

```cpp
if (!dr) {
    SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
        "DpdkTcpStream::drain_codec_(TLS): decode err={}",
        dr.error().detail);
    tls_codec_pending_.clear();
    return;
}
```

The lambda returns silently but the outer `process_records_in_place`
continues feeding subsequent records, which then re-enter the same
broken codec and re-fail. The equivalent kernel path at
tcp_stream.hpp:744-748 flips `state_` to `TcpState::Closed` on decode
error so the connection is torn down; the DPDK path only logs.

The consequence: a WS protocol violation on a DPDK stream causes a
WARN flood as every subsequent record re-triggers the error, instead
of tearing down the session for the reconnect policy to handle.

Fix: latch a `codec_err_latched_` bool in the lambda and, after
`process_records_in_place` returns, reset the session so the
reconnect loop can establish a fresh session (same pattern as the
existing `reasm_overflowed_` escalation at line 856-857).

### LOW-1 — DPDK `close_gracefully` logs a bare `r.error()` not `.detail`
`eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:623-625`

```cpp
SPDLOG_LOGGER_WARN(detail::tcp_stream_logger(),
    "DpdkTcpStream::close_gracefully: TcpSession::close err={}",
    r.error());
```

The kernel path logs `.error().detail` (a `const char*`). Here the
logged object is whatever `TcpSession::close()` returns as its error
type. Without checking TcpSession, the format of the `{}` output is
uncertain. Not a correctness bug, just a potential readability issue.

Skipping — would need to trace TcpSession to confirm.

### INFO — DpdkUdpSocket codec decode error already WARN-logged
`eph-net-dpdk/include/eph/net/dpdk/udp_socket.hpp:338-342`

Per-mbuf datagram decode errors are WARN-logged and the mbuf is freed.
This is correct for UDP (datagram-oriented, no session to tear down).
No fix.

### INFO — MbufView / SpanView semantic parity confirmed
`eph-net-dpdk/include/eph/net/dpdk/detail/mbuf_view.hpp:56-73`
vs `eph-net-kernel/include/eph/net/kernel/detail/span_view.hpp:45-47`

The DPDK `MbufView::trim_front/back` have bounds-checking (clamp at
length_) while the kernel `SpanView::trim_front/back` require the
caller to guarantee `n <= length()`. This is a deliberate defensive
choice on the DPDK side because NIC-sourced bytes are less trusted.
Semantic parity is maintained — both conform to `core::PacketView`.

No fix.

## Remediation plan this round

Fix MED-1 (escalate DPDK TLS codec decode error to session reset).
Skip LOW-1 (readability-only). No DPDK local build — report notes
the risk explicitly. Kernel-only tests do not cover the DPDK TLS
drain path so the fix is pure static code change.

Scope note: DPDK edits CANNOT be locally verified this round (see
environment caveat). The change is mechanically parallel to the
existing `reasm_overflowed_` + `sess_.reset()` escalation that is
already proven correct in the same file.
