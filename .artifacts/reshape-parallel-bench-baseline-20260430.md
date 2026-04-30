# Baseline — reshape/parallel-bench

- Date: 2026-04-30 12:29 UTC
- Branch: `reshape/parallel-bench` from `961001b0`
  (rss-aware-connect HEAD, Task 1 final retro)

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
| **Total** | **186** | |

## E2E — 6 scripts (5 prior + Task 1 acceptance)

| Script | Result |
|--------|--------|
| dpdk_mp_e2e.sh                          | PASS |
| dpdk_mp_topology_e2e.sh                 | PASS |
| dpdk_mp_icmp_e2e.sh                     | PASS |
| dpdk_mp_fd_fallback_e2e.sh              | PASS |
| dpdk_mp_dynamic_e2e.sh                  | PASS |
| dpdk_mp_dynamic_tcp_handshake_e2e.sh    | PASS |

## 7 lat_*_dpdk binaries — build clean

| Binary | Build |
|--------|-------|
| lat_tcp_dpdk           | OK |
| lat_udp_dpdk           | OK |
| lat_ws_dpdk            | OK |
| lat_ex_market_dpdk     | OK |
| lat_ex_market_2p_dpdk  | OK |
| lat_ex_order_dpdk      | OK |
| lat_ex_md_udp_dpdk     | OK |

## Single-process bench parity reference (from Task 1 final retro,
   same HEAD 961001b0)

### lat_tcp_dpdk (30s, payload=256)

| Metric | ns      |
|--------|---------|
| p50    | 22,183  |
| p99    | 28,455  |

### lat_udp_dpdk

| Metric | ns      |
|--------|---------|
| p50    | 19,655  |
| p99    | 26,727  |

## Phase 3 parity gate (≤5%)

| Metric | lat_tcp budget | lat_udp budget |
|--------|----------------|----------------|
| p50    | ≤ 23,292 ns    | ≤ 20,638 ns    |
| p99    | ≤ 29,878 ns    | ≤ 28,063 ns    |
