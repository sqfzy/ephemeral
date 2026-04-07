# Project: eph-dpdk

> Header-only C++23 DPDK networking library providing user-space TCP/IP + UDP for ultra-low-latency trading systems.

**Language**: C++23 | **Build**: xmake | **Style**: Header-only | **Namespace**: `eph::dpdk`

---

## Overview

eph-dpdk replaces the kernel network stack with DPDK poll-mode drivers for sub-microsecond network I/O. It provides a complete user-space TCP state machine, UDP unicast/multicast support, ARP/DNS resolution (necessary when kernel APIs are unavailable in exclusive PMD mode), and a protocol-aware Reactor for multiplexed RX dispatch.

The library is organized in 4 layers with strict downward-only dependencies. All code is header-only with inline functions, enabling full compiler optimization across module boundaries. Performance-critical paths use precomputed packet templates, zero-copy parsing, and compile-time protocol dispatch (`if constexpr`).

Target users are HFT systems that need deterministic latency on DPDK-capable NICs (Mellanox ConnectX, Intel X710, AWS ENA).

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Layer 3: Application Entry                                     │
│  reactor.hpp  connector.hpp  flow_steering.hpp  types.hpp       │
├─────────────────────────────────────────────────────────────────┤
│  Layer 2: Protocol Implementation                               │
│  tcp.hpp  udp.hpp  arp.hpp  dns.hpp  multicast.hpp              │
├─────────────────────────────────────────────────────────────────┤
│  Layer 1: Packet Processing (split from net_header.hpp)         │
│  packet_core.hpp  packet_parse.hpp  packet_template.hpp         │
├─────────────────────────────────────────────────────────────────┤
│  Layer 0: DPDK Resource Management                              │
│  eal.hpp  platform.hpp                                          │
└─────────────────────────────────────────────────────────────────┘
```

---

## Module Map

| Module | Lines | Responsibility | Key Types | Depends On |
|--------|-------|----------------|-----------|------------|
| `packet_core.hpp` | ~450 | Constants, byte order, checksum, ConnectionTuple, IPv4 format | `UdpHeader`, `ConnectionTuple` | detail/logger.hpp |
| `packet_parse.hpp` | ~390 | Layered packet parsing (IP/TCP/UDP) | `ParsedIpHeader`, `ParsedPacket`, `ParsedUdpPacket` | packet_core |
| `packet_template.hpp` | ~450 | Precomputed TX header templates | `PacketTemplate`, `UdpPacketTemplate` | packet_core |
| `net_header.hpp` | 13 | Umbrella header (includes above 3) | — | packet_* |
| `tcp.hpp` | 3391 | TCP state machine (connect/send/recv/close) | `TcpSession<>`, `TcpConfig`, `TcpState` | packet_core, packet_template |
| `udp.hpp` | ~350 | UDP unicast TX | `UdpSender`, `UdpConfig`, `UdpSenderStats` | net_header |
| `arp.hpp` | ~300 | ARP resolution | `ArpPacket` | net_header |
| `dns.hpp` | ~500 | DNS A-record over UDP | `DnsConfig`, `DnsHeader` | net_header |
| `multicast.hpp` | ~800 | UDP multicast RX | `MulticastReceiver`, `MulticastGroup` | net_header |
| `reactor.hpp` | ~640 | Protocol-aware RX dispatch | `Reactor<EnableUdp>`, `ReactorEntry`, `UdpReactorEntry` | net_header, tcp |
| `flow_steering.hpp` | ~460 | NIC hardware dispatch | `FlowRule`, `FlowProtocol`, `RxDispatchMode` | net_header |
| `connector.hpp` | ~400 | TCP connection helper | `DpdkEndpoint`, `ConnectorOptions`, `ConnectResult` | tcp, arp, dns, platform |
| `platform.hpp` | ~400 | DPDK port/queue/mempool | `Platform`, `PlatformConfig` | eal |
| `eal.hpp` | ~100 | EAL init/cleanup | `EalGuard` | (DPDK) |
| `types.hpp` | ~90 | Transport type aliases | `DpdkTransport`, etc. | tcp, eph-transport |

---

## Data Flow

### TCP TX Path
```
Application → TcpSession::send(data, len)
  → PacketTemplate::fill_packet(mbuf, seq, ack, flags, payload)
    → memcpy precomputed headers + update dynamic fields
    → rte_eth_tx_burst(port, queue, &mbuf, 1)
```

### UDP TX Path
```
Application → UdpSender::send(data, len)
  → UdpPacketTemplate::build(pool, data, len)
    → memcpy 42B precomputed header + 3 field updates + payload
    → rte_eth_tx_burst(port, queue, &mbuf, 1)
```

### Reactor RX Dispatch (TCP + UDP)
```
rte_eth_rx_burst(pkts[], 32)
  │
  ▼ for each pkt
