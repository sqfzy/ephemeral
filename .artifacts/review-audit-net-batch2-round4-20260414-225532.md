# Net audit — batch 2, round 4

**Dimension:** TLS / WS / HTTP parser 严格性 (strict compliance)

**Scope:** `eph-net/include/eph/net/hmac.hpp`,
`eph-net/include/eph/net/http.hpp` (corner cases),
`eph-net/include/eph/net/detail/websocket.hpp` (close frame /
is_valid_close_code), `eph-net-kernel/include/eph/net/kernel/detail/tls_state.hpp`.

## Findings

### LOW-1 — `HmacSha256Tag::to_hex()` (allocating overload) double-copies
`eph-net/include/eph/net/hmac.hpp:224-230`

```cpp
[[nodiscard]] std::string to_hex() const {
    std::string s(64, '\0');
    uint8_t     buf[64];
    to_hex(std::span<uint8_t, 64>{buf});
    std::memcpy(s.data(), buf, 64);
    return s;
}
```

The function allocates the destination string, writes the hex digits into
a stack buffer, then memcpy's the stack buffer into the string. The
intermediate stack buffer + memcpy is redundant — we can hand `s.data()`
directly to the span-based overload. One less memcpy on the non-hot-path
variant.

This is explicitly called out in the header comment as "do NOT call on
the HFT hot path". Still, cleanup is trivial and non-risky.

### INFO — TLS `close_notify` not emitted by `close_gracefully` (carryover)
`eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:567-597`

Recorded in batch2-round1 MED-2. Fixing properly requires exposing a
`close_notify` emitter on `TlsState` / `TlsRecordCrypto`, which is a
separate round of work. Carryover — not repaired here.

### INFO — WS close code echoing is already RFC-correct
`eph-net/include/eph/net/detail/websocket.hpp:135-142`,
`eph-codec/include/eph/codec/ws_codec.hpp:193-218`

Scanned the close-code handling end-to-end:
  - `is_valid_close_code` correctly allows 1000-1003, 1007-1011, 3000-4999
    and rejects 1004-1006, 1015 (reserved) per RFC 6455 §7.4.1.
  - `emit_close` substitutes `kProtocolError` when the peer's code is
    invalid and `kNormal` when absent — matches the RFC's "MUST NOT
    echo reserved codes" rule.
  - `close_status_code` correctly XOR-unmasks when `masked` is set, using
    `mask_key[0]`/`[1]` — the matching byte positions for offsets 0/1
    per RFC 6455 §5.3.

No finding.

### INFO — HTTP `parse_decimal` overflow guard is tight
`eph-net/include/eph/net/http.hpp:210-222`

The `v > (size_t{1} << 60)` pre-check ensures the subsequent
`v*10 + digit` cannot wrap (`2^60 * 10 + 9 ≈ 1.15e19 < 2^64 ≈ 1.84e19`).
No wrap possible. Combined with the upper-level `kMaxBodySize` cap this
is correct.

No finding.

### INFO — HTTP request-target allows only printable ASCII minus SP
`eph-net/include/eph/net/http.hpp:189-191`

`is_target_char` accepts `0x20 < c < 0x7f`. This rejects UTF-8-encoded
targets (high-bit set). Some exchanges may send unicode paths. Per the
v3.3 pragmatic stance of "HFT uses percent-encoded ASCII targets only,"
this is a deliberate choice and not a finding.

## Remediation plan this round

Fix LOW-1 only (the allocating `to_hex()` double-copy cleanup). The other
cells are info/carryover and do not need a new fix.

No DPDK-touching changes. No kernel-side TLS changes.
