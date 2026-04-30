# Baseline — reshape/parallel-bench v2 (single-process multi-lcore)

- Date: 2026-04-30 15:19 UTC
- Branch: `reshape/parallel-bench` HEAD `bae608b6`
- Direction: single-process N-lcore (replaces autojoin MP, hard-reset away)

## Unit suites — 12 binaries

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
| test_bench_conf                 | 20  | PASS |

## E2E — 6 scripts

| Script | Result |
|--------|--------|
| dpdk_mp_e2e.sh                          | PASS |
| dpdk_mp_topology_e2e.sh                 | PASS |
| dpdk_mp_icmp_e2e.sh                     | PASS |
| dpdk_mp_fd_fallback_e2e.sh              | PASS |
| dpdk_mp_dynamic_e2e.sh                  | PASS |
| dpdk_mp_dynamic_tcp_handshake_e2e.sh    | PASS |

## 7 lat_*_dpdk binaries — build clean

All 7 (lat_tcp/udp/ws/ex_market/ex_market_2p/ex_order/ex_md_udp).

## Single-process bench parity reference (30s, payload=256, NIC_B vfio-pci)

### lat_tcp_dpdk_tls

| Metric | ns | samples |
|--------|----|---------|
| p50    | 21,911 | 1,335,474 |
| p99    | 28,023 |  |
| p99.9  | 35,343 |  |

### lat_udp_dpdk

| Metric | ns | samples |
|--------|----|---------|
| p50    | 20,071 | 1,456,337 |
| p99    | 26,551 |  |
| p99.9  | 34,255 |  |

## Phase 2-5 parity gate (≤5%)

| Metric | lat_tcp budget | lat_udp budget |
|--------|----------------|----------------|
| p50    | ≤ 23,007 ns    | ≤ 21,075 ns    |
| p99    | ≤ 29,424 ns    | ≤ 27,879 ns    |
