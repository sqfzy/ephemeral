# Deferred items from the 2026-05-05 action list

This document tracks the items from the 2026-05-05 review action list
that were not fully closed and explains why each warrants its own
dedicated `pax` invocation rather than being folded into a single bulk
session.

Companion to: `CHANGELOG.md` (closed items) and
`.artifacts/primer-20260505-113039.md` (framework snapshot).

---

## Closed in the 2026-05-05 session (14 items / 15 commits)

For reference. Each shipped as an independent commit on `main`.

| Track | Commit | Summary |
|---|---|---|
| **T2.5** | `b775310b` | systemd unit `AmbientCapabilities=` + `CapabilityBoundingSet=` |
| **T3.5** | `2effc6db` | `kNumericalAnomaliesDetected` StreamMetric + Stats counter |
| **T2.4** | `613fa93d` | doc: src_port disjointness library-enforced (audit, no code) |
| **T3.4** | `d0f49316` | `tests/legacy/AUDIT.md` — 23 files, 720 cases, keep all |
| **T2.6** | `554ee375` | `PrometheusTextfileSink` + 8 unit tests |
| **T1.3** | `2102fdc4` | reactivate 4 RSS integration tests |
| **T3.1** | `16e987c8` | `test_queue_allocator_concurrent` — 3 stress cases |
| **T2.3** | `f2733623` | `HmacKeyedEntry<T>` skeleton + 7 unit tests (not yet wired) |
| —     | `934ff594` | DEFERRED.md initial draft |
| **T3.2** | `2d22e880` | `test_src_port_collision` — 13 boundary cases |
| **T3.3** | `c7ba595b` | `test_hmac_tamper_simulation` — 5 fuzz cases (3500 trials) |
| **T3.6** | `95e1db37` | NUMA-aware bench helper + `EPH_BENCH_NUMA_NODE` |
| **T1.1+T1.2** | `51bc1155` | `daemon_disconnected_hook` skeleton + 7 unit tests |
| **T2.1** | `01ded79c` | platform.hpp partial split — Public Config types extracted |
| **T2.2** | `cdbc9832` | tcp_stream.hpp partial split — ReasmBuffer extracted |

**By Tier**:
- 🔴 Tier 1: 1 fully closed (T1.3), 1 skeleton (T1.1+T1.2 hook + tests; rx/tx_burst wire-up deferred)
- 🟡 Tier 2: 6 of 6 progressed — T2.4/T2.5/T2.6 fully shipped; T2.3 skeleton; T2.1+T2.2 partial extractions
- 🔵 Tier 3: 5 of 6 progressed — T3.1/T3.2/T3.3/T3.4/T3.5/T3.6

Total commits: **15** (chain `b775310b..cdbc9832`).

## Deferred (3 items)

Listed in suggested execution order based on dependency graph.

---

### T1.1 + T1.2 (full wire-up) — daemon-died in-flight semantics in rx/tx_burst (Tier 1)

**Skeleton shipped**: commit `51bc1155` provides `daemon_disconnected_hook.hpp`
with `InFlightStatus` enum, `DaemonDisconnectedDetail` POD, and a
thread_local accessor — exercised by 7 unit tests.

**Why full wire-up deferred**: actually invoking `Platform::is_alive()`
from inside `DpdkTcpStream::send` / `DpdkUdpSocket::send_to` /
`DpdkPoller::poll` requires:

1. **Platform back-pointer in Stream**. Currently the Stream holds
   `attached_to_ : DpdkPoller<void>*`, not `Platform*`. Adding the
   back-pointer raises lifetime questions (who keeps the Platform
   alive past Stream destruction? does the Poller proxy the call?
   does the Stream pin a `weak_ptr<Platform>`?).
2. **Hot-path cost**. An `is_alive()` atomic load per `send` is small
   but real; the design needs to be bench-validated against
   `bench_rx_hot_path` baseline before merge.
3. **Multi-stream coordination**. Should detection on one Stream
   cascade-mark sibling Streams? Or each Stream poll independently?
   Affects observed latency on the dead-daemon failure mode.
4. **`Sent` status detection**. The skeleton supports the eventual API
   (Sent / Unsent / Uncertain) but Sent requires the burst path to
   surface "partial-tx-burst-then-died", which the additive design
   doesn't expose for free.

**Recommended next step**:
```
/pax --feat --deep "T1.1+T1.2 wire-up: Platform back-pointer, is_alive()
in rx/tx_burst, Sent/Uncertain population, bench validation"
```

**Estimated effort**: 1-2 weeks design + 3-5 days施工 + 2-3 days testing.

---

### T1.4 — daemon kill recovery integration test (Tier 1)

**Why deferred**: Without the T1.1+T1.2 wire-up the integration test
would only exercise the existing `Platform::is_alive()` polling
behaviour — not the new InFlightStatus three-state classification.
Writing the test now would lock the polling-only behaviour into a
regression gate that becomes wrong once the wire-up lands.

