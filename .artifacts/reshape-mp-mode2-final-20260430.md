# Final retro — reshape/mp-mode2-dynamic (autojoin)

- Date: 2026-04-30 07:56 UTC
- Branch: `reshape/mp-mode2-dynamic` (5 commits off `3676878b`)
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`
- Baseline: `.artifacts/reshape-mp-mode2-baseline-20260430.md`

## What shipped

A new `Platform::join_dynamic` factory: zero-coordination MP
bring-up. Two unrelated processes share one NIC by agreeing on PCI
BDF + nb_rx_queues only — no shared file_prefix, no manual
`MpTopology`, no manual `self_index`. First peer becomes primary;
later peers auto-attach as secondaries and CAS-claim the lowest
free `procs[].claimed` slot.

Naming: per user feedback, "Mode 1 / Mode 2" was rejected as
semantically empty. The two paths are now consistently named
**declarative** (`create_primary` / `create_secondary` +
`MpTopology`) and **autojoin** (`join_dynamic`). Documentation,
comments, commit messages, CHANGELOG all use these names.

## Stages — committed

| # | Commit    | Subject                                                    |
|---|-----------|------------------------------------------------------------|
| 0 | `f5adb3f` | chore(reshape/mp-mode2-dynamic): stage 0 baseline snapshot |
| 1 | `51cc2e3` | feat(dpdk): Mode 2 detail helpers — bdf sanitize + registry self-claim |
| 2 | `53ab361` | feat(dpdk): Platform::join_dynamic — autojoin MP factory   |
| 3 | (this set)| test(dpdk): autojoin e2e — dpdk_mp_dynamic_{primary,secondary} |
| 4 | (this set)| docs(dpdk): TL;DR switches to autojoin + new example + CHANGELOG |

(Stages 1's commit message and stage 0 still say "Mode 2" in
their bodies — preserved as historical record. From stage 2
onward all naming is "autojoin / declarative".)

## Verification — all green

### Unit suites (build + run)

| Suite | Cases | Result |
|-------|-------|--------|
| test_icmp_dispatch         | 10  | PASS |
| test_mp_registry           | 19  | PASS (was 13, +6 for autojoin helpers) |
| test_mp_topology           | 20  | PASS |
| test_dpdk_multiprocess_config | 27 | PASS |
| test_mp_ipc                | 12  | PASS |
| test_icmp_directory        | 17  | PASS |
| test_flow_rule_variant     | 10  | PASS |
| test_fd_ipc_handlers       | 6   | PASS |
| test_flow_steering         | 51  | PASS |
| test_bdf_sanitize **(new)**| 11  | PASS |
| **Total**                  | **183** | (baseline was 166, +17 new cases) |

### E2E (real NIC, vfio-pci, AWS aarch64 ENA)

| E2E                                | Baseline | After |
|------------------------------------|----------|-------|
| dpdk_mp_e2e.sh                     | PASS     | PASS  |
| dpdk_mp_topology_e2e.sh            | PASS     | PASS  |
| dpdk_mp_icmp_e2e.sh                | PASS     | PASS  |
| dpdk_mp_fd_fallback_e2e.sh         | PASS     | PASS  |
| dpdk_mp_dynamic_e2e.sh **(new)**   | —        | PASS  |

### Bench parity (30s-equivalent samples, payload 256, NIC_B vfio-pci)

#### lat_tcp_dpdk

| Metric | Baseline ns | Final ns | Δ      |
|--------|-------------|----------|--------|
| p50    | 21,767      | 22,151   | +1.8%  |
| p99    | 27,591      | 28,695   | +4.0%  |
| p99.9  | 38,830      | 35,086   | -9.6%  |

Long-sample (255s, 11.2M samples) within the same window:
- p50 = 21,655  (-0.5% vs baseline)
- p99 = 27,399  (-0.7%)
- p99.9 = 38,190 (-1.6%)

#### lat_udp_dpdk

| Metric | Baseline ns | Final ns | Δ      |
|--------|-------------|----------|--------|
| p50    | 19,495      | 19,543   | +0.2%  |
| p99    | 69,149 *    | 26,535   | n/a    |
| p99.9  | 72,093 *    | 52,654   | n/a    |

(* Baseline UDP p99/p99.9 already flagged as "high — sample noise".)

**Verdict: zero hot-path regression** (autojoin path is cold;
post-init `Impl` byte-identical to declarative).

## Invariant audit — every "❌ MUST NOT TOUCH" preserved

| Surface                                              | State |
|------------------------------------------------------|-------|
| `Platform::create` / `create_primary` signature      | unchanged |
| `Platform::create_secondary` public signature        | unchanged (delegates to private impl) |
| `PlatformConfig` fields                              | unchanged |
| `MpTopology` public API                              | unchanged |
| `MpRegistryHandle::create_primary` signature         | unchanged |
| `MpRegistryHandle::attach_secondary` callers         | unchanged (3rd param defaulted) |
| `IcmpDirectoryHandle` / `RemoteFlowRulesMap` / `MpIpcAction` | unchanged |
| Hot path: `Stream::send` / `process_burst` / `inc_<M>` | unchanged |
| DPDK mempool / port lifecycle                        | unchanged |
| 5 examples (`simple_hft_dpdk_mp` etc.)               | 0 breaking changes; only doc-comment cross-link added |

## What I'd do differently next time

1. **Auto-emit `--proc-type=auto` in `EalConfig` when `proc_type_set=false`**
   would have saved one stage-3 e2e debug round. Currently the field
   `proc_type_set=false` means "don't emit the flag", which makes
   DPDK default to `primary` (not auto). Adding `ProcType::Auto` was
   the right fix; but it would have been visible earlier with a
   comment in `build_eal_argv` warning that "no flag = primary, not
   auto".

2. **The shell `pgrep -af '<keyword>'` busy-detection pattern is fragile**
   when the keyword appears in any tool wrapper's argv (the Claude
   shell wrapper's eval string contained "dpdk_mp_dynamic", causing
   the script to false-positive on itself). `pgrep -x <exe-name>`
   (matches against `comm`, not cmdline) is the correct primitive.
   Worth back-porting to the other 4 e2e shells when they start using
   shared prefixes.

3. **Auto-derived `file_prefix` collides across runs by design** —
   two consecutive invocations of `dpdk_mp_dynamic_e2e.sh` share
   `/var/run/dpdk/eph_0000_28_00_0/`. Stage 3 needed both startup
   and post-success cleanup of the runtime dir to be idempotent.
   Declarative-path shells dodge this with PID-suffixed prefixes.

4. **`Platform::create_secondary` refactor went from public-only to
   public-thunk + private impl** to thread the `registry_preclaimed`
   flag without changing the public signature. Clean outcome but the
   intermediate plan that suggested "add `attach_secondary` 3rd param"
   wasn't sufficient by itself; the impl-flag was needed too. Worth
   noting in future "thread an internal flag through a public path"
   reshape templates.

## Follow-ups deliberately deferred

- **Migrate every DPDK example + benchmark to `join_dynamic`**
  (user-requested follow-up at end of stage 4). Tracked as the
  next task. Will require touching `simple_hft_dpdk_mp`,
  `simple_hft_dpdk_rss`, `dpdk_multicast_md`,
  `multi_port_platform_demo`, `binance_latency` and the
  `lat_*_dpdk` benchmark targets. Each migration has to clear
  the same bench-parity gate as this reshape.

- **lcore consensus for autojoin** — currently the caller still
  passes `lcores` to `JoinDynamicConfig` and is responsible for
  picking disjoint sets per process. Auto-deriving lcores would
  be a separate, larger reshape (it touches every DPDK
  benchmark / example bring-up).

- **Mode 1 deprecation** — explicitly out-of-scope per KD-8.
  Declarative path stays as Advanced usage, no `[[deprecated]]`
  tag.

## Branch / commit summary

```
51cc2e3 -> 53ab361 -> [stage3 commit] -> [stage4 commit] -> [stage5 commit]
                               ↑                                  ↑
                       autojoin e2e PASS                  retro + bench parity
```

Ready to merge.
