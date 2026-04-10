# Cleanup Report — benchmarks/latency

## Concept

- **Time**: 2026-04-09 → 2026-04-10
- **Scope**: `benchmarks/latency/` + `tests/unit/bench/`
- **Mode**: full /cleanup workflow (audit → discussion → execute → verify → report) with one mid-execution course correction
- **Net commits**: 5
- **Net LOC**: +671 lines (mostly tests + documentation; the value is reliability + observability, not code reduction)

## Executive summary

Started as a "simplify the latency bench code" request expecting LOC reduction. The structural audit predicted ~−629 LOC of cleanable code. Mid-execution verification showed **the structural audit was largely wrong** — 4 of 6 dead-code claims and most of the proposed dedups didn't survive verification:

| Audit claim | Reality |
|---|---|
| `scenario_concept.hpp` is dead (34 LOC) | Used as a template constraint by `runner.hpp` |
| `stream_scheduler.hpp` is dead (149 LOC) | Used by `exchange/mock_ws.hpp` + has unit test |
| `ws_handshake.hpp` has 1 caller | Has 2 callers (`lat_ws.cpp` + `exchange/mock_ws.hpp`) |
| `runner.hpp` has 3 collapsable sweep variants | Already collapsed via `run_rtt_window` helper |
| `BenchConfig` has 39 over-inclusive fields | ~25 fields, well-structured; 415 LOC of `config.hpp` is mostly parser/loader infra |
| lat script has ~85 LOC of trimmable defensive checks | Defensive checks were vindicated by today's wedge state |

User chose to dig deeper rather than abort. A second audit pass found different and more substantive issues — 3 HIGH-severity bugs in the lat wrapper script that were validated by failure modes hit on the bench host this very session, plus 4 documentation/test gaps that the structural audit didn't even attempt to find.

The cleanup pivoted from "delete code" to "fix bugs the audit missed", which is why the LOC stat is positive (mostly new tests + new docs).

## Pre-clean audit summary

- **Structural audit** (`audit-bench-latency-20260409.md`): 4 Critical, 5 Major, 4 Minor — but with 5 false positives discovered during verification.
- **Deep audit** (`audit-bench-latency-deep-20260409.md`): 0 Critical, 0 High → revised to 3 High after further investigation, 4 Medium, 3 Low. Different set entirely.

## Final cleanup scope (5 commits)

| # | Commit | What | Files | LOC |
|---|---|---|---|---|
| 1 | `bcd6e71` | rm dead `core/udp_client.hpp` (only true positive from structural audit) | 1 file deleted | −80 |
| 2 | `5b26eb4` | lat wrapper robustness: H1 sysfs-driven state detection + H2 idempotent `host_to_bench_ns` + H3 wedge state auto-recovery + M4 post-condition retry + new `NIC_B_PCI` config key | `lat`, `bench.conf` | +193 / −43 |
| 3 | `b5d6d1a` | docs: M1 outlier semantics + M2 reproducibility checklist + M3 fix false "no formal unit tests" claim | `README.md`, `summary.md` | +118 / −15 |
| 4 | `f0a7c8d` | unit test for `BenchRunner` (L1) — 12 tests, 5 suites, ~19 s runtime, exercises sweep loops + cancel + TSC inversion | `tests/unit/bench/test_runner.cpp` (new), `xmake.lua` | +334 |
| 5 | `57e90b2` | lint test for 0-caller `core/*.hpp` headers (regression-validated by injecting a dead header) | `tests/unit/bench/test_no_dead_headers.cpp` (new), `xmake.lua` | +164 |

**Total**: 8 files changed, 809 insertions, 138 deletions, **net +671 LOC**.

## HIGH severity findings (validated by today's failure modes)

### H1: lat wrapper couldn't detect "bound to vfio-pci" without `.dpdk_state`

