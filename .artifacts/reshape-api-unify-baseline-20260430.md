# Baseline — reshape/api-unify-platform-eal-bench

- Date: 2026-04-30 09:02 UTC
- Branch: `reshape/api-unify-platform-eal-bench` (off `main` HEAD `c582c8a8`)
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`

## Build

`xmake build -g tests` — OK 2.9s incremental.

## Unit baseline (must remain green after every stage)

| Suite | Cases | Result |
|-------|-------|--------|
| test_icmp_dispatch | 10 | PASS |
| test_mp_registry | 19 | PASS |
| test_mp_topology | 20 | PASS |
| test_dpdk_multiprocess_config | 27 | PASS |
| test_mp_ipc | 12 | PASS |
| test_icmp_directory | 17 | PASS |
| test_flow_rule_variant | 10 | PASS |
| test_fd_ipc_handlers | 6 | PASS |
| test_flow_steering | 51 | PASS |
| test_bdf_sanitize | 11 | PASS |
| **Total** | **183** | — |

## E2E baseline (must remain green after every stage)

| E2E | Result |
|-----|--------|
| `dpdk_mp_e2e.sh` (declarative MP) | PASS |
| `dpdk_mp_topology_e2e.sh` (MpTopology) | PASS |
| `dpdk_mp_icmp_e2e.sh` (cross-proc ICMP) | PASS |
| `dpdk_mp_fd_fallback_e2e.sh` (FlowDir fallback) | PASS |
| `dpdk_mp_dynamic_e2e.sh` (autojoin) | PASS |

## Bench baseline (30s sample, payload 256, NIC_B vfio-pci)

### lat_tcp_dpdk

| Metric | ns |
|--------|-----|
| samples | 1,312,079 |
| RTT p50 | 22,263 |
| RTT p99 | 28,839 |
| RTT p99.9 | 35,022 |

### lat_udp_dpdk

| Metric | ns |
|--------|-----|
| samples | 1,501,423 |
| RTT p50 | 19,463 |
| RTT p99 | 25,879 |
| RTT p99.9 | 34,510 |

## DPDK shared resource state (pre-construction)

- `0000:28:00.0` bound to vfio-pci ✓
- HugePages_Free: 1024 (pre-bench)
- No DPDK process running
- `/var/run/dpdk/` clean post-baseline

Ready to proceed to stage 1.
