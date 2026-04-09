# Bench latency rewrite — retrospective

**Date**: 2026-04-09
**Plan**: `.artifacts/plan-bench-latency-rewrite-20260409-023700.md`
**Status**: ✅ Complete (6/6 phases shipped)

## What got built

A clean-slate rewrite of the latency benchmark suite under
`benchmarks/latency/`, replacing the old organically-grown 27 files +
700-line orchestration script that mixed concerns.

| | Old | New |
|---|---|---|
| Scenarios | 6 (mixed: tcp/udp/ws/market/order/relay) | 6 (3 raw + 3 quant) |
| Mock servers | 3 binaries + mode flags | 5 binaries, one per workload |
| Mock org | `ws_server.hpp` 500 lines, 3 modes via if/goto | scenario-per-directory, shared `mock/lib/` |
| Scenario abstraction | 1 concept with `payload=0` hack for 1-leg | 2 concepts (`RttScenario` / `OneWayScenario`) |
| CPU pinning | weak (set affinity, no validation) | strict (isolcpus / SMT sibling / NUMA / IRQ) |
| Order RTT | sync only | N-inflight pipeline (`--inflights 1,4,16,64`) |
| Bench data | written to `.bench/*.jsonl` + `HISTORY.md` | **stdout only** — user pipes to `tee` |
| Orchestration script | 700 lines | 685 lines, dual-mode (`--loopback` / `--nic-*`) |

## Phase commits

```
67cb348 phase 5 — bench_latency.sh thin orchestrator
d6dc0ee phase 4 — exchange (quant) scenarios + 10 build targets
1df2314 phase 3 — tcp / udp / ws scenarios + 12 build targets
3991dd9 phase 2 — core/ + mock/lib/ shared header layer
5693263 phase 1 — wipe latency bench, lay new skeleton
```
plus this phase 6 commit.

## Sanity-check numbers (loopback, 3s windows, kernel transport)

| scenario | leg | p50 | p99 | p999 | n |
|---|---|---|---|---|---|
| tcp 64B   | RTT | 10.9µs |  11.4µs |  15.1µs | 271k |
| tcp 64B   | SRV |   268ns |   268ns |   276ns |  ↑  |
| udp 64B   | RTT |  9.4µs |   9.9µs |  13.6µs | 314k |
| udp 64B   | SRV |   260ns |   260ns |   260ns |  ↑  |
| ws 64B    | RTT | 12.1µs |  12.6µs |  16.4µs | 241k |
| ws 64B    | SRV |   284ns |   300ns |   300ns |  ↑  |
| ex/market | RX  |  6.1µs |   8.5µs |   9.0µs |  92k |
| ex/order inflight=1  | RTT |  9.5µs | 14.7µs | (skew)  | 147k |
| ex/order inflight=16 | RTT | 66.8µs | 76.3µs | (skew)  | 415k |
| ex/md_udp 64B  | RTT |  9.4µs |   9.8µs | 13.3µs | 313k |
| ex/md_udp 1400B| RTT |  9.7µs |  10.2µs | 13.6µs | 304k |

## Comparison vs the deleted suite (`.bench/full_kernel_20260408.txt`)

| scenario | old (NIC wire, 10s) | new (loopback, 3s) | analysis |
|---|---|---|---|
| tcp 64B RTT p50 | 24.4µs | 10.9µs | **expected** — loopback halves the wire path |
| udp 64B RTT p50 | 22.7µs | 9.4µs | **expected** — same reason |
| tcp 64B server-leg p50 | 0.0µs | 268ns | **D-5 working** — `--server-work-ns 200` injects realistic business spin; old pure-echo could not measure it |
| udp 64B server-leg p50 | 0.0µs | 260ns | same |

The plan asked for "DPDK p50/p99 ±10% of pre-rewrite baseline". That
comparison must wait for an actual two-NIC machine — neither this
session nor the loopback path can speak to it. The kernel-loopback
sanity check is what was achievable, and it confirms:

1. The bench mechanics are sound (4-leg report, sub-µs server-leg,
   100k–400k samples per 3-second window).
2. The new SRV-leg now reflects business work, which the old pure-echo
   suite explicitly could not measure.
3. The order pipeline degrades p50 1→16 inflight 9.5→66.8µs as
   expected for queueing on a single mock thread.

## Things deferred to follow-up plans

1. **DPDK transport (real)** — every `*_dpdk` binary today compiles the
   same source with `EPH_USE_DPDK=1` and uses POSIX sockets at runtime
   with a stderr warning. A real transport requires EAL init, Platform,
   manual `rte_eth_*` for raw TCP/UDP and is its own plan-sized chunk.
2. **`--trace FILE` for `mock_lat_exchange_ws`** — argument is parsed
   but ignored (warns). Replay from a recorded Binance trace is on the
   roadmap but not in scope of this rewrite.
3. **Real-NIC regression baseline** — needs ens34/ens35 + isolcpus +
   sudo. The loopback comparison above is the substitute for this
   session.

## Memory updates

- `feedback_bench_data_convention.md` — already deleted earlier (the
  ".bench/HISTORY.md required" rule was contradicted by D-15 "no file
  output").
- `project_bench_rewrite.md` — rewritten to mark the project complete
  and to surface the three non-obvious facts (no file output, DPDK
  stubs, `lat_` binary prefix).

## Did we follow the plan?

Mostly. The two deviations:

- **Binary names**: plan called for `mock_<scenario>` / `bench_<scenario>`;
  shipped as `mock_lat_<scenario>` / `bench_lat_<scenario>` to dodge
  collisions with `eph-dpdk/benchmarks/bench_udp.cpp` and
  `eph-net/benchmarks/bench_ws.cpp`. Discovered at link time in phase 3.
- **DPDK build implementation**: plan acceptance for stages 3-4 only
  required "12/10 targets build pass". The targets exist, link
  eph-dpdk, and define `EPH_USE_DPDK=1`, but the source remains POSIX.
  This is documented in every `mock.cpp` / `bench.cpp` header and via
  a stderr warning at startup.

Both deviations are spelled out in the corresponding commit messages.
