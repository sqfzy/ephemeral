# Fix Report — TLS hot-path AEAD ordering bug

## Summary

Fixed a P0 protocol-layer bug introduced by commit `9788c4a8` (2026-04-02
"refactor(transport): split Transport into 3 classes") that caused
**every** TLS+WS connection through `Transport<...>` or
`DirectTxTransport<...>` (the threaded variants) to fail immediately on
the first hot-path record with `bad_record_mac`. Latent for ~7 days,
not caught by CI because no test exercised the
`use_tls=true + WsFramer + Transport::create + real send/recv`
combination end-to-end.

## Timeline

- **2026-04-02** — bug introduced in commit `9788c4a8` during the
  Transport-split refactor. `extract_hot_state` was inlined into
  `TransportCore::do_connect` *before* `do_ws_upgrade` runs, freezing
  the hot-path crypto's TLS sequence counters at zero while the
  subsequent WS upgrade advances SSL's internal counters via
  `SSL_write(GET)` / `SSL_read(101 + NewSessionTicket)`.
- **2026-04-09 17:38** — discovered during code review while explaining
  DPDK transport TLS internals. Discussion (`/discuss`) confirmed
  protocol-layer hard error after 4 rounds with 5 adversarial roles —
  see `discuss-20260409-173817.md`.
- **2026-04-09 17:53** — plan created (`plan-tls-aead-ordering-fix-...md`)
  with 5 phases and 10 key decisions.
- **2026-04-09 17:55** — phase 0 (symptom capture) completed against
  `echo.websocket.org`. Reproduced the bug on the first try:
  `RX 0 packets, 4 decrypt errors, 4 reconnections in 2.8s`. See
  `fix-tls-ordering-symptoms-20260409.txt`.
- **2026-04-09 18:00** — phase 1 (hotfix) completed in commit `4eab3fb`.
  All 5 affected files modified, 865 existing unit tests still pass,
  `simple_hft` against `echo.websocket.org` now stable
  (`RX 4 packets, 0 decrypt errors, 0 reconnections, 22us RTT`).
- **2026-04-09 18:13** — phase 2 (e2e regression tests) completed in
  commit `4751ad7`. New `tests/support/tls_ws_echo_server.hpp` fixture
  + `tests/integration/test_transport_tls_ws_e2e.cpp` with 5 tests
  covering 3 transport variants × {RoundTrip, Reconnect}. Validated
  that 4/5 tests fail when the fix is temporarily reverted, and all 5
  pass with the fix in place.

## Root cause

`TransportCore::do_connect()` was structured as a one-shot
"connect+TLS+key-extract" flow:

```
TCP connect → TLS handshake → extract_hot_state → build TlsRecordCrypto
```

But `Transport::create()` and `DirectTxTransport::create()` then call
`do_ws_upgrade()`, which uses `tls->handshake_write/handshake_read`
(direct `SSL_write/SSL_read` wrappers) to perform the WebSocket HTTP
Upgrade. These advance SSL's application traffic sequence numbers, but
the hot-path `TlsRecordCrypto` is frozen at the snapshot taken before
the upgrade. The next record on the wire is at TLS seq=1+ (or 2+ if
NewSessionTicket was sent), but the hot path tries to decrypt with
nonce(seq=0) → AEAD tag mismatch.

`DirectTransport::do_connect_()` had the correct ordering all along
(TLS handshake → WS upgrade → key extract) — that's why this variant
was the only one that worked. The Transport-split refactor introduced
the inversion only in the new shared `TransportCore::do_connect`.

## Fix

Extracted `TransportCore::arm_aead_crypto()` as a separate method with
an explicit pre-condition: "must be called after all SSL_write/SSL_read
on this session are complete". Called in 4 callsites after
`do_ws_upgrade()`:

  1. `Transport::create` (transport.hpp:209)
  2. `Transport` reconnect lambda (transport.hpp:~1255)
  3. `DirectTxTransport::create` (direct_tx_transport.hpp:139)
  4. `DirectTxTransport` reconnect lambda (direct_tx_transport.hpp:~745)

