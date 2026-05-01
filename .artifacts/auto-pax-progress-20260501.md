# Auto-pax progress (2026-05-01)

> Started by user instruction: 5 pax (1 review + 4 fix + 1 doc) auto, no human in the loop.
> Branch: main. NOT pushed until user reviews.

## Plan (frozen at start)

| # | Task | Mode | Status |
|---|------|------|--------|
| 0 | review (mp mental model gaps) | --auto | ✅ done — `.artifacts/review-mp-mental-model-20260501.md` |
| 1 | fix Vuln 1 lcore conflict detection (ABI v2 + lcore_mask) | --auto | ✅ done — commit `fceb0f78` (library half; bench wiring deferred) |
| 2 | fix Vuln 5 JSON filename pid suffix | --auto | ✅ done — commit `d849d58a` |
| 3 | fix Vuln B+C lat wrapper mockex exit check | --auto | ✅ done — commit `3b94a09a` |
| 4 | fix Vuln 2 stale slot pid probe (extends v2 ABI) | --auto | ✅ done — commit `1b2d13f8` |
| 5 | fix Vuln A envvar parse strictness | --auto | ✅ done — commit `c7989efb` (executed early; combined w/ fix 4 plumbing intent) |
| 6 | doc same-scenario fan-out semantics | --auto | ✅ done — commit `f2e45a0d` |
| ~~ | ~~fix mockex multi-client mixing~~ | DROPPED | False alarm (per-conn isolated) |
| ~~ | ~~fix pin_to_queue silent fallback~~ | DEFERRED | Already hard-errors; remaining risk needs runtime probe |

## Pre-set decisions (auto mode will use these)

### Fix 1: lcore conflict
- ABI: hard bump MpRegistryVersion = 2
- lcore_mask: uint64_t (caps at 64 lcores, matches MpTopology::kMaxProcs)
- API: extend MpTopology::ProcSpec with lcore_mask field
- Diagnostic: list slot index + tag + colliding lcore IDs
- Scope: EAL lcore only (BENCH_CLIENT_CPU is bench-app concern, not library)

### Fix 2: pin_to_queue
- Direction: failure → Error::InvalidConfig hard error
- Opt-out: cfg.dpdk.allow_pin_fallback (default false)
- Mark BREAKING in CHANGELOG

### Fix 3: kill-9 stale slot
- Approach: attach-time kill(pid, 0) probe (no background thread)
- ProcSlot += pid_t pid (combined with fix 1 ABI bump → single v2 schema)
- On stale slot detected: CAS preempt + WARN log

### Fix 4: mockex multi-client
- Direction: doc + runtime WARN (not data isolation; that's feat scope)
- WARN: when 2nd concurrent client connects to same scenario port

### Doc: same-scenario fan-out
- Add "Same-scenario fan-out" section to dpdk-mp-teardown-protocol.md

## Discipline

- Each task end → run focused regression (test_mp_registry + test_dpdk_e2e at minimum)
- Each commit independent + revertable
- Failure → record + continue next task (auto-mode unblocks per pax doctrine)
- ABI conflicts between fix 1 & fix 3 → merged into single v2 schema (already coordinated)

## Timeline

| Time | Event |
|------|-------|
| 06:49 | progress file initialized; starting review |
| 07:05 | review done; 5 known vulns: 4 confirmed + 1 false alarm; 3 NEW vulns found |
| 07:05 | fix list updated: 5 fix + 1 doc (was 4 fix + 1 doc) |
| 07:05 | starting fix 1: Vuln 1 lcore conflict (ABI v2 + lcore_mask) |
| 07:35 | fix 1 done; commit `fceb0f78`; 33/33 unit PASS; bench wiring deferred |
| 07:35 | fix 2 starting: Vuln 5 JSON filename pid suffix |
</content>
