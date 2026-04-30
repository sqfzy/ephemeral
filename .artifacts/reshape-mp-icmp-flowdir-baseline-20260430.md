# Baseline — reshape/mp-icmp-flowdir

- Date: 2026-04-30 05:12 UTC
- Branch: `reshape/mp-icmp-flowdir` (off `reshape/mp-topology` HEAD `89d9b1ec`)
- Plan: `/home/ec2-user/.claude/plans/lexical-wandering-cerf.md`

## Build state

`xmake build -g tests` — OK in 2.3 s incremental, 100% green.

## Test baseline (must remain green after every stage)

| Suite | Cases | Result |
|-------|-------|--------|
| `test_icmp_dispatch` | **10** | PASS (102 ms) |
| `test_mp_registry` | 13 | PASS (65 ms) |
| `test_mp_topology` | 20 | PASS (0 ms) |
| `test_dpdk_multiprocess_config` | 27 | PASS (0 ms) |
| **Total** | **70** | — |

Note: `test_icmp_dispatch` is **10 cases** (not 7 as the plan
estimated). Updated invariant baseline accordingly.

## E2E baseline (must remain green after every stage)

| E2E | Result |
|-----|--------|
| `dpdk_mp_e2e.sh` (legacy MP) | PASS — primary rc=0, secondary rc=0 |
| `dpdk_mp_topology_e2e.sh` (MpTopology) | PASS — primary rc=0, secondary rc=0 |

Both require NIC_B `0000:28:00.0` on vfio-pci (verified) +
hugepages ≥ 128 free (1020 free at baseline).

## Bench baseline (30 s sample, payload 256, NIC_B vfio-pci)

### `lat_tcp_dpdk` (TLS RTT)

| Metric | ns |
|--------|-----|
| samples | 1,059,861 |
| RTT p50 | 22,247 |
| RTT p99 | 70,493 |
| RTT p99.9 | 75,357 |
| TX p50 | 13,235 |
| TX p99 | 60,686 |
| RX p50 | 8,795 |
| RX p99 | 12,875 |
| throughput | 35,410 sps |

### `lat_udp_dpdk` (raw UDP RTT)

| Metric | ns |
|--------|-----|
| samples | 1,487,341 |
| RTT p50 | 19,575 |
| RTT p99 | 26,711 |
| RTT p99.9 | 39,310 |

## Invariant snapshot (must remain byte-identical)

- `detail::IcmpRegistry` whole module (`eph/dpdk/detail/icmp_registry.hpp`)
- `IcmpRegistry::Entry` POD layout
- `Platform::register_icmp_target` signature
- `eph::net::dpdk::FlowRule` public ctor + RAII destruct success path
- `install_flow_rule` public signature (`std::expected<FlowRule, std::string>`)
- `TcpSession::on_icmp_frag_needed` impl + `effective_mss_` plain uint16_t
- Hot path: `Stream::send` / `send_batch` / `process_burst_`
- DPDK mempool / port lifecycle ordering

## DPDK shared resource state (pre-construction)

- `0000:28:00.0` bound to vfio-pci ✓
- HugePages_Free: 1020 / 1024
- No DPDK process holding runtime dir
- `/var/run/dpdk/` empty

Ready to proceed to stage 1 (IPC scaffolding).
