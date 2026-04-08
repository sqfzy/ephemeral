# Design Report

## Overview
- Time: 2026-04-07
- Mode: default (auto, plan-driven)
- Requirement: Add UDP unicast sender support to eph-dpdk
- Review rounds: 0 (design pre-validated via /discuss + /plan)
- Commit: f5f0f7e

## Scope
**In scope**: UdpPacketTemplate, UdpSender, udp_checksum, build_udp_packet, Flow Steering UDP
**Out of scope**: UDP RX, Reactor extension, UdpTransport concept, DNS refactor, IP fragmentation

## Implementation Summary

### New files
- `eph-dpdk/include/eph/dpdk/udp.hpp` — UdpConfig, UdpSender, UdpSegment, UdpSenderStats, build_udp_packet
- `eph-dpdk/tests/test_udp.cpp` — 30 unit tests

### Modified files
- `eph-dpdk/include/eph/dpdk/net_header.hpp` — +209 lines: kUdpAllHeadersLen, udp_checksum, UdpPacketTemplate
- `eph-dpdk/include/eph/dpdk/flow_steering.hpp` — +64 lines: FlowProtocol enum, install_flow_rule protocol parameter
- `eph-dpdk/include/eph/dpdk.hpp` — +1 line: include udp.hpp

### Test results
- 30 new tests: all passing
- 88 existing net_header tests: no regression
- Full project build: clean

### Key design decisions
| Decision | Choice | Rationale |
|----------|--------|-----------|
| Abstraction layers | UdpPacketTemplate + UdpSender (two-layer) | Matches TCP's PacketTemplate + TcpSession pattern |
| hw_cksum default | false (checksum=0) | IPv4 UDP checksum optional per RFC 768; lowest latency |
| Flow Steering | Protocol parameter with Tcp default | Backward compatible; ~60 lines of changes |
| Naming | UdpSender (not UdpSession/UdpEndpoint) | No state machine; consistent with MulticastReceiver |
| Concept | None | UDP too simple for concept abstraction |

## Next steps
- DNS refactor: `build_dns_packet` to use UdpPacketTemplate (separate PR)
- Reactor UDP support when unicast RX use case emerges
- Benchmark UdpSender TX throughput vs raw rte_eth_tx_burst
