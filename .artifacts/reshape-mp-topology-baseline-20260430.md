# Baseline — reshape/mp-topology

- Date: 2026-04-30 03:11 UTC
- Branch: `reshape/mp-topology` (off `main`)
- HEAD: `7a00b1e5` — fix(integration/dpdk_e2e wrapper): migrate from bench.conf to config.toml
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`
- DPDK NIC operations: **DEFERRED — user has another DPDK program holding hugepages/vfio**

## Build state

| Target | Result |
|--------|--------|
| `xmake build test_dpdk_multiprocess_config` | OK (already cached) |
| `xmake build -g tests` | OK in 1.5s (incremental) |

## Test baseline (must remain green after every stage)

`xmake run test_dpdk_multiprocess_config` — 22 tests / 5 suites, all pass:

- `BuildEalArgv` (5)
- `RrCounterRange` (6) — `FullRangeFallbackEquivalentToModNbQ`, `PartitionedRangeStaysWithinBounds`, `SingleQueueRangeReturnsConstant`, `DistinctRangesDoNotCollide`, `WrapAroundFromMaxU16PreservesBounds`, `WrapAroundWithNonPowerOfTwoRange`
- `PlatformEffectiveGetters` (1)
- (+ 2 more suites — 10 cases for `CreateSecondaryValidation` + `ValidateConfigRxQueueRange`)

## DEFERRED until user re-enables NIC

| Item | Reason | Notes |
|------|--------|-------|
| `lat_tcp_dpdk` / `lat_udp_dpdk` / `lat_ws_dpdk` | needs vfio-pci NIC + hugepages | bench p50/p99 baseline & post-reshape compare |
| `dpdk_mp_e2e.sh` | needs vfio-pci NIC + hugepages | invariant — must still pass after stages 4-5 |
| `test_mp_registry` actual run | needs EAL (would conflict with user's DPDK process via file_prefix / hugepage) | only build will be verified during stages 2-7 |
| `dpdk_mp_topology_e2e.sh` | new e2e, same constraint | new path verification — gates stage 4-5 |
| `examples/simple_hft_dpdk_mp` actual run | same | only compile verified during stage 6 |

## Invariant snapshot (must remain byte-identical)

- `PlatformConfig` field set & defaults (platform.hpp:170-409)
- `validate_config` rules (platform.hpp:417-447)
- `Platform::create / create_primary / create_secondary` factory contract (platform.hpp:536-571)
- `find_src_port_for_queue` signature (flow_steering.hpp:950-973)
- `rr_counter` algorithm (`lo + fetch_add(1) % (hi-lo)` — tcp_stream.hpp:811-827, udp_socket.hpp:273-288)
- ICMP registry whole module (detail/icmp_registry.hpp)
