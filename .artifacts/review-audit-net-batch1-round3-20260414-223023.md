# Net Audit — Batch 1 Round 3

- Dimension: **Dead code / unused config fields / duplicated implementations**
- Date: 2026-04-14 (Asia/Shanghai)
- Scope: `eph-net/`, `eph-net-kernel/`, `eph-net-dpdk/` — hunt for unused
  config fields, dead internal methods, duplicated primitives.

## Summary

**No CRITICAL / HIGH / MEDIUM findings.** The dead-field cleanup was
already exhaustively done in recent refactors:

- `.artifacts/design-drop-stream-reconnect-field-20260414-070500.md`
  removed `StreamConfig::reconnect` and the corresponding backend
  members from both kernel and DPDK backends (commit `4fb76ca`,
  merged 2026-04-14).
- Commits `4bf5ddc`, `68a7980` (2026-04-14) removed two dead
  `PollerConfig` fields from `eph-net-dpdk`.

A linear scan of every public config aggregate
(`eph::net::kernel::StreamConfig`, `UdpConfig`, `PollerConfig`, and
their DPDK counterparts) turns up only fields that are read at least
once in the corresponding `create()` factory or member function.

## Low-severity observations (no action this round)

### NIT-1 — `WsSha1` is a full hand-rolled SHA-1 implementation inside `ws_handshake.hpp`

**File:** `eph-net/include/eph/net/detail/ws_handshake.hpp:84-167`

The header comment explains that we deliberately avoid `aws-lc::SHA1`
so that the handshake path compiles without transitively linking
aws-lc at downstream consumers. This is intentional and not dead
code, but worth noting: the bench mock had a matching implementation
(no longer present in-tree per the comment's "aligns with" phrase).

Defer: the comment is self-explaining and the duplication is a
one-time cost that stays isolated to this header. If a future audit
pass merges TLS and non-TLS handshake paths we could consider
gating SHA-1 behind aws-lc when available.

### NIT-2 — `DpdkPoller<P>` primary template's `~DpdkPoller` is implicit

**File:** `eph-net-dpdk/include/eph/net/dpdk/poller.hpp:538-573`

The `DpdkPoller<P>` primary template wraps a `unique_ptr<DpdkPoller<void>>`
and relies on the implicit destructor. Because `DpdkPoller<void>::~`
already notifies every attached Pollable via `detach_fn`, this is
correct — but slightly surprising on first read. Consider adding a
`~DpdkPoller() = default;` with a comment, or explicitly forwarding
the destructor body, for clarity.

Deferred: cosmetic only. No functional impact.

### NIT-3 — `KernelPoller::poll_impl_` EINTR handling diverges between zero-timeout and blocking

**File:** `eph-net-kernel/include/eph/net/kernel/poller.hpp:300-314`

Zero-timeout `poll()` returns 0 on EINTR (correct — non-blocking
doesn't want to spin); blocking `poll(timeout)` retries (also correct
— EINTR shouldn't shorten a caller-requested wait). The split is
intentional but has no comment explaining the contract. Worth a
one-line comment in the next observability pass.

## Scope decision

No fixes this round. The audit report itself is the deliverable —
this commit proves the dead-code dimension is already clean after
the recent refactors.
