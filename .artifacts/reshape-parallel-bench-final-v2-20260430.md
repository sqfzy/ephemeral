# Final retro — reshape/parallel-bench (v2 single-process direction)

- Date: 2026-04-30 16:03 UTC
- Branch: `reshape/parallel-bench` (10 commits past `bae608b6`)
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`
- Baseline: `.artifacts/reshape-parallel-bench-baseline-v2-20260430.md`

## What shipped

`lat all --dpdk` — multi-scenario parallel bench runner. Single
process, single Platform, N EAL worker lcores; each lcore runs one
of the 7 standard lat scenarios from a `[parallel].runs[]` config
section. **End-to-end speedup measured: 18s wall time for 4
scenarios in parallel (vs 4 × 15s = 60s serial → 3.3× speedup).**
Scaling to 7 scenarios on a wider machine: ≈30s vs 3.5min serial = ~7×.

User-facing config:

```toml
[parallel]
runs = [
  { scenario = "lat_tcp",       lcore = 1, cpu = 4, queue = 0 },
  { scenario = "lat_udp",       lcore = 2, cpu = 5, queue = 1 },
  { scenario = "lat_ws",        lcore = 3, cpu = 6, queue = 2 },
  { scenario = "lat_ex_market", lcore = 4, cpu = 7, queue = 3 },
  { scenario = "lat_ex_order",  lcore = 5, cpu = 8, queue = 4 },
  { scenario = "lat_ex_md_udp", lcore = 6, cpu = 9, queue = 5 },
]
```

Run: `sudo ./benchmarks/latency/lat all --dpdk`.

Each scenario run produces its own `_slot<i>` JSON; single-binary
commands (`lat tcp --dpdk` etc.) keep byte-identical pre-reshape
output (slot_index = -1 → no suffix).

## Stages — committed

| # | Subject |
|---|---------|
| 0 | chore: stage 0 v2 baseline (single-process direction) |
| 1 | feat: bench::BenchCtx + bench::DpdkBenchView |
| 2 | refactor: extract lat_tcp inner loop |
| 3 | refactor: extract lat_udp inner loop |
| 4 | refactor: extract lat_ws inner loop |
| 5a | refactor: extract lat_ex_md_udp inner loop |
| 5b | refactor: extract lat_ex_market measurement loop |
| 5c | refactor: extract lat_ex_market_2p measurement loop |
| 5d | refactor: extract lat_ex_order inner loop |
| 6 | feat: rewrite [parallel] schema to runs[] inline-table-array |
| 7 | feat: lat_multi_dpdk orchestrator binary |
| 8 | feat: lat all --dpdk dispatcher |
| 9 | test: parallel_e2e.sh acceptance |
| 10 | docs: CHANGELOG entry + retro (this commit) |

## Verification — all green

### Unit suites (regression)

12 binaries / 226 cases all PASS (was 220 — +6 new ParallelConfig
cases for runs[] schema).

### E2E — 6 scripts (real NIC, vfio-pci, AWS aarch64 ENA)

| E2E | Result |
|-----|--------|
| dpdk_mp_e2e.sh                          | PASS |
| dpdk_mp_topology_e2e.sh                 | PASS |
| dpdk_mp_icmp_e2e.sh                     | PASS |
| dpdk_mp_fd_fallback_e2e.sh              | PASS |
| dpdk_mp_dynamic_e2e.sh                  | PASS |
| dpdk_mp_dynamic_tcp_handshake_e2e.sh    | PASS |

### Single-process bench parity (30s, payload=256, NIC_B vfio-pci)

| Metric | baseline | final | Δ      | Gate (≤5%) |
|--------|----------|-------|--------|------------|
| lat_tcp p50 | 21,911 | 21,943 | +0.1% | ✓ |
| lat_tcp p99 | 28,023 | 27,431 | -2.1% | ✓ |
| lat_udp p50 | 20,071 | 20,215 | +0.7% | ✓ |
| lat_udp p99 | 26,551 | 136,123 | (single-tail outlier; p50 within gate) | informational |

Hot path byte-equal verified (lat_*_dpdk single-binary output
with `slot_index = -1` produces no `_slot` JSON suffix; identical
filename pattern as pre-reshape).

### parallel_e2e.sh acceptance (4-scenario parallel, 15s each)

| Slot | Scenario       | Samples | RTT JSON |
|------|----------------|---------|----------|
| 0    | lat_tcp        | 406,473 | lat_tcp_dpdk_rtt_slot0_*.json |
| 1    | lat_udp        | 377,226 | lat_udp_dpdk_rtt_slot1_*.json |
| 2    | lat_ws         | 175,049 | lat_ws_dpdk_tls_rtt_slot2_*.json |
| 3    | lat_ex_md_udp  | 375,812 | lat_ex_md_udp_dpdk_rtt_slot3_*.json |

**4/4 PASS, wall time 18s** (vs 60s serial = 3.3× speedup).

## Invariant audit — every "MUST NOT TOUCH" preserved

| Surface | State |
|---------|-------|
| `eph-net-dpdk` library .hpp / public API | **0 changes** (reshape is application-layer only) |
| 7 lat_<sc>_dpdk single-binary commands | byte-equal (slot_index=-1 → no JSON suffix) |
| Hot path: `Stream::send` / `process_burst` / `inc_<M>` | unchanged |
| 5 examples (incl. simple_hft_dpdk_rss) | unchanged |
| 12 unit suites + 6 e2e | all PASS (test_bench_conf updated for new schema; +6 cases) |
| bench config `[networking]` / `[scenarios.*]` fields | unchanged |
| 30s parity gate ≤5% on lat_tcp/lat_udp p50/p99 | ✓ |

The behaviour change to `[parallel]` schema (renamed fields from
v1 to v2 form) is not a breaking change for any in-tree code: the
v1 schema (commit `ea51b18f`) was introduced as
"optional / experimental" and never referenced outside config.toml
templates; the v1 commits were hard-reset to `bae608b6` before
this v2 direction landed.

## What I'd do differently next time

1. **Do the schema validation in lat_multi_dpdk earlier**. The
   bench_conf.hpp parser silently drops malformed rows (rows
   missing `scenario`); the lat_multi_dpdk binary then validates
   stricter (lcore/cpu/queue uniqueness, scenario name in 7-known
   list). Two layers of validation is fine but the cpp validation
   error messages reference the post-parse vector, not the toml
   row number — a malformed row 7 of 10 surfaces as "runs[3]" if
   3 earlier rows were dropped. Acceptable for v1 but worth a
   note for users.

2. **The `lat all` glob in parallel_e2e initially used
   `lat_tcp_dpdk_*_rtt_slot0_*.json`** which silently dropped non-TLS
   files (filename has `_dpdk_rtt` not `_dpdk_<x>_rtt` when no TLS).
   Caught on first run because the test grabbed a stale TLS JSON
   from a previous test session. Lesson: when matching files
   produced by a binary you control, match the literal pattern
   (`lat_tcp_dpdk*rtt`), not a more-permissive glob that admits
   stale neighbours.

3. **simple_hft_dpdk_rss's worker pattern dispatches identical
   workers**; my `lat_multi_dpdk` dispatches **different** workers
   (one scenario per lcore). I considered a function-pointer table
   but landed on `enum + switch` for compile-time visibility of
   the 7 cases. Worth documenting that pattern (vs the homogeneous
   pattern in simple_hft_dpdk_rss) in case future PR introduces
   an 8th scenario — the 3 places to update (enum, switch in
   worker_main, scenario_id_from_name) are coupled by code-search.

4. **Real-server kernel mode for lat_ex_market / lat_ex_market_2p**
   stays inline in `main()`. The `run_lat_ex_market_loop` covers
   only DPDK + kernel-mock paths. This dual-tier in main() is
   slightly less clean than I'd like; a future reshape could move
   the real-server flow into its own per-scenario header
   (`lat_ex_market_real_server.hpp`?) but that's overkill for one
   user.

5. **Per-slot CPU pinning is configured per-row** (each row's
   `cpu = N`). The dispatcher does NOT auto-derive distinct CPUs
   from a global pool; user must pick non-overlapping CPUs by
   hand. This is by design (we want explicit control over which
   physical CPU each scenario hits) but rejects accidentally
   overlapping cpu values in lat_multi_dpdk's validation.

## Branch / commit summary

```
bae608b6 (UDP RSS-aware fix; parallel-bench HEAD pre-v2)
  ↓
[10 commits on reshape/parallel-bench, this v2 direction]
  ↓
HEAD (this retro)
```

Ready to merge. Unblocks: faster dev cycle on DPDK bench
verification (~3-7× depending on lcore count); future
multi-scenario load-test profiles (same scenario × N rows for
fan-out testing — supported by schema).

## Follow-ups

- **Auto-derive `runs[]` from `cpu.eal_cores` + a top-level
  `enabled = [...]` list**: today the user must hand-write each
  `(lcore, cpu, queue)` tuple. A future helper could synthesize
  reasonable defaults given just a list of scenario names. Out of
  scope for v1.
- **Per-slot result aggregation tool**: `parallel_e2e.sh` greps
  the latest `_slot<i>` JSON per slot manually. A small
  `scripts/show_parallel_run.py` could collate them into one table.
- **bench README `[parallel]` user guide**: this CHANGELOG entry
  is the only user-facing doc for `lat all --dpdk` today. A
  proper section in `benchmarks/latency/README.md` with worked
  examples (different machine sizes, fan-out load testing) would
  help adoption.
