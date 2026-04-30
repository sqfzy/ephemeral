# TODO

Open follow-ups across the project. Items here are **not yet promised**
to anyone — they're a holding tank for "would be nice" / "should do later"
work. Promote items into `/pax`-driven reshapes when picked up.

Format: `- [ ] <action>` per line, with a hint of when / why under each
top-level bullet. Keep terse — long descriptions belong in the linked
plan / retro / issue.

---

## reshape/parallel-bench (v2) follow-ups (2026-04-30)

Source: `.artifacts/reshape-parallel-bench-final-v2-20260430.md` §Follow-ups.

- [ ] **Run 7-scenario `lat all --dpdk` end-to-end** to verify the
  ~7× speedup claim (current verification is 4-scenario × 18s wall
  time = 3.3×). Needs a host with ≥8 lcores free and 7 distinct
  CPU IDs. Update retro's "Final result" table once measured.

- [ ] **Add `[parallel]` user guide section to
  `benchmarks/latency/README.md`**. Today the only user-facing doc
  is the CHANGELOG entry + the commented template in
  `benchmarks/latency/config.toml`. A README section with worked
  examples (small/medium/large machine sizings, fan-out load test
  pattern of multiple rows pointing at the same scenario) would
  help adoption.

- [ ] **Auto-derive `runs[]` from `cpu.eal_cores` + a top-level
  `enabled = ["lat_tcp", ...]` list**. Today the user must
  hand-write each `(scenario, lcore, cpu, queue)` tuple. A helper
  could synthesize reasonable defaults given just a list of
  scenario names. Out of scope for v1 reshape.

- [ ] **Per-slot result aggregation tool**:
  `benchmarks/latency/scripts/show_parallel_run.py` collating the
  latest `_slot<i>` JSONs into one table (slot / scenario / p50 /
  p99 / samples / wall_time). Today `parallel_e2e.sh` greps each
  slot manually.

- [ ] **2-week regression check** on `parallel_e2e.sh` after main
  has accumulated 2 weeks of unrelated commits — make sure no new
  PR silently broke the parallel path. Could be a one-shot
  `/schedule` agent.

- [ ] **Move lat_ex_market kernel real-server flow into its own
  per-scenario header**. Currently the real-server (DNS-resolved
  wss://) path stays inline in `lat_ex_market.cpp` main(); only
  the DPDK + kernel-mock path is in `scenarios/lat_ex_market_loop.hpp`.
  Splitting would let lat_multi_dpdk also support real-server
  (questionable: real-server is single-stream by nature, parallel
  doesn't add value) — but if a future user wants symmetry it'd
  be a small refactor.

## reshape/rss-aware-connect follow-ups (2026-04-30)

Source: `.artifacts/reshape-rss-aware-connect-final-20260430.md` §Follow-ups.

- [ ] **`validate_config` strengthening**: reject
  `nb_rx_queues > 1 && nb_tx_queues == 1` early so the
  TX-queue-mismatch gotcha (Task 1 retro #1) can't sneak through
  silently. ~30-line follow-up reshape.

- [ ] **FlowDirector handshake race** (KNOWN LIMITATION at
  `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp:788-816`):
  install rule before connect() instead of after, or steer-to-
  rx_queue transient rule. Needs FD-capable NIC (Mellanox /
  Intel) to validate. Out of scope for ENA-only host.

## reshape/api-unify follow-ups (2026-04-30)

Source: `.artifacts/reshape-api-unify-final-20260430.md` §Follow-ups.

- [ ] **Extract `PlatformConfig` to its own header**. Resolves
  the circular include (`platform.hpp` ↔ `join_dynamic.hpp`)
  currently bridged by the `EPH_DPDK_PLATFORM_CONFIG_DEFINED`
  sentinel macro. 1-2 hour task; eliminates a fragile
  inclusion-order requirement.

- [ ] **Document `Platform::create_with_eal` more deeply in user
  docs** (currently has good doc-comment in platform.hpp but no
  prose in `eph-net-dpdk/docs/`).

## Open bugs / known limitations

- [ ] **ENA PMD MP secondary RX starvation under primary load**
  (bug #7 from reshape/parallel-bench v1 investigation, A/B
  confirmed): when primary process is actively rx_burst-ing on
  one queue, secondary rx_burst on a different queue is starved.
  Affects DPDK MP (`Platform::join_dynamic` / `create_secondary`)
  TCP/UDP **data plane** under sustained load on AWS ENA.
  Workaround: use single-process N-lcore design (which is what
  `lat_multi_dpdk` does — bypasses the PMD limitation entirely).
  Not in eph code; tracking here so future MP-on-ENA users find
  the documented limitation.