`DirectTransport::do_connect_()` was refactored to also call
`arm_aead_crypto()` so all 3 transport variants share a single
hot-path crypto initialization implementation (defense in depth
against the same refactor mistake recurring).

`TlsSession::extract_hot_state` doxygen was updated to spell out the
pre-condition explicitly, replacing a self-contradictory comment that
claimed "stays in sync after any SSL_write/SSL_read usage".

## Verification

**Before fix** (commit afaceba, against echo.websocket.org):
```
TLS traffic keys extracted: ... write_seq=0, read_seq=0
EVP_AEAD_CTX_open failed: record_len=24, seq=0
TLS decrypt failed -- triggering reconnect
RX: 0 packets, 4 decrypt errors, Reconnections: 4
```

**After fix** (commit 4eab3fb, against echo.websocket.org):
```
TLS traffic keys extracted: ... write_seq=1, read_seq=2  ← post-WS-upgrade
RX: 4 packets, 32 bytes, 0 decrypt errors, 0 reconnections
RTT: 4 samples, 12.7us min, 22.5us mean, 28.2us max
```

**Unit tests**:
- 865 existing tests across `test_transport`, `test_tls_record`,
  `test_websocket`, `test_transport_e2e`, `test_transport_types`,
  `test_transport_config`, `test_tls_config` — all pass.
- 5 new tests in `test_transport_tls_ws_e2e` — all pass in 108ms total.

**Regression check** (with fix temporarily reverted):
- 4/5 new tests fail with `decrypt_errors > 0` and `wait_recv` timeout
- `DirectTransport_RoundTrip` still passes (its do_connect_ always had
  the correct ordering)
- Confirms the test file is a true regression check, not a tautology.

## Affected types

All `Framer = WsFramer` + `use_tls = true` instantiations of:

- `DefaultTransport<TcpImpl>`         (= `Transport<TcpImpl, WsFramer, 512, 1024>`)
- `SmallTransport<TcpImpl>`           (= `Transport<TcpImpl, WsFramer, 64, 256>`)
- `LargeTransport<TcpImpl>`           (= `Transport<TcpImpl, WsFramer, 4096, 512>`)
- `EvictTransport<TcpImpl>`           (= `Transport<TcpImpl, WsFramer, ..., EvictingQueue>`)
- `DirectTxDefaultTransport<TcpImpl>` (= `DirectTxTransport<TcpImpl, WsFramer, 512, 1024>`)
- `DirectTxSmallTransport<TcpImpl>`   (= `DirectTxTransport<TcpImpl, WsFramer, 64, 256>`)

Over both backends:

- Socket: `eph::net::SocketTransport`
- DPDK: `eph::dpdk::TcpSession<>` (via `DpdkTransport = DefaultTransport<TcpSession<>>`)

Affected examples (all use `Transport<...>` with TLS+WS):
`simple_hft`, `simple_hft_dpdk`, `binance_book`, `ws_echo_client`,
`ws_echo_client_dpdk`, `production_client`.

`DirectTransport<...>` variants (`DirectDefaultTransport`,
`DirectSmallTransport`, etc.) were not affected because they had the
correct ordering all along.

## Commits

- `4eab3fb` fix(transport): defer hot-path AEAD crypto init until after WS upgrade
- `4751ad7` test(transport): add TLS+WS e2e regression tests for AEAD ordering bug

## Follow-ups (not in this PR)

- Nightly CI smoke test that runs `simple_hft` / `binance_book`
  against a local mock TLS+WS server (`tools/mock_binance_server.py`
  with TLS wrapper). The example targets currently aren't in any CI
  group; this fix was discovered by code review, not by CI. The new
  e2e test catches the *class* of bug but doesn't catch
  example-specific wiring mistakes.
- Consider whether `direct_tx_transport.hpp` and `transport.hpp` could
  share more of the connect/reconnect orchestration code — currently
  they each have their own near-identical reconnect lambdas, which is
  exactly the kind of duplication that lets bugs like this hide on
  one side and not the other.
