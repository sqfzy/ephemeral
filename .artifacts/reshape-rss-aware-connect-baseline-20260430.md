# Baseline — reshape/rss-aware-connect

- Date: 2026-04-30 11:21 UTC
- Branch: `reshape/rss-aware-connect` from `c267b9d6`
  (api-unify stage 8 final retro)
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`

## Unit suites — 11 binaries / 186 cases

| Suite | Cases | Result |
|-------|-------|--------|
| test_icmp_dispatch              | 10  | PASS |
| test_mp_registry                | 19  | PASS |
| test_mp_topology                | 20  | PASS |
| test_dpdk_multiprocess_config   | 27  | PASS |
| test_mp_ipc                     | 12  | PASS |
| test_icmp_directory             | 17  | PASS |
| test_flow_rule_variant          | 10  | PASS |
| test_fd_ipc_handlers            | 6   | PASS |
| test_flow_steering              | 51  | PASS |
| test_bdf_sanitize               | 11  | PASS |
| test_platform_create_with_eal   | 3   | PASS |
| **Total**                       | **186** | |

## E2E — 5 scripts (real NIC vfio-pci)

| Script | Result |
|--------|--------|
| dpdk_mp_e2e.sh           | PASS |
| dpdk_mp_topology_e2e.sh  | PASS |
| dpdk_mp_icmp_e2e.sh      | PASS |
| dpdk_mp_fd_fallback_e2e.sh | PASS |
| dpdk_mp_dynamic_e2e.sh   | PASS |

## Bench (30s, payload=256, NIC_B vfio-pci)

### lat_tcp_dpdk

| Metric | ns      |
|--------|---------|
| avg    | 22,350  |
| min    | 19,807  |
| p50    | 22,007  |
| p90    | 23,831  |
| p99    | 27,127  |
| p99.9  | 32,295  |
| max    | 541,502 |
| samples | 1,335,822 |
| throughput | 44,562 samples/s |

JSON: `benchmarks/latency/outputs/lat_tcp_dpdk_tls_rtt_2026-04-30_11-20-53.json`

### lat_udp_dpdk

| Metric | ns      |
|--------|---------|
| avg    | 19,916  |
| min    | 17,471  |
| p50    | 19,495  |
| p90    | 21,511  |
| p99    | 25,831  |
| p99.9  | 34,319  |
| max    | 182,547 |
| samples | 1,498,468 |
| throughput | 49,988 samples/s |

JSON: `benchmarks/latency/outputs/lat_udp_dpdk_rtt_2026-04-30_11-21-31.json`

## Parity gate (for Phase 1 verification)

| Metric | lat_tcp budget (≤5%) | lat_udp budget (≤5%) |
|--------|----------------------|----------------------|
| p50    | ≤ 23,108 ns          | ≤ 20,470 ns          |
| p99    | ≤ 28,484 ns          | ≤ 27,123 ns          |

p99.9 reported informationally only (sample noise dominates over a 30s run).
