# Changelog

All notable changes to the latency benchmark suite are recorded here.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Added
- `core/pin_client.hpp::pin_client_from_cfg` now calls
  `eph::utils::lock_memory` after pinning, immunizing the bench
  client's address space against page-fault tail spikes during the
  measurement window. Mockex (`benchmarks/mockex/src/main.cpp`) does
  the same. The pair cuts lat_ws DPDK RTT max from ~7M ns
  (1-11M ns cross-run) to ~3M ns (2.4-4.0M ns cross-run, ~4× tighter
  TX stddev) with no change to p50 / p99. Both calls are best-effort
  (WARN + continue on failure). `ulimit -l unlimited` is the
  recommended host setting; sudo is already a hard requirement for
  DPDK runs so the typical bench operator already has the privileges.
### Changed
- Subproject now owns its own `xmake.lua` and `scripts/` (the `lat`
  runner lives next to its binaries, not at the repo root).
- `scripts/lat` fails hard when `dpdk-setup.sh` or `dpdk-teardown.sh`
  reports failure, instead of silently continuing into a broken state.

### Removed
- Stale `.bench/` artifact directories — the bench writes no files, so
  any residue was historical noise.

### Fixed
- `scripts/lat` `cleanup()` deletes the mockex log on clean exit and
  preserves + prints the path on failure / mid-run mockex death /
  SIGINT/SIGTERM (commit `afcfd223`). Stops `/tmp/lat_mockex_*.log`
  from accumulating on a long-running dev box.
- `scripts/lat` `conf_get()` / `conf_get_section()` now split on the
  FIRST `=` only via `index()`+`substr()` (commit `12d239a3`). The
  previous `split($0, a, "=")` silently truncated values containing
  `=` — e.g. `endpoint = "wss://host/stream?streams=btc&depth=20"`
  was returned as `wss://host/stream?streams`, causing opaque WS
  connect failures with no diagnostic pointing at the parser.

## [Simplify Plan] — 2026-04

End state of the 8-stage "simplify" plan that replaced the old
`framework/` machinery with one self-contained `lat_<scenario>.cpp` per
scenario.

### Added
- `BenchConfig` + `load_bench_conf()` — single-source tuning read from
  `bench.conf`, replaces dozens of CLI flags.
- `scripts/lat` — single-command runner. Detects current NIC-B state
  (host / `bench_ns` / `vfio-pci`), drives it idempotently toward the
  desired state, and execs the right `lat_<scenario>[_dpdk]` binary.
- Six latency scenarios:
  - `lat_tcp` — raw TCP echo RTT sweep
  - `lat_udp` — raw UDP echo RTT sweep
  - `lat_ws`  — plain WebSocket echo RTT sweep
  - `lat_ex_market` — exchange bookTicker push (1-leg, TSC stamp read from server `T`)
  - `lat_ex_order`  — exchange order RTT, N-inflight pipeline
  - `lat_ex_md_udp` — exchange UDP market-data echo RTT
- Real DPDK transport for every client via `DpdkBenchEnv::create_full`
  (EAL guard → Platform → MAC lookup → ARP gateway resolve), producing a
  fully functional `lat_<scenario>_dpdk` variant from the same source.

### Changed
- Exchange WS mock now covers all 4 stream classes (bookTicker, depth,
  trade, kline) with realistic Binance-like rates by default, and reuses
  the unified `StreamScheduler` for multi-symbol dispatch.
- Latency samples flow through a single `BenchRunner` that drives
  warmup → measurement → print, feeding TSC deltas into
  `eph::utils::Recorder` instances (HdrHistogram + TSC→ns conversion).
- Mock is always kernel — only the client-side transport switches to
  DPDK. This keeps kernel-vs-DPDK comparisons fair because the dominant
  server-side TSC stamp is identical on both sides.

### Removed
- `bench_latency.sh` — the multi-scenario orchestrator that predated
  the single-command `scripts/lat` runner.
- `METRICS.md` and top-level `bench_matrix.hpp` — every subproject now
  owns its own matrix header.
- Per-scenario CLI flags — tuning lives in `bench.conf`, not argv.
- `LegStats`, bundled base64 duplicates, and generic helpers that
  escaped to `eph-utils`.

## [Earlier]

Older history is preserved in `git log`. Notable checkpoints:

- **phase 4 exchange scenarios** — added 10 exchange build targets
  covering the original 3-leg market/order/md split.
- **phase 3 raw scenarios** — added raw TCP / UDP / WS RTT benches with
  12 build targets.
- **phase 2 core shared layer** — consolidated common mock / lib helpers
  into `core/`.
- **phase 1 rewrite** — initial skeleton for the current bench shape.
