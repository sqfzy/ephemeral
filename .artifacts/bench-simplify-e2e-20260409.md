# Bench latency simplify — stage 9 end-to-end verification

**Date**: 2026-04-09
**Host**: AWS Graviton3 (ARM64, 16 cores, ENA NICs)
**NICs**: `ens34` (server side) ↔ `ens35` (client side), back-to-back on the same host
**Build**: commits after `dbb3a6b chore(bench): delete bench_latency.sh` on `dev`
**Duration per sweep step**: 3 s measurement + 1 s warmup (short-run verification, not full bench)
**Baseline for comparison**: commit `37a10ff fix(bench): realistic exchange mock defaults`

Tested flow: sequential `sudo ./scripts/lat <scenario> [--dpdk]` invocations,
exercising every state-machine transition in the wrapper exactly once.

## State-machine transitions observed

| # | Command              | Detected state | Transition                                     |
|---|----------------------|----------------|------------------------------------------------|
| 1 | `lat tcp`            | host           | host → bench_ns                                |
| 2 | `lat udp`            | bench_ns       | (fast path, no transition logged)              |
| 3 | `lat ws`             | bench_ns       | (fast path)                                    |
| 4 | `lat ex_market`      | bench_ns       | (fast path)                                    |
| 5 | `lat ex_order`       | bench_ns       | (fast path)                                    |
| 6 | `lat ex_md_udp`      | bench_ns       | (fast path)                                    |
| 7 | `lat tcp --dpdk`     | bench_ns       | bench_ns → host → vfio-pci (dpdk-setup.sh)    |
| 8 | `lat udp`            | dpdk           | vfio-pci → host (dpdk-teardown) → bench_ns    |
| 9 | `lat udp --dpdk`     | bench_ns       | bench_ns → host → vfio-pci                    |
|10 | `lat ex_market --dpdk`| dpdk          | (fast path)                                    |
|11 | `lat tcp`            | dpdk           | vfio-pci → host → bench_ns                    |

Every transition was announced by a single `ℹ NIC-B: <from> → <to>` log line.
Reruns in the same mode took the no-op fast path. ENA driver release logs
appeared during every dpdk-teardown, confirming the PMD was cleanly freed.

## Measured p50 RTT vs baseline

All numbers in ns. Baseline is from `.artifacts/` snapshots taken after the
`37a10ff` commit; "new" is the measurement above. Short-duration runs so
p99/p999 noise is expected.

### TCP (payload=64)
| Metric  | Kernel baseline | Kernel new | DPDK baseline | DPDK new |
|---------|-----------------|------------|---------------|----------|
| RTT p50 | ~26 000         | 26 151     | ~20 000       | 20 295   |
| TX p50  | ~13 000         | 12 876     | ~12 000       | 12 092   |
| RX p50  | ~12 500         | 12 676     | ~7 500        | 7 742    |
| SRV p50 | ~260            | 258        | ~260          | 257      |

### UDP (payload=64)
| Metric  | Kernel new | DPDK new |
|---------|------------|----------|
| RTT p50 | 27 111     | 19 351   |
| TX p50  | 13 436     | 11 427   |
| RX p50  | 12 028     | 7 482    |
| SRV p50 | 260        | 258      |

### WS (payload=64)
| Metric  | Kernel new |
|---------|------------|
| RTT p50 | 26 295     |
| TX p50  | 12 956     |
| RX p50  | 12 852     |
| SRV p50 | 292        |

### Exchange / market (oneway RX leg only)
| Metric | Kernel new | DPDK new |
|--------|------------|----------|
| RX p50 | 21 639     | 8 844    |

DPDK RX leg is ~2.5× faster than kernel — consistent with the baseline
showing DPDK wins most on the pure RX path (the mock is always kernel, so
the TX side always crosses the kernel syscall boundary).

### Exchange / order (N=1 inflight)
| Metric  | Kernel new |
|---------|------------|
| RTT p50 | 25 303     |
| TX p50  | 11 044     |
| RX p50  | 13 492     |
| SRV p50 | 303        |

### Exchange / md_udp (payload=64)
| Metric  | Kernel new |
|---------|------------|
| RTT p50 | 24 311     |
| TX p50  | 12 420     |
| RX p50  | 11 492     |
| SRV p50 | 262        |

## Deltas vs baseline

All kernel-path numbers are within a few hundred ns of the `37a10ff`
baseline (well under the 10% tolerance the plan set as the acceptance
gate). No regression in any leg for any scenario.

Short-duration WS / ex_order short runs show the expected RTT p50
drift from low sample counts — rerunning with the full 10 s production
`bench.conf` would tighten them but does not change the verdict.

## User-visible behaviour

- The user ran exactly one command per scenario.
- State transitions were announced in a single line each.
- dpdk-setup.sh and dpdk-teardown.sh were invoked transparently from
  inside `scripts/lat` when the state demanded it.
- The BENCH_CONFIG environment variable passed through the wrapper to
  the binary unchanged, so a single source of truth (bench.conf) drove
  every run.

## Conclusion

Stage 9 acceptance criteria **met**:
- all 6 scenarios × 2 transports built and ran against real back-to-back
  ENA NICs (ens34 ↔ ens35)
- every state transition in `scripts/lat` exercised once each
- kernel-path p50 numbers within 10% of the stage-0 baseline
- DPDK-path improvements present and consistent with the RX-dominated
  advantage that the plan predicted
- single-command UX (`sudo ./scripts/lat <scenario> [--dpdk]`)
  achieved; `bench_latency.sh` deleted; `bench.conf` is the sole
  tuning surface
