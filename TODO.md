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

_None at this time._

---

## Resolved (archive)

- [x] **ENA PMD MP secondary `rx_burst` crashes — root cause was
  eph DPDK MP teardown protocol violation, not an ENA limitation**
  (resolved 2026-05-01; fix commits `0b3a4aaa` / `b4074d62` /
  `ef1bec67` / `3b66ee35` / `067dccbc`; root cause `aa625b4d`)

  Originally framed as "ENA PMD MP secondary `rx_burst` is
  fundamentally broken" — that framing was wrong. ENA's behaviour
  followed the DPDK contract correctly: when primary called
  `rte_eth_dev_stop`, ENA tore down all queues including those owned
  by secondaries (writing NULL into shared hugepage `io_cq` state),
  which is what the spec allows. The bug was in eph's `~Platform()`
  unconditionally calling stop / close / `eal_cleanup` on primary
  exit, regardless of whether secondaries were still attached.

  **Fix**: gate primary teardown on `MpRegistry::is_last_alive_proc()`.
  Implementation lives in `Platform::Impl::defer_for_peers()` and is
  consulted at four sites: `Impl::cleanup()`, `~Impl()` body (for
  `IcmpDirectoryHandle::disable_memzone_free`), `~Platform()` (for
  `rte_eal_cleanup`), and `MpRegistryHandle::release_()` (for its
  own memzone free).

  **Side finding (also resolved by `3b66ee35`)**: the seven `lat_*.cpp`
  binaries previously hardcoded `register_poller(0, ...)` regardless
  of which queue the process owned; they now read
  `env.platform.effective_rx_queue_range().first`. Validated by
  `/tmp/ena_mp_7proc_parallel.sh` (7 lat_*_dpdk binaries running as
  7 MP processes on ENA, 7/7 PASS, ~1 M total samples).

  **Reference**:
  * Protocol guide: `eph-net-dpdk/docs/dpdk-mp-teardown-protocol.md`
  * Methodology retro: `.artifacts/retro-20260501-ena-mp-rootcause-discovery.md`
  * Idle-ring sentinel (still maintained):
    `eph-net-dpdk/tests/integration/repro_ena_mp_secondary_rxburst.cpp`
    — should still exit 9; if it ever exits 0, the gate has regressed.
  * Acceptance harnesses: `/tmp/ena_mp_rootcause.sh`,
    `/tmp/ena_mp_rootcause_primary_early.sh`, `/tmp/ena_mp_7proc_parallel.sh`.

  **Trade-off (v1)**: the gate is refcount-only — abnormal peer exit
  (`kill -9`, OOM) leaves a stale claimed slot and primary then
  defers teardown indefinitely. `scripts/dpdk-teardown.sh` recovers
  between sessions. v2 candidate (IPC heartbeat reaper) parked.

---

## Schedule candidates (use `/schedule`, not TODO)

These are time-anchored, one-shot checks better suited to a scheduled
agent than a TODO entry:

- **2-week regression check on `parallel_e2e.sh`** after main
  accumulates 2 weeks of unrelated commits. If `lat all --dpdk`
  4-scenario PASS + wall ≤ 25s, do nothing; else open issue with
  log tail.
