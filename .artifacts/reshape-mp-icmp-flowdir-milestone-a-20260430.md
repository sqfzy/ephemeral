# Milestone A complete — Cross-process ICMP MTU propagation

- Date: 2026-04-30 05:50 UTC
- Branch: `reshape/mp-icmp-flowdir`
- HEAD at milestone A: `7364ea70`
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`
- Baseline: `.artifacts/reshape-mp-icmp-flowdir-baseline-20260430.md`

## Stage map (each = one commit)

| Stage | Commit | Subject |
|-------|--------|---------|
| 0 | 418b96fd | baseline snapshot (70 cases + 2 e2e + bench) |
| 1 | 1d6b39e7 | feat(mp_ipc): typed RAII rte_mp_* IPC wrapper |
| 2 | dacf7b57 | feat(icmp_directory): hugepage POD directory |
| 3 | 7364ea70 | feat(dpdk): cross-process ICMP forwarding (core) |

## Diff stats (milestone A)

```
 stage 1 (mp_ipc):           +488 / -0   (2 files: header + test)
 stage 2 (icmp_directory):   +762 / -0   (2 files: header + test)
 stage 3 (core integration): +990 / -11  (8 files: 5 modified, 3 new)
                             ─────────────
 milestone A total:         +2240 / -11
```

## Verification — all green

| Check | Result |
|-------|--------|
| `xmake build -g tests` (full) | OK |
| `test_icmp_dispatch` | 10 / 10 PASS (baseline, unchanged) |
| `test_mp_registry` | 13 / 13 PASS (baseline, unchanged) |
| `test_mp_topology` | 20 / 20 PASS (baseline, unchanged) |
| `test_dpdk_multiprocess_config` | 27 / 27 PASS (baseline, unchanged) |
| `test_mp_ipc` (NEW) | 12 / 12 PASS — pack/parse + MpIpcAction RAII + degrade |
| `test_icmp_directory` (NEW) | 17 / 17 PASS — POD layout + register/lookup/gen + RAII |
| **unit total** | **99 / 99** |
| `dpdk_mp_e2e.sh` (legacy invariant) | PASS — primary rc=0, secondary rc=0 |
| `dpdk_mp_topology_e2e.sh` (legacy invariant) | PASS — primary rc=0, secondary rc=0 |
| `dpdk_mp_icmp_e2e.sh` (NEW) | **PASS** — secondary IPC forward → primary cb fired with MTU=1280 |

## Bench parity (force-rebuilt vs baseline)

| Metric | Baseline | Milestone A | Delta |
|--------|----------|-------------|-------|
| `lat_tcp_dpdk` RTT p50 | 22247 ns | 21575 ns | **−3.0 %** |
| `lat_tcp_dpdk` RTT p99 | 70493 ns | 27191 ns | −61 % (baseline noise) |
| `lat_tcp_dpdk` throughput | 35410 sps | 45365 sps | +28 % |
| `lat_udp_dpdk` RTT p50 | 19575 ns | 19639 ns | +0.3 % |
| `lat_udp_dpdk` RTT p99 | 26711 ns | 27335 ns | +2.3 % |

Both within sample noise. Hot path is byte-for-byte unchanged
(verified: stream attach is the only Platform path that touches the
ICMP closure, and the closure body only runs on actual ICMP receive).

## Architecture proven

The full forwarding round-trip works end-to-end:

```
secondary (proc 1):
  synthesized Frag Needed
       ↓
  IcmpDirectory::lookup(tuple, proto)
       ↓ owner_proc=0 (primary)
  mp_ipc_send_oneway("eph_icmp_dispatch", IcmpDispatchMsg{...gen=N})
       ↓
       ─── DPDK rte_mp socket ───
       ↓
primary (proc 0) — DPDK IPC thread:
  on_icmp_dispatch_thunk(msg)
       ↓ parse_payload<IcmpDispatchMsg>
       ↓ gen check: directory[slot].gen == N ? yes
       ↓ rebuild ParsedIcmp essentials
  IcmpRegistry::dispatch(parsed)
       ↓ tuple match
  icmp_test_cb(stream, mtu=1280)
       ↓
  TEST OBSERVES: g_observed_mtu = 1280 ✓
```

## Decisions cascade (KD-1..KD-8 from plan)

All eight key decisions held up under construction:

* KD-1 (IcmpDirectory parallel to IcmpRegistry, not replacement) → kept
  IcmpRegistry untouched, all 10 baseline test_icmp_dispatch cases
  pass byte-for-byte.
* KD-2 (per-action IPC handlers) → only one action so far
  (`eph_icmp_dispatch`); milestone B will add `eph_fd_install` and
  `eph_fd_destroy` parallel to it.
* KD-3 (24-byte IcmpDispatchMsg) → adjusted to 32 bytes after
  alignas(8) padding, well within 256-byte DPDK msg cap. Static
  assert pinned.
* KD-5 (per-slot gen, no wraparound handling) → uint32 gen field in
  every entry; wraparound documented but not handled. HFT scenario
  unregister rate puts wrap at ~13 years.
* KD-7 (fire-and-forget for ICMP, sync for FlowDir) → fire-and-forget
  used for ICMP forward as planned; sync IPC will arrive in
  milestone B.
* KD-8 (preserve invariants) → IcmpRegistry public API + behaviour
  byte-for-byte, all baseline tests pass without modification.

## What's next

Milestone B (FlowDir secondary fallback) — stages 5-8:
* Stage 5: `FlowRule::handle` becomes `std::variant<monostate,
  LocalHandle, RemoteHandle>`; RAII destruct visit-dispatches to
  rte_flow_destroy or IPC destroy.
* Stage 6: `eph_fd_install` / `eph_fd_destroy` IPC handlers in
  primary; secondary's `Platform::Impl` keeps a request_id-keyed map
  of remote-owned rules.
* Stage 7: `install_flow_rule` learns try-secondary-then-fallback.
* Stage 8: B milestone validation + docs + CHANGELOG.

Stage 9: examples/simple_hft_dpdk_mp annotation + final retrospective.

Milestone A is independently shippable — opening a PR at this point
is valid even without milestone B.