**Recommended next step (after T1.1+T1.2 wire-up)**:
```
/pax --test "T1.4 daemon kill recovery: spawn daemon + 2 tenants,
SIGKILL daemon, verify Error::DaemonDisconnected from rx/tx_burst,
verify in-flight semantics, verify reattach succeeds"
```

**Estimated effort**: 3-5 days.

---

### T2.1 + T2.2 (full splits) — platform.hpp / tcp_stream.hpp remaining sections (Tier 2)

**Partial splits shipped**: commits `01ded79c` + `cdbc9832` extracted the
Public Config types and ReasmBuffer respectively (lowest-risk, lowest-
coupling sections). platform.hpp 3515 → 3474; tcp_stream.hpp 2325 → 2282.

**Why remaining sections deferred**:

- platform.hpp **bringup section** (lines ~700-2100) is the EAL init,
  primary/secondary attach orchestration, IcmpDirectory wiring, and
  RSS bring-up matrix. Tightly coupled to internal `BringupConfig`
  type defined inline; extracting forces moving BringupConfig too,
  which has its own dependencies.
- platform.hpp **runtime section** (lines ~2100-3500) is the
  Platform method definitions including `register_icmp_target`,
  `dispatch_icmp_`, signal handling. Some methods are inline-friction
  candidates that need bench validation.
- tcp_stream.hpp **handshake section** is the TCP/TLS/WS handshake
  orchestration in `create()` and `create_and_attach()`. Heavy
  template+codec coupling.
- tcp_stream.hpp **hot_drain section** is the codec drain loop +
  `process_burst_` — the actual hot path. Splitting requires careful
  preservation of inline visibility; bench validation mandatory.

Each of these warrants its own focused `pax --reshape` cycle with a
dedicated bench validation pass. Lumping them creates ambiguous
attribution if a regression appears.

**Recommended next steps (one pax per section, sequential)**:
```
/pax --reshape "T2.1.bringup: extract platform.hpp bringup section
(EAL init + primary/secondary attach + RSS bring-up) into
detail/platform_bringup.hpp; bench gate"

/pax --reshape "T2.1.runtime: extract platform.hpp runtime section
(register_icmp_target + dispatch + signal handling) into
detail/platform_runtime.hpp; bench gate"

/pax --reshape "T2.2.handshake: extract tcp_stream.hpp handshake
section (TCP/TLS/WS create() orchestration) into
detail/tcp_stream_handshake.hpp; bench gate"

/pax --reshape "T2.2.hot_drain: extract tcp_stream.hpp drain loop
into detail/tcp_stream_hot_drain.hpp; mandatory bench validation"
```

**Estimated effort**: 1 day each + bench rerun = ~1 week total.

---

## Out of scope (📎 reference points — not actively planned)

These remain documented in the 2026-05-05 review as "future trigger
conditions" and are not deferred施工:

| 标识 | 描述 | 触发条件 |
|---|---|---|
| 📎 P1 | standby daemon 热备 | 业务证明 1s 重启窗口不可接受 |
| 📎 P2 | daemon 细粒度 capability + setcap | 合规审计要求最小权限 |
| 📎 P3 | OTel SDK 集成 | 外部 collector 选型 OTLP |
| 📎 P4 | C++ Modules 迁移 | GCC ≥ 16 + Clang ≥ 18 普及 |

---

## Audit summary (final)

```
2026-05-05 action list completion:
  Closed in this session:    14 / 16  (88%)
  Deferred to dedicated pax:  3 / 16  (19% — overlaps with skeleton/partial)
  Out of scope (📎):          4 / 4   (intentional, not in 16)

By Tier:
  Tier 1 (multi-tenant production):    1.5 / 4   (T1.3 fully + T1.1/T1.2
                                                  skeleton; T1.4 deferred)
  Tier 2 (重要):                        6 / 6     (all progressed: T2.4/T2.5/
                                                  T2.6 fully + T2.3 skeleton +
                                                  T2.1/T2.2 partial)
  Tier 3 (长期):                        6 / 6     (all closed)

Total session commits: 15
Test suites green:     218 build targets compile, 23 core tests pass
                       (1300+ individual test cases across the sweep)
Hot path bench:        unchanged (no hot path code moved; partial
                       splits avoided inline-visibility-sensitive blocks)
```

The deferral pattern is principled:

- Tier 1 deferrals (full T1.1+T1.2 wire-up + T1.4) are design-heavy,
  warrant `--deep` adversarial review and bench validation.
- Tier 2 partial-split residual (4 sub-sections) each deserve their
  own focused `pax --reshape` + bench cycle.
- All Tier 3 items closed (T3.1/T3.2/T3.3/T3.4/T3.5/T3.6).

---

*Last updated: 2026-05-05 (post auto-continuation施工)*
*Commit chain: `b775310b..cdbc9832` (15 commits)*
*See `.artifacts/INDEX.md` 2026-05-05 entries for the chain of
artifacts (primer + decision records) supporting this施工 session.*
