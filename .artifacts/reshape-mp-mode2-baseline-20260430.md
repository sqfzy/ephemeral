# Baseline — reshape/mp-mode2-dynamic

- Date: 2026-04-30 07:01 UTC
- Branch: `reshape/mp-mode2-dynamic` (off `main` HEAD `3676878b`)
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`

## Build

`xmake build -g tests` — OK in 4.0s incremental.

## Unit baseline (must remain green after every stage)

| Suite | Cases | Result |
|-------|-------|--------|
| test_icmp_dispatch | 10 | PASS |
| test_mp_registry | 13 | PASS |
| test_mp_topology | 20 | PASS |
| test_dpdk_multiprocess_config | 27 | PASS |
| test_mp_ipc | 12 | PASS |
| test_icmp_directory | 17 | PASS |
| test_flow_rule_variant | 10 | PASS |
| test_fd_ipc_handlers | 6 | PASS |
| test_flow_steering | 51 | PASS |
| **Total** | **166** | — |

## E2E baseline (must remain green after every stage)

| E2E | Result |
|-----|--------|
| `dpdk_mp_e2e.sh` (legacy MP) | PASS |
| `dpdk_mp_topology_e2e.sh` (MpTopology) | PASS |
| `dpdk_mp_icmp_e2e.sh` (cross-proc ICMP) | PASS |
| `dpdk_mp_fd_fallback_e2e.sh` (FlowDir fallback) | PASS |

## Bench baseline (30s sample, payload 256, NIC_B vfio-pci)

### lat_tcp_dpdk

| Metric | ns |
|--------|-----|
| samples | 1,346,679 |
| RTT p50 | 21,767 |
| RTT p99 | 27,591 |
| RTT p99.9 | 38,830 |

### lat_udp_dpdk

| Metric | ns |
|--------|-----|
| samples | 1,417,238 |
| RTT p50 | 19,495 |
| RTT p99 | 69,149 (high — sample noise; baseline) |
| RTT p99.9 | 72,093 |

## DPDK shared resource state (pre-construction)

- `0000:28:00.0` bound to vfio-pci ✓
- HugePages_Free: 1020 / 1024
- No DPDK process running
- `/var/run/dpdk/` empty

Ready to proceed to stage 1.
