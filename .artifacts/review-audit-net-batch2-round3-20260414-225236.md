# Net audit — batch 2, round 3

**Dimension:** 边界输入 & 恶意 payload (boundary/malicious input defenses)

**Scope:** HTTP parser edge cases, WS codec fragment/mask handling, WS
handshake response validation, UDP datagram boundary.

## Findings

### MED-1 — `std::string(sv)` remnants in ws_handshake.hpp
`eph-net/include/eph/net/detail/ws_handshake.hpp:421, 457, 475, 496`

Same class as batch2-round2 MED-2 — four `std::string(std::string_view)`
wrappers inside SPDLOG arguments. spdlog/fmtlib accepts `string_view`
directly; the wrappers are pure allocations.

  - line 421: WARN on non-101 response status (error path)
  - line 457: WARN on Sec-WebSocket-Accept mismatch (error path)
  - line 475: WARN on unsolicited Sec-WebSocket-Extensions (error path)
  - line 496: INFO on handshake success (**happy path**) — runs on every
              WebSocket connection

Line 496 is particularly visible: every WS connect allocates two temporary
`std::string`s just to format the SPDLOG_INFO line. Drop the wrappers.

### LOW-1 — `emit_pong` relies on upstream bound, no defensive assert
`eph-codec/include/eph/codec/ws_codec.hpp:381-384`

```cpp
uint8_t unmask_buf[ws::kMaxControlPayloadLen];  // 125 bytes
...
if (ping.masked && ping.payload && ping.payload_len > 0) {
    std::memcpy(unmask_buf, ping.payload, ping.payload_len);
```

`ping.payload_len` comes from `decode_frame` which already rejects control
frames with `payload_len > 125` (see `kMaxControlPayloadLen` check at
websocket.hpp:631). So in practice `payload_len <= 125` and the memcpy is
safe. Still, this is relying on an invariant maintained by a separate
component — a defensive assert or guard would be worth one line.

Low priority — correct today, only a hardening improvement.

### NIT-1 — `std::string(sv)` in http.hpp WARN site for response-no-CL
`eph-net/include/eph/net/http.hpp` — already handled in round 2.

### LOW-2 — TCP scheme `is_valid_close_code` rejects 0 (no-code) correctly
`eph-net/include/eph/net/detail/websocket.hpp:135`

Scanned as a false positive: `is_valid_close_code(0)` returns false, which
is correct because RFC 6455 §7.4 mandates the echoed code must be a valid
status code (not the "no code provided" sentinel 1005). The codec's
emit_close already substitutes `kNormal` / `kProtocolError` before calling
`is_valid_close_code`, so the path is safe.

No fix needed.

## Remediation plan this round

Fix MED-1 (drop remaining `std::string(sv)` wrappers in ws_handshake.hpp).
Skip LOW-1 (hardening, not a bug) and LOW-2 (not a bug).

Only one fix this round — most of the boundary defenses are already in
place (WsFrameBad, CodecOverflow, is_valid_close_code, D-1 TE rejection,
CRLF injection protection, kMaxHeaderLineLength cap, kMaxPayloadLen +
decode_frame incomplete check). The RFC 6455 / RFC 7230 compliance surface
is genuinely tight already; round 3 returns one small cleanup finding.

No DPDK-touching changes.
