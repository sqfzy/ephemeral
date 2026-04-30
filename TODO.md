# TODO

Open follow-ups across the project. Items here are **not yet promised**
to anyone — promote into `/pax`-driven reshapes when picked up.

Last verified: 2026-04-30 (none completed at this revision).

---

## P1 — quick wins (≤ 1 day each)

- [ ] **`validate_config` reject `nb_rx_queues > 1 && nb_tx_queues == 1`**
  early. Today only `nb_tx_queues == 0` is rejected — `=1` with
  `nb_rx_queues > 1` silently lets a misconfigured user reach
  Phase-1-style "secondary's RSS-aware tx_queue_id ≥ nb_tx_queues"
  TX starvation. ~30 lines in
  `eph-net-dpdk/include/eph/dpdk/platform.hpp:474-498`.
  Source: `.artifacts/reshape-rss-aware-connect-final-20260430.md`.

- [ ] **Per-slot result aggregation script**
  `benchmarks/latency/scripts/show_parallel_run.py`. Collate the
  latest `outputs/lat_*_dpdk_*_slot<i>_*.json` into one table
  (slot / scenario / p50 / p99 / samples / wall). Today
  `parallel_e2e.sh` greps each slot manually.
  Source: parallel-bench v2 retro.

- [ ] **`[parallel]` user guide section in
  `benchmarks/latency/README.md`**. Today the only user-facing doc
  is the CHANGELOG entry + commented template in `config.toml`.
  Worked examples (small/medium/large machine sizings, fan-out
  load test pattern of multiple rows pointing at the same scenario).
  Source: parallel-bench v2 retro.

- [ ] **Document `Platform::create_with_eal` in user docs**.
  Has good doc-comment in `platform.hpp` but no prose in
  `eph-net-dpdk/docs/`. New page `docs/platform-bringup.md`?
  Source: api-unify retro.

## P2 — medium reshapes (1-3 days)

- [ ] **Run 7-scenario `lat all --dpdk` end-to-end** to verify the
  ~7× speedup claim (today verified: 4-scenario × 18s wall =
  3.3×). Needs ≥8 free lcores + 7 distinct CPU IDs. Update
  retro's "Final result" table once measured.
  Source: parallel-bench v2 retro.

- [ ] **Auto-derive `runs[]` from `cpu.eal_cores` + top-level
  `enabled = ["lat_tcp", ...]` list**. User today must hand-write
  each `(scenario, lcore, cpu, queue)` tuple. Synthesize defaults
  given just a scenario name list.
  Source: parallel-bench v2 retro.

- [ ] **Extract `PlatformConfig` to its own header**. Resolves the
  circular include between `platform.hpp` ↔ `join_dynamic.hpp`
  currently bridged by `EPH_DPDK_PLATFORM_CONFIG_DEFINED` sentinel
  macro. 1-2 hour task; eliminates fragile inclusion-order
  requirement.
  Source: api-unify retro.

- [ ] **Move `lat_ex_market` kernel real-server flow to its own
  header**. Currently the DNS-resolved wss:// path stays inline
  in `lat_ex_market.cpp` main(); only DPDK + kernel-mock is in
  `scenarios/lat_ex_market_loop.hpp`. Splitting would let
  `lat_multi_dpdk` symmetrically support real-server (debatable
  value: real-server is single-stream, parallel doesn't add much).
  Source: parallel-bench v2 retro.

## P3 — conditional / hardware-dependent

- [ ] **FlowDirector handshake race fix** at
  `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:788-816`
  KNOWN LIMITATION block. Either install `rte_flow` rule before
  connect() instead of after, or use a transient steer-to-rx_queue
  rule. **Needs FD-capable NIC** (Mellanox / Intel) to validate;
  AWS aarch64 ENA host can't.
  Source: rss-aware-connect retro.

---

## Known limitations (documented, no fix planned)

- **ENA PMD MP secondary RX starvation under primary load**
  (parallel-bench v1 bug #7, A/B confirmed): when DPDK MP primary
  is actively `rx_burst`-ing on one queue, secondary `rx_burst` on a
  different queue is starved on AWS ENA. Affects autojoin /
  `create_secondary` data plane under sustained load.
  **Workaround already shipped**: `lat_multi_dpdk` uses
  single-process N-lcore design — bypasses the PMD limitation
  entirely. This note exists so future MP-on-ENA users find
  the documented diagnosis instead of re-investigating.

---

## Schedule candidates (use `/schedule`, not TODO)

These are time-anchored, one-shot checks better suited to a scheduled
agent than a TODO entry:

- **2-week regression check on `parallel_e2e.sh`** after main
  accumulates 2 weeks of unrelated commits. If `lat all --dpdk`
  4-scenario PASS + wall ≤ 25s, do nothing; else open issue with
  log tail.
