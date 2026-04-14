# Net Audit — Batch 3, Round 4 (2026-04-14 23:24 CST)

**Dimension**: Config validation — which StreamConfig / ProxyConfig /
ws_handshake field combinations should fail early but currently slip
through to a misleading runtime error (or succeed against expectation).

## Findings

### MEDIUM-1 — `ws_extra_headers` not deduped against the mandatory RFC 6455
headers — user can accidentally ship duplicate `Upgrade:` etc.

- **File**: `eph-net/include/eph/net/detail/ws_handshake.hpp:310-319`
- **Defect**: `perform_ws_handshake` pre-populates `hdrs[0..4]` with
  Host / Upgrade / Connection / Sec-WebSocket-Key / Sec-WebSocket-Version,
  then blindly appends `extra_headers`. A caller that passes
  `{"Upgrade", "h2c"}` or a second `Host` header will emit a duplicate
  header; most servers then reject the whole request with a 400 and the
  client sees only `WsHandshakeFailed: parse/Accept mismatch` — the
  *actual* cause (duplicate Upgrade) is invisible.
- **Severity**: MEDIUM — developer-error footgun that surfaces late and
  confusingly in production.
- **Fix**: iterate `extra_headers` once at the top of
  `perform_ws_handshake` and reject any name that collides with a
  mandatory name (case-insensitive). Return
  `Error::WsHandshakeFailed` with detail
  `"ws_handshake: extra header '<name>' conflicts with a mandatory
   WebSocket header"`.

### MEDIUM-2 — `StreamConfig::ws_timeout <= 0` accepted; drives the
handshake to immediate Timeout with no explanation

- **File**: `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:255-261`
- **Defect**: the only validation in `KernelTcpStream::create` is
  `reasm_capacity > 0` + `proxy->validate()`. Fields like `ws_timeout`,
  `connect_timeout` are not sanity-checked. A value of `0ms` passes
  through to `perform_ws_handshake`, where `deadline = now() + 0ms`
  → the first `now() >= deadline` check immediately trips, returning
  `Error::Timeout` with no hint that the caller's config was wrong.
- **Severity**: MEDIUM — UX footgun. Bounds-check should happen at
  the validation boundary, not emerge from deep inside the handshake.
- **Fix**: add a `StreamConfig::validate()` method (or inline checks
  inside `create()`) that rejects `connect_timeout <= 0` and
  `ws_timeout <= 0` with `Error::InvalidConfig` + a clear detail.

### LOW-1 — `reasm_capacity` not validated against the minimum needed for
a single WS handshake over-read

- **File**: `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp:462-472`
- **Defect**: `ws_path` non-empty + `reasm_capacity < sizeof(leftover)`
  path rejects the stream at `leftover copy time` — but only IF there
  was over-read. If there wasn't, a user shipping `reasm_capacity=64`
  with `ws_path="/ws"` silently gets a tiny buffer that dies on the
  first frame. An upfront check against a minimum (say, 512 bytes)
  would catch this.
- **Severity**: LOW — pathological configs only.
- **Fix**: skip for this round; `docs/architecture.md` should document
  the minimum instead.

## Action plan (batch rules: ≤ 6 findings)

1. MEDIUM-1: dedupe `ws_extra_headers` against mandatory names.
2. MEDIUM-2: validate `ws_timeout > 0` and `connect_timeout > 0` in
   `KernelTcpStream::create`.
3. LOW-1: skip (docs-only recommendation).
