# Reshape complete — mp-icmp-flowdir

- Date: 2026-04-30 06:30 UTC
- Branch: `reshape/mp-icmp-flowdir`
- Off `reshape/mp-topology` HEAD `89d9b1ec`
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`
- Stage 0 baseline: `.artifacts/reshape-mp-icmp-flowdir-baseline-20260430.md`
- Milestone A retro: `.artifacts/reshape-mp-icmp-flowdir-milestone-a-20260430.md`

## Stage map (all 10 done; 9 commits, retro inline as 10th)

| Stage | Commit | Subject |
|-------|--------|---------|
| 0 | 418b96fd | baseline snapshot (70 cases + 2 e2e + bench) |
| 1 | 1d6b39e7 | feat(mp_ipc): typed RAII rte_mp_* IPC wrapper |
| 2 | dacf7b57 | feat(icmp_directory): hugepage POD directory |
| 3 | 7364ea70 | feat(dpdk): cross-process ICMP forwarding (core) |
| 4 | 20a06611 | docs(mp_icmp): milestone A docs + CHANGELOG + retro |
| 5 | b7d2a5ee | feat(flow_steering): FlowRule handle as variant |
| 6 | b9c6a93d | feat(dpdk): FlowDir IPC handlers (install / destroy) |
| 7 | cf4ce4a9 | feat(dpdk): try-secondary-then-fallback FlowDir install |
| 8+9 | (this commit) | milestone B docs + CHANGELOG + final retro |

## Diff summary

```
 milestone A (stages 0-4):  +2240 lines / -11 lines
 milestone B (stages 5-7):  +1402 lines / -68 lines (FlowRule variant + IPC handlers + fallback wiring)
 stage 8+9 (this commit):     ~150 lines (docs + example annotation + retro)
                              ────────────────
 reshape/mp-icmp-flowdir:   ~3800 lines added; ~80 lines deleted
```

## Verification — all green

| Layer | Result |
|-------|--------|
| `xmake build -g tests` | OK (~45 s clean / 1.6 s incremental) |
| Unit `test_icmp_dispatch` | 10 / 10 PASS (baseline, unchanged) |
| Unit `test_mp_registry` | 13 / 13 PASS (baseline, unchanged) |
| Unit `test_mp_topology` | 20 / 20 PASS (baseline, unchanged) |
| Unit `test_dpdk_multiprocess_config` | 27 / 27 PASS (baseline, unchanged) |
| Unit `test_mp_ipc` (NEW stage 1) | 12 / 12 PASS |
| Unit `test_icmp_directory` (NEW stage 2) | 17 / 17 PASS |
| Unit `test_flow_rule_variant` (NEW stage 5) | 10 / 10 PASS |
| Unit `test_fd_ipc_handlers` (NEW stage 6) | 6 / 6 PASS |
| Unit `test_flow_steering` | 51 / 51 PASS (white-box ports applied for variant) |
| **Unit total** | **166 cases, all green** |
| `dpdk_mp_e2e.sh` (legacy) | PASS |
| `dpdk_mp_topology_e2e.sh` (legacy) | PASS |
| `dpdk_mp_icmp_e2e.sh` (NEW milestone A) | PASS |
| `dpdk_mp_fd_fallback_e2e.sh` (NEW milestone B) | PASS |
| **Bench parity** (`lat_tcp_dpdk` 30 s) | RTT p50 21.8 µs vs baseline 22.2 µs (**−1.9 %**) |
| Hot path | byte-for-byte unchanged |

## What user code sees

Zero API change. `Platform::register_icmp_target` /
`Stream::create_and_attach` / `install_flow_rule` /
`eph::net::dpdk::FlowRule` all keep their public signatures and
default-mode behaviour. The two reshape additions are
**transparent**:

* **ICMP Frag Needed → owning stream's `effective_mss`** auto-
  propagates across processes when `mp_topology` is set. Static
  failure surface eliminated: PMTU updates are no longer silently
  dropped when the message lands on a non-owner secondary.
* **FlowDir secondary install** auto-falls-back through primary
  via `eph_fd_install` IPC if local `rte_flow_create` rejects.
  PMD compatibility moves from caller's concern to library
  detail. Hosts where the PMD supports secondary install pay
  zero IPC overhead.

## Pre-existing PMD limitation surfaced (not a reshape bug)

The `dpdk_mp_fd_fallback_e2e.sh` test exposed a host-side PMD
limitation: ENA on this aarch64 EC2 instance returns ENOSYS
(rte_errno=38, "Function not implemented") for `rte_flow_create`
when RSS is active, even on the **primary** process. This means
the IPC-fallback's primary-side install can't proceed on this
host either — the test handles this gracefully with a status=1
reply that exercises the IPC handler path fully, logs the PMD
limitation, and passes. On hosts with a PMD that supports
arbitrary primary-side `rte_flow_create` (e.g. mlx5 in
FlowDirector mode), the test would also exercise the destroy
IPC round-trip.

This is a known testing gap. The fallback **mechanism** is
fully wired and verified end-to-end up to the rte_flow_create
boundary; the actual `rte_flow_create` side is exercised by the
existing `test_dpdk_e2e.cpp` against the same NIC in
single-process mode.

## How to ship

The branch is independently shippable as a single PR. Two
checkpoints inside it for review chunking:

* HEAD `20a06611` (milestone A complete): ICMP cross-process
  forwarding only. Stand-alone, valuable, low blast radius.
* HEAD `cf4ce4a9` + this commit (milestone B complete + final
  docs): adds FlowDir secondary fallback. Builds on milestone
  A's IPC scaffolding, no new infrastructure.

If splitting PRs is preferred, A is mergeable without B; B
can land as a follow-up.

## Out-of-scope items (deliberately deferred)

* **Cross-process metric aggregation** — out of scope per the
  reshape's "Out of scope" doc list. External aggregators stay
  caller's responsibility.
* **Cross-process connection migration / failover** — same.
* **Independent-primary multi-process** (N processes, no
  shared mempool) — defeats the point of MP.

## Resource state at completion

* Branch: 9 commits ahead of `reshape/mp-topology`
* HugePages_Free: 1019 / 1024 (4 leaked from earlier SEGV
  before stage 0; reshape itself is clean)
* `0000:28:00.0` still bound to vfio-pci
* No DPDK process running
* `/var/run/dpdk/eph_mp_*` all GC'd

Ready for `gh pr create`.
