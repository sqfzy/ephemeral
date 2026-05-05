# Deferred items from the 2026-05-05 action list

This document tracks the items from the 2026-05-05 review action list
that were **not closed** during the 2026-05-05 施工 session and explains
why each warrants its own dedicated `pax` invocation rather than being
folded into a single bulk施工 pass.

Companion to: `CHANGELOG.md` (closed items) and `.artifacts/primer-20260505-113039.md`
(framework snapshot) and the original Tier-classified action list in
`.artifacts/INDEX.md` 2026-05-05 entries.

---

## Closed in the 2026-05-05 session (8 / 16)

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
| **T2.3** | `f2733623` | `HmacKeyedEntry<T>` SKELETON + 7 unit tests (not yet wired) |

## Deferred (7 / 16)

Listed in suggested execution order based on dependency graph + Tier.

---

### T1.1 + T1.2 — daemon-died in-flight semantics + rx/tx_burst 接线 (Tier 1)

**Why deferred**: This is the single largest design item on the action list.
Cannot be reduced to a mechanical change because it requires:

1. **API contract design**: When `Platform::is_alive() == false` mid-burst,
   what does `tx_burst` return? Three-state enum (Sent / Unsent / Uncertain)
   embedded where? Through `core::ErrorInfo::detail`? A new error variant?
   Each has cross-codebase implications.
2. **Multi-role review**: Stream API change → `pax --deep` with at minimum
   R1 risk + R12 system thinking + R10 security (poisoned-app scenario).
3. **All four backends** (DpdkTcpStream / DpdkUdpSocket / KernelTcpStream /
   KernelUdpSocket) must handle the new return uniformly.
4. **Documentation**: `docs/dpdk-reconnect-pattern.md` rewrite of the
   "in-flight bytes" section — currently says "staged separately"
   (`docs/dpdk-reconnect-pattern.md:9-19`).

**Recommended next step**:
```
/pax --feat --deep "T1.1 + T1.2 daemon-died in-flight semantics: 3-state
return (Sent/Unsent/Uncertain), rx/tx_burst is_alive() check wiring, all
4 backend implementations, docs/dpdk-reconnect-pattern.md rewrite"
```

**Estimated effort**: 1-2 weeks design + 3-5 days施工 + 2-3 days testing.

---

### T1.4 — daemon kill recovery integration test (Tier 1)

