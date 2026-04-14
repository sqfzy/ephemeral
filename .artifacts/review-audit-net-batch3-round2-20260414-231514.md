# Net Audit — Batch 3, Round 2 (2026-04-14 23:15 CST)

**Dimension**: Observability gaps — error branch coverage, log levels,
expensive formatting in hot paths, log context (fd / remote / bytes).

**Scope**: `eph-net-kernel/detail/tls_state.hpp`,
`eph-net/detail/http_connect.hpp`, `eph-net-kernel/poller.hpp`.

## Findings

### HIGH-1 — `TlsState` has ZERO logging — every TLS failure is invisible

- **File**: `eph-net-kernel/include/eph/net/kernel/detail/tls_state.hpp` (entire
  file, grep returns zero SPDLOG invocations).
- **Defect**: `TlsState::handshake`, `process_records`, and `encrypt_for_send`
  are the kernel backend's entire TLS surface. When a handshake fails
  (`TlsSession::create` / `session.handshake` / `extract_hot_state` /
  unsupported AEAD key length / `TlsRecordCrypto::create`), the user receives
  an `ErrorInfo` with a static-literal detail string — nothing in the log
  tells them which sub-step failed. When a decrypt fails mid-session
  (`process_records → bad header / TLS decrypt failed`), the stream silently
  tears down with no log breadcrumb.
- **Severity**: HIGH — TLS failures are the #1 production debugging ask.
  Operators currently see "TLS handshake failed" and have no idea whether it's
  a cert chain issue, an SNI miss, or a key-length mismatch.
- **Fix**: add SPDLOG_LOGGER_DEBUG at entry/exit of `handshake()`,
  SPDLOG_LOGGER_ERROR on every failure branch with the specific sub-step
  name, SPDLOG_LOGGER_DEBUG on success with the negotiated cipher key length.
  For `process_records` / `encrypt_for_send` — ERROR-only on failure branches
  (hot path must not pay for log-level checks in the happy path; spdlog's
  macros already compile out below SPDLOG_ACTIVE_LEVEL).

### MEDIUM-1 — `http_connect.hpp` constructs `std::string` for hot-path log
formatting (duplicate of batch2 R3 finding in a different file)

- **File**: `eph-net/include/eph/net/detail/http_connect.hpp:235, 408, 415, 437`
- **Defect**: four sites allocate a `std::string(std::string_view)` just to
  pass to spdlog's `{}` formatter. `{}` accepts `string_view` natively —
  there is no need to convert. Line 408 is inside a WARN branch (release-
  visible) and line 437 is inside the INFO success log (happens on EVERY
  proxy handshake). Each proxy handshake pays at least ONE `std::string`
  allocation just for the log. Batch2 R3 fixed the same bug in
  `ws_handshake.hpp`; this is the sister file that was missed.
- **Severity**: MEDIUM — heap alloc on the connect hot path; not hot-path
  send/recv but still on every reconnect cycle. Per CLAUDE rule: zero-copy
  parsers and no owning string conversions.
- **Fix**: drop the `std::string(...)` wrappers — spdlog / std::format
  handle `string_view` directly.

### MEDIUM-2 — `KernelPoller::poll_impl_` pf-lookup linear scan per event
logs nothing on lookup miss

- **File**: `eph-net-kernel/include/eph/net/kernel/poller.hpp:320-335`
- **Defect**: after `epoll_wait` returns `n` events, the loop looks up each
  event's `obj` pointer in `entries_` by linear scan (expected < 16, OK).
  If the lookup fails (a callback removed itself mid-poll), the code
  silently skips the event. There is no DEBUG log of "orphan event ignored"
  — making it impossible to distinguish a self-removal from a genuine
  dangling-pointer scenario.
- **Severity**: MEDIUM — observability-only.
- **Fix**: SPDLOG_LOGGER_DEBUG at the "orphan event" branch so tests and
  operators can see when a pollable self-removes. Also makes the code more
  defensive — if `pf == nullptr` is ever observed in a hot loop, that's a
  real bug signal.

### MEDIUM-3 — TLS `process_records` bad-header / decrypt-fail returns
ErrorInfo with no context bytes or sequence number

- **File**: `eph-net-kernel/include/eph/net/kernel/detail/tls_state.hpp:248-267`
- **Defect**: when a TLS record header parses as malformed (bad content
  type, length overflow), we return `Error::TlsRecordBad` with only
  `"bad record header"` detail. When AEAD decrypt fails we return
  `Error::TlsCipherFailed` with only `"TLS decrypt failed"`. Neither error
  carries: the record content type, declared payload length, input offset,
  or any hint about whether the problem is at the first record or the
  thousandth. Production diagnosis requires strace / tcpdump.
- **Severity**: MEDIUM — coupled to HIGH-1; once logging is in, this is
  addressed by simply including the context in the log.
- **Fix**: bundled with HIGH-1.

### LOW-1 — `format_host_port`'s `target_host` bounds truncation silently
clips DNS labels at 253 chars with no diagnostic

- **File**: `eph-net/include/eph/net/detail/http_connect.hpp:89-90`
- **Defect**: `const size_t host_len = std::min<size_t>(target_host.size(), 253);`
  silently truncates the host name. In practice 253 is the DNS limit so
  any longer string is guaranteed garbage, but we should WARN-log once
  so nobody debugs a mysterious "Host: header says something different"
  issue in prod.
- **Severity**: LOW — pathological input only.
- **Fix**: optional — bundled if cheap.

## Action plan (≤ 6 findings per round — this round has 4 hot)

1. **HIGH-1**: add TlsState observability layer.
2. **MEDIUM-1**: drop `std::string(sv)` in `http_connect.hpp` logs.
3. **MEDIUM-2**: poller orphan-event DEBUG log.
4. **MEDIUM-3**: include context in TlsState error paths (bundled with #1).
5. LOW-1: skip (pathological).
