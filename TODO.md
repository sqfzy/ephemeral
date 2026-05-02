# TODO

Open follow-ups across the project. Items here are **not yet promised**
to anyone — promote into `/pax`-driven reshapes when picked up.

Last verified: 2026-04-30 (none completed at this revision).

---

## P1 — quick wins (≤ 1 day each)

- [x] ~~**`validate_config` reject `nb_rx_queues > 1 && nb_tx_queues == 1`**
  early.~~ Done in batch 23 (commit on `reshape/parallel-bench`):
  rejection added to `validate_config` with explanatory comment +
  three new test cases in `test_dpdk_platform_mempool.cpp`
  (`ValidatorRejectsRssWithSingleTxQueue`,
  `ValidatorAcceptsMatchedMultiQueue`, `ValidatorAcceptsSingleQueueBoth`).

- [ ] **Document `Platform::create_with_eal` in user docs**.
  Has good doc-comment in `platform.hpp` but no prose in
  `eph-net-dpdk/docs/`. New page `docs/platform-bringup.md`?
  Source: api-unify retro.

## P2 — medium reshapes (1-3 days)

- [ ] **Extract `PlatformConfig` to its own header**. Resolves the
  circular include between `platform.hpp` ↔ `create_or_join.hpp`
  currently bridged by `EPH_DPDK_PLATFORM_CONFIG_DEFINED` sentinel
  macro. 1-2 hour task; eliminates fragile inclusion-order
  requirement.
  Source: api-unify retro.

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

_None scheduled._