**Why deferred**: Requires T1.1+T1.2 first (the test exercises the
in-flight semantics that don't exist yet). Independently the test
needs:

1. A daemon-spawn helper that's safe under integration test timeouts
2. Multi-tenant fork pattern (daemon + 2 tenants in 3 processes)
3. Coordinated SIGKILL + reattach assertion machinery

**Recommended next step (after T1.1+T1.2)**:
```
/pax --test "T1.4 daemon kill recovery: spawn daemon + 2 tenants,
SIGKILL daemon, verify Error::DaemonDisconnected from rx/tx_burst,
verify in-flight semantics, verify reattach succeeds"
```

**Estimated effort**: 3-5 days.

---

### T2.1 + T2.2 — mega-file splits (Tier 2)

**Why deferred**: Each is a 2-3 hour mechanical refactor that MUST be
followed by full bench validation against `bench_rx_hot_path` baseline
to prove no inline-visibility regression. Doing both in one shared
施工 session risks混淆 attribution if a regression appears (which
file caused it?) and the bench validation needs hardware time.

- `platform.hpp` 3515 → ~1200 main + 3 detail files
- `tcp_stream.hpp` 2325 → ~900 main + 3 detail files

The split lines are documented in the meta-plan (sections 阶段 B1 / B2):

- `platform.hpp` → `detail/platform_config.hpp` + `detail/platform_bringup.hpp` + `detail/platform_runtime.hpp`
- `tcp_stream.hpp` → `detail/tcp_stream_config_validate.hpp` + `detail/tcp_stream_handshake.hpp` + `detail/tcp_stream_hot_drain.hpp`

**Recommended next step (one pax per file, sequential)**:
```
/pax --reshape "T2.1 split platform.hpp by config / bringup / runtime;
保行为 + bench_rx_hot_path 5% gate"
```
(then T2.2 after T2.1 lands and bench is green)

**Estimated effort**: 1 day each + bench rerun.

---

### E2 / T3.2 — cross-tenant src_port collision negative test (Tier 3)

**Why deferred (and partially redundant)**: T2.4 audit (closed via
commit `613fa93d`) found that `MpTopology::valid()` already enforces
pairwise overlap rejection at `Platform::serve_nic` time. Existing
`test_mp_topology.cpp` has unit-level coverage. T3.2 adds an
integration-level negative test which is genuinely useful but
**lower priority** than what got closed: existing coverage already
catches the regression case at a lower level.

**Recommended next step**:
```
/pax --test "T3.2 add cross-tenant src_port collision integration test:
two-secondary fixture, configure overlapping port_lo/port_hi, verify
second secondary attach fails with InvalidConfig + clear diagnostic"
```

**Estimated effort**: 1-2 days (mostly fixture work).

---

### E3 / T3.3 — malicious-secondary fuzz (Tier 3)

**Why deferred**: Depends on T2.3 full deployment (this commit only
shipped the skeleton). Until `MpRegistry` / `IcmpDirectory` /
`QueueAllocator` actually carry HMAC tags + verify on read, there's
nothing tamper-checked to fuzz against. The fuzz harness can be
written today but would have nothing meaningful to assert.

**Recommended next step (after T2.3 full wiring)**:
```
/pax --test "T3.3 malicious-secondary fuzz: spawn primary with HMAC
key, secondary attaches, deliberately writes random bytes into a
ProcSlot's data region, verify HMAC mismatch detected on next read"
```

**Estimated effort**: 2-3 days (depends on T2.3 wiring).

---

### E4 / T3.6 — NUMA-aware bench + cycle counters (Tier 3)

**Why partially deferred**: The host this施工 session ran on is
aarch64 single-socket (no NUMA boundary to exercise). Adding the
`numa_pin(node)` helper in `bench_helpers.hpp` is straightforward
and could be done blind, but verification — confirming bench
numbers stabilise under NUMA pinning — needs a multi-socket box.
Leaving it for an operator with the right hardware avoids
shipping unverified bench infrastructure.

**Recommended next step (on a multi-socket / dual-socket host)**:
```
/pax --bench --deep "T3.6 NUMA-aware bench: numa_pin helper in
bench_helpers.hpp, EPH_BENCH_NUMA_NODE env hook in bench_rte_ring_vs_bq
+ bench_rx_hot_path, perf cache-miss counter publish, baseline rerun
+ comparison vs unpinned"
```

**Estimated effort**: 1 week (mostly verification cycles on
multi-socket hardware).

---

## Out of scope (📎 reference points — not actively planned)

These remain documented in the 2026-05-05 review as "future trigger
conditions" and are not deferred施工 — they're**future possibilities**:

| 标识 | 描述 | 触发条件 |
|---|---|---|
| 📎 P1 | standby daemon 热备 | 业务证明 1s 重启窗口不可接受 |
| 📎 P2 | daemon 细粒度 capability + setcap | 合规审计要求最小权限 |
| 📎 P3 | OTel SDK 集成 | 外部 collector 选型 OTLP |
| 📎 P4 | C++ Modules 迁移 | GCC ≥ 16 + Clang ≥ 18 普及 |

---

## Audit summary

```
2026-05-05 action list completion:
  Closed in this session:    8 / 16  (50%)
  Deferred to dedicated pax: 7 / 16  (44%)
  Out of scope (📎):         4 / 4   (intentional, not in 16)
  
Tier 1 (multi-tenant production blockers):
  Closed:    1 / 4   (T1.3 RSS reactivate)
  Deferred:  3 / 4   (T1.1 / T1.2 / T1.4 — design-heavy, deserve --deep)

Tier 2 (重要):
  Closed:    4 / 6   (T2.4 / T2.5 / T2.6 + T2.3 skeleton)
  Deferred:  2 / 6   (T2.1 / T2.2 — mechanical but bench-validated)

Tier 3 (长期):
  Closed:    3 / 6   (T3.1 / T3.4 / T3.5)
  Deferred:  3 / 6   (T3.2 / T3.3 / T3.6 — depend on bigger items
                     or multi-socket hardware)
```

The deferral pattern is principled: every Tier 1 deferral is design-
heavy (warrants `--deep` adversarial discussion before施工). Every
Tier 2 deferral is mechanical-but-bench-gated (warrants its own
focused validation cycle). Tier 3 deferrals are dependent or
hardware-gated.

---

*Last updated: 2026-05-05*
*See `.artifacts/INDEX.md` 2026-05-05 entries for the chain of
artifacts (primer + decision records) supporting this施工 session.*