parse_ip_header(pkt) → { proto }
  │
  ├─ if constexpr (EnableUdp) && proto == UDP
  │    parse_udp_from_ip(pkt, ip_hdr) → callback(payload)
  │
  └─ proto == TCP
       parse_tcp_from_ip(pkt, ip_hdr) → hash match → session.process_rx
```

---

## Key Components

### `TcpSession<ReorderSlots>` (`tcp.hpp`)
3391-line TCP state machine. Implements RFC 793 client-side TCP with deferred ACK, reorder buffer (configurable 1-255 slots), and CSPRNG ISN generation. Does not implement retransmission or congestion control — loss triggers immediate reconnect. Satisfies the `TcpTransport` concept for use with eph-transport.

### `UdpSender` (`udp.hpp`)
Connected UDP TX handle. Binds a `UdpPacketTemplate` (precomputed 42-byte header) to DPDK resources (mempool, port, queue). `send()` = alloc + fill + tx_burst. `send_batch()` for burst TX. NIC checksum offload supported via `hw_cksum` flag.

### `Reactor<bool EnableUdp>` (`reactor.hpp`)
Template-based RX multiplexer. `Reactor<false>` compiles to identical code as pre-template Reactor (TCP-only). `Reactor<true>` adds UDP dispatch via `parse_ip_header` → proto check → `parse_udp_from_ip`. Uses direction-symmetric FNV hash for O(1) fast-reject on 4-tuple match. Fixed-size arrays (16 TCP + 8 UDP) avoid heap allocation on hot path.

### `UdpPacketTemplate` (`packet_template.hpp`)
Precomputed 42-byte Eth+IP+UDP header, cache-line aligned. `fill()` hot path: 1x memcpy(42B) + 3 stores (ip_total_length, ip_id++, udp_length) + 1x memcpy(payload). Software mode: UDP checksum=0 (legal for IPv4), IP checksum computed in software. HW mode: NIC offload via `RTE_MBUF_F_TX_UDP_CKSUM`.

### Layered Parse API (`packet_parse.hpp`)
Eliminates redundant L2/L3 parsing in Reactor dispatch:
- `parse_ip_header()`: L2+L3 only, returns proto for dispatch
- `parse_tcp_from_ip()` / `parse_udp_from_ip()`: L4 from pre-parsed IP
- `parse_packet()` / `parse_udp_packet()`: convenience (call layered internally)

---

## Dependencies

### Internal (module graph)

```
connector ──→ tcp ──→ packet_template ──→ packet_core
    │              ↗                         ↑
    ├──→ arp ─────┘                          │
    ├──→ dns ────────────────────────────────┘
    └──→ platform ──→ eal

reactor ──→ tcp
    └──→ packet_parse ──→ packet_core

udp ──→ packet_template ──→ packet_core
multicast ──→ packet_parse ──→ packet_core
flow_steering ──→ packet_core
```

### External

| Package | Version | Purpose |
|---------|---------|---------|
| DPDK | ≥23.11 | NIC PMD, mbuf, EAL, ethdev, rte_flow |
| aws-lc | latest | CSPRNG (RAND_bytes) for ISN, ephemeral ports, DNS txids |
| spdlog | ≥1.12 | Structured logging, compile-time level filtering |
| eph-core | internal | TcpTransport concept, JSON utilities |
| eph-transport | internal | Generic Transport layer, presets |
| eph-utils | internal | TSC timestamps, CPU affinity |

---

## Testing

| Test Suite | Tests | Location | Coverage Focus |
|------------|-------|----------|----------------|
| test_net_header | 102 | tests/test_net_header.cpp | Byte order, checksum, parse_ip_header, parse_tcp/udp_from_ip, ConnectionTuple, IPv4 format |
| test_udp | 42 | tests/test_udp.cpp | UdpSender, UdpPacketTemplate, UdpConfig, udp_checksum, FlowProtocol, ParsedUdpPacket |
| test_multicast | 64 | tests/test_multicast.cpp | Multicast MAC derivation, group validation, ParsedUdpPacket, MulticastReceiver |
| test_reactor | 45 | tests/test_reactor.cpp | Reactor hash, TCP dispatch, UDP dispatch, Config, entry management |
| test_tcp | 56 | tests/test_tcp.cpp | TcpSession state machine, config validation, stats |
| test_flow_steering | ~20 | tests/test_flow_steering.cpp | FlowRule RAII, RxDispatchMode, FlowProtocol |
| test_connector | ~30 | tests/test_connector.cpp | Endpoint validation, hostname resolution, options |
| test_dns | ~70 | tests/test_dns.cpp | DNS codec, query/response, security validation |
| **Total** | **~430+** | | |

All tests run without DPDK EAL using fake mbufs and mock structures.
