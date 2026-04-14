# Net audit — batch 2, round 2

**Dimension:** 热路径零拷贝 (hot-path zero-copy, unnecessary allocations)

**Scope:** KernelTcpStream / KernelUdpSocket hot paths, eph-net/http.hpp log
sites, builder/parser allocation sites.

## Findings

### MED-1 — `tls_plain_buf_.erase(begin, begin + plain_off)` in RX hot path
`eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:771-773`

```cpp
// Compact the plaintext buffer: drop everything we consumed.
if (plain_off > 0) {
    tls_plain_buf_.erase(tls_plain_buf_.begin(),
                          tls_plain_buf_.begin() + plain_off);
}
```

Called **every** `poll_once_()` on TLS streams. `std::vector::erase` on the
front shifts `(size - plain_off)` bytes via memmove. For a 16 KiB TLS record
multi-record scenario where the codec consumed the first N bytes this is a
full memmove of the tail on every poll cycle — a hot-path O(n) operation that
should be O(1).

The sister buffer `reasm_` uses `ReassemblyBuffer` with head/tail offsets
(`consume(n)` + `compact()`) precisely to avoid this. The fix is to either
(a) track a plaintext head offset and erase only at the end when the buffer
empties, or (b) switch `tls_plain_buf_` from `std::vector` to
`ReassemblyBuffer`.

Option (a) is the minimal patch — one `std::size_t tls_plain_head_{}` field
and a guard that resets both to zero when the head reaches size.

### MED-2 — `std::string(string_view)` inside hot-path DEBUG/WARN logs
`eph-net/include/eph/net/http.hpp:395, 610, 645, 775, 837`

Five call sites create `std::string(string_view)` to pass to an SPDLOG
format argument. spdlog's underlying fmtlib formats `std::string_view`
natively via `{}` — the `std::string` construction is a pure
allocation-and-copy for no formatting reason.

Sites:
  - line 395: WARN inside `parse_header_block` Content-Length non-numeric
  - line 610: WARN inside `parse_http_request` unsupported version
  - line 645: DEBUG (successful parse) — on every parsed request
  - line 775: DEBUG (successful parse) — on every parsed response
  - line 837: DEBUG (successful build) — on every built request

Line 645 and 775 are on the **successful** parse happy path, so in
SPDLOG_LEVEL_DEBUG builds every single HTTP parse does two `std::string`
allocations per call. Fix: drop the `std::string()` wrapper — pass the
`std::string_view` directly.

### MED-3 — `std::string(reason_phrase)` on happy path
`eph-net/include/eph/net/http.hpp:775`

Same class as MED-2 but called out separately because `reason_phrase` is
typically short (≤ 16 bytes: "OK", "Not Found") and may hit
small-string-optimization, so the alloc cost is lower but still non-zero
compared to passing `string_view` through. Folded into MED-2 fix.

### LOW-1 — `uint8_t buf[65536]` per-poll stack frame in UDP poll
`eph-net-kernel/include/eph/net/kernel/udp_socket.hpp:279`

Every `KernelUdpSocket::poll_once_()` reserves 64 KiB of stack for the max
UDP payload. Stack allocation is effectively free (SP adjustment) but the
frame size forces the L1 cache line for the return address much further
down, and deep call stacks risk SIGBUS on constrained threads.

Consequences are mild — a dedicated poller thread has MB-size stacks. Not
fixing this round.

### NIT-1 — `on_message` std::function dispatch
`eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:242`

`using OnMessage = std::function<void(...)>` forces an indirect call and
potential heap allocation on assignment. For ultra-low-latency HFT this is
the obvious template-based improvement — but it has been discussed in the
v3.3 design spec and deliberately accepted because it keeps the Stream
concept uniform. Leave alone.

## Remediation plan this round

Fix MED-1 (hot-path O(n) erase) and MED-2/3 (drop `std::string(sv)` in
http.hpp log sites). Skip LOW-1 / NIT-1.

DPDK path not touched — all fixes are kernel TCP stream + eph-net header.