**Reproduction** (hit on first lat invocation today):
- ens35 was permanently dedicated to DPDK on this host
- `.dpdk_state` was missing (it's not in git, only created by `dpdk-setup.sh -y`)
- `detect_nic_b_state()` returned "unknown" → wrapper died with "ens35 not visible in host, bench_ns, or as a vfio device"

**Fix** (commit 2): introduced a `nic_b_pci()` helper that resolves NIC-B's PCI BDF through three fallbacks (bench.conf `NIC_B_PCI` → `/sys/class/net/$NIC_B/device` → cached `.dpdk_state`), then `detect_nic_b_state()` probes sysfs directly. `.dpdk_state` is now a convenience hint, not the source of truth. Added optional `NIC_B_PCI=0000:28:00.0` to `bench.conf` for hosts where NIC-B is invisible to the kernel at startup.

### H2: `host_to_bench_ns()` was not idempotent

**Reproduction**: `ip link set $NIC_B netns bench_ns` fails with "Cannot find device" if NIC-B is already in bench_ns (which happens when state mis-detection sends the wrapper down a stale path).

**Fix** (commit 2): check `ip netns exec bench_ns ip link show $NIC_B` first; skip the move if NIC-B is already there.

### H3: Wedged state (driver bound, no netdev) misclassified as "unknown"

**Reproduction** (also hit today): kernel auto-rebinds ena to PCI device ~30 s after a DPDK process exits, but the netdev never materialises. dmesg shows "renamed from eth0" but `/sys/bus/pci/devices/$pci/net/` stays empty. Manual recovery is `echo $pci > /sys/bus/pci/drivers/ena/{unbind,bind}`.

**Fix** (commit 2): added a `wedged` state to `detect_nic_b_state()` (driver bound + no netdev), plus `unwedge_nic()` helper that issues unbind/bind via sysfs with a 3 s materialisation poll. Called automatically before the transition switch when "wedged" is detected, and as a recovery step inside `dpdk_to_host` post-condition checks.

## MEDIUM findings (doc + observability)

### M1: README didn't explain why max is sometimes huge

The baseline run showed worst cases like `tcp/kernel/64B max=1.97 ms` with `p999=69 us` (28× ratio) and `ex_md_udp/kernel/256B max=15.3 ms` with `p99=33 us` (440,000× ratio). These are HDR histogram raw maxima, not bench bugs — they capture rare OS jitter (scheduler preemption, IRQ flushes, page faults). Without context, anyone reading the histograms thinks the bench is broken.

**Fix** (commit 3): new "Interpreting the output" section in `README.md` explains the four legs (RTT/TX/RX/SRV), how they relate (`RTT ≈ TX + RX + SRV`), what max means vs percentiles, and which percentiles to trust for comparisons.

### M2: Reproducibility checklist missing from README

The bench controls CPU pinning + warmup but not CPU governor / NIC IRQ affinity / hugepage prefiguring / C-states / isolcpus. These are operator's job and we should say so.

**Fix** (commit 3): new "Reproducibility checklist" section with 7 concrete steps.

### M3: summary.md falsely claimed "no formal unit tests"

`tests/unit/bench/` actually has 4 test files (527 LOC) + the 2 new ones from this cleanup.

**Fix** (commit 3): replaced with accurate inventory + explicit list of UN-tested core/ headers (so the gap is visible).

### M4: post-condition checks died on transient races

`host_to_dpdk` and `dpdk_to_host` checked `ip link show $NIC_B` exactly once, no tolerance for the kernel taking a beat to materialise the netdev.

**Fix** (commit 2): introduced `wait_for_link()` helper polling for up to 2 s before declaring failure.

## LOW findings

- **L1**: `core/runner.hpp` had no unit test → fixed in commit 4.
- **L2**: `core/socket_bind.hpp::accept_one` poll/retry untested → not addressed (out of scope; would require mocking POSIX sockets).
- **L3**: Bash color codes always emitted even for piped output → not addressed (cosmetic, low value).

## Verification

### Build & test sweep

| Target | Tests | Result |
|---|---|---|
| `test_bench_load_bench_conf` | 7 | ✅ |
| `test_bench_stream_scheduler` | 3 | ✅ |
| `test_bench_tsc_protocol` | 10 | ✅ |
| `test_bench_ws_frame` | 8 | ✅ |
| `test_bench_runner` (new, commit 4) | 12 | ✅ (~19 s runtime) |
| `test_bench_no_dead_headers` (new, commit 5) | 1 | ✅ |
| `test_transport_tls_ws_e2e` (from earlier in session) | 5 | ✅ |
| Full `xmake build -g tests` | — | ✅ no new warnings |

**Lint test regression-validated**: temporarily added `core/dead_test_header.hpp`, ran `test_bench_no_dead_headers`, observed:
```
found 1 dead header(s) under benchmarks/latency/core/ ...
  - core/dead_test_header.hpp
```
Removed canary; test passes again.

### Smoke test on lat wrapper

| Scenario | Result |
|---|---|
| `lat tcp --dpdk` (vfio-pci → vfio-pci, no transition) | ✅ |
| `lat tcp --dpdk` after `rm .dpdk_state` (H1 fix) | ✅ — detected via `NIC_B_PCI` from bench.conf |
| `lat tcp` (vfio-pci → bench_ns full transition) | ✅ |
| `lat tcp` re-run (bench_ns,bench_ns idempotent no-op) | ✅ |
| `lat tcp --dpdk` after wedged state | ✅ — auto-recovered via `unwedge_nic()` |

### Perf parity (commit 1 didn't touch any `lat_*.cpp`, perf preserved by construction; spot-checked anyway)

Reran 3 representative scenarios post-cleanup and compared to baseline. See `bench-lat-postclean-compare-20260410.md`. Headline:

- **p50 max delta**: +3.7% (within ±5% threshold; most p50s within ±1%)
- **SRV leg max delta**: ±1.1% (effectively unchanged — strongest "underlying mock byte-identical" signal)
- **p99 / p999 ⚠ flags**: present but expected — HDR bucket noise at the tail of the distribution. Documented in commit 3 README updates.

**Verdict**: no regression introduced by the cleanup.

## Audit corrections lessons learned

The structural audit (first pass) had a fundamental methodology flaw: it grep'd `lat_*.cpp` for direct includes and called any unreferenced `core/*.hpp` "dead". It missed:

1. Transitive includes (e.g. `runner.hpp` includes `scenario_concept.hpp`)
2. Includes from `mock_*.hpp` (which are themselves included by `lat_*.cpp`)
3. Includes from `tests/unit/bench/*.cpp` (out-of-tree test files)
4. The actual function signatures inside `runner.hpp` (it didn't read past the public API headers)

**Lesson recorded**: future audits should walk the include graph from a comprehensive root set (scenario sources + mock headers + unit tests) and verify "0 callers" claims by full transitive traversal, not by leaf-file grep. The new `test_bench_no_dead_headers` lint test embodies this rule and will catch any future regression.

## Architecture changes (LOC delta by area)

```
benchmarks/latency/
  bench.conf                      +7    new optional NIC_B_PCI key
  lat                            +193 / -43   (H1+H2+H3+M4 robustness)
  README.md                      +94 / -2     (M1 + M2 + housekeeping)
  summary.md                     +24 / -13    (M3 + housekeeping)
  core/udp_client.hpp            -80           DELETED (truly dead)

tests/unit/bench/
  test_runner.cpp                +334          NEW (BenchRunner unit tests)
  test_no_dead_headers.cpp       +156          NEW (lint test)
  xmake.lua                      +14           2 new test targets

Total: +822 / -138 = net +684 LOC
                    (~14% growth, all in tests + docs;
                     production code is net -80 LOC)
```

## Behavior changes

| Change | Old behavior | New behavior | Reason |
|---|---|---|---|
| `lat tcp --dpdk` on fresh checkout where ens35 is on vfio-pci | Died with "ens35 not visible..." (H1) | Detects state via `NIC_B_PCI` or sysfs probe; runs cleanly | H1 fix |
| `lat tcp` re-run when already in bench_ns | Died with "Cannot find device" if state mis-detection occurred (H2) | Detects already-in-bench_ns; idempotent no-op | H2 fix |
| `lat tcp --dpdk` after kernel auto-rebinds ena post-DPDK exit (wedged state) | Died with "unknown state" (H3) | Detects "wedged"; auto-recovers via unbind/bind cycle; transitions normally | H3 fix |
| `host_to_dpdk` / `dpdk_to_host` post-condition fails on first check | Died immediately | Polls for up to 2 s before declaring failure | M4 fix |
| `xmake run -g tests` | Required manual review for dead `core/*.hpp` | Now auto-fails if any dead header exists | new lint |

## Follow-ups (out of scope but worth noting)

- **L2**: `core/socket_bind.hpp::accept_one` poll/retry loop is still untested. Would need POSIX socket mocking.
- The structural audit's hallucinated dead-code list should serve as a cautionary tale for future audit prompts — be specific about how to verify "0 callers" claims.
- Consider whether the kernel's auto-rebind race (~30 s after DPDK process exit) is worth filing upstream. It's a real ENA/kernel quirk that affects any DPDK user, not just this bench.

## Commit log (in execution order)

```
bcd6e71 refactor(bench/latency): rm dead udp_client.hpp helper
5b26eb4 fix(bench/latency): robust NIC-B state detection in lat wrapper
b5d6d1a docs(bench/latency): outlier semantics, reproducibility checklist, test inventory
f0a7c8d test(bench/latency): unit tests for BenchRunner sweep loops
57e90b2 test(bench/latency): lint test catching unreachable core/*.hpp headers
```

All 5 are local-only (not pushed). Ready for `git push` on user authorization.
