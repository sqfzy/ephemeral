# Project: eph-dpdk

> Header-only C++23 library wrapping DPDK with safer user-space TCP/IP, UDP
> unicast/multicast, ARP/DNS, and a high-level connector API for
> ultra-low-latency networking.

**Language**: C++23 | **Build**: xmake | **Style**: Header-only | **Namespace**: `eph::dpdk`

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Module Map](#module-map)
4. [Data Flow](#data-flow)
5. [Key Components](#key-components)
6. [Entry Points and APIs](#entry-points-and-apis)
7. [Dependencies](#dependencies)
8. [Testing](#testing)

---

## Overview

eph-dpdk is the DPDK backend for the eph networking ecosystem. It replaces
the kernel network stack with DPDK poll-mode drivers for sub-microsecond I/O,
provides a complete user-space TCP state machine, UDP unicast and multicast
support, ARP and DNS resolution (needed in exclusive-mode PMDs where kernel
APIs are unavailable), and a protocol-aware Reactor for multiplexed RX
dispatch.

The library is strictly header-only. All functions are `inline` and all data
structures are defined in-place, letting the compiler optimize across module
boundaries. Performance-critical paths (TX, RX dispatch) use precomputed
packet templates, zero-copy parse views, and compile-time protocol dispatch
via `if constexpr` and template parameters.

Target users are HFT client systems needing deterministic latency on
DPDK-capable NICs (Mellanox ConnectX, Intel X710, AWS ENA). The intentional
design constraints — no retransmission, no congestion control, no Nagle, no
SACK — are acceptable in data-center environments where packet loss is near
zero and the loss strategy is "detect gap, immediate reconnect (~2 ms)".

The subproject also provides the real DPDK client used by the latency
benchmarks at the workspace root, and its `scripts/` directory contains DPDK
hugepage and VFIO binding helpers (not invoked by the test suite).

---

## Architecture

eph-dpdk is organised into four strictly-downward layers. Each layer only
depends on headers in the same or lower layers.

```
+------------------------------------------------------------------+
|  Layer 3 : Application Entry                                      |
|  reactor.hpp   connector.hpp   flow_steering.hpp   types.hpp      |
+------------------------------------------------------------------+
|  Layer 2 : Protocol Implementation                                |
|  tcp.hpp   udp.hpp   arp.hpp   dns.hpp   multicast.hpp            |
+------------------------------------------------------------------+
|  Layer 1 : Packet Processing (net:: namespace)                    |
|  packet_core.hpp   packet_parse.hpp   packet_template.hpp         |
|                (net_header.hpp is a 13-line umbrella)             |
+------------------------------------------------------------------+
|  Layer 0 : DPDK Resource Management                               |
|  eal.hpp   platform.hpp                                           |
+------------------------------------------------------------------+
        detail/logger.hpp  (leaf helper, usable from any layer)
```

`dpdk.hpp` is a convenience umbrella that pulls in `eal.hpp`, `platform.hpp`,
`connector.hpp`, `udp.hpp`, and `types.hpp` — enough for the common
"EalGuard + Platform + connect() + send" pattern.

---

## Module Map

| Module | Lines | Responsibility | Key types | Depends on |
|---|---|---|---|---|
| `detail/logger.hpp` | 48 | Compile-time named spdlog factory via C++20 NTTP | `LoggerName`, `get_logger` | spdlog |
| `packet_core.hpp` | 456 | Constants, byte order, checksum, `ConnectionTuple`, IPv4/MAC format | `UdpHeader`, `ConnectionTuple` | detail/logger |
| `packet_parse.hpp` | 386 | Layered packet parsing (IP / TCP / UDP) | `ParsedIpHeader`, `ParsedPacket`, `ParsedUdpPacket` | packet_core |
| `packet_template.hpp` | 445 | Precomputed TX header templates | `PacketTemplate`, `UdpPacketTemplate` | packet_core |
| `net_header.hpp` | 13 | Umbrella header — includes all three packet_* files | — | packet_core / parse / template |
| `eal.hpp` | 134 | EAL init/cleanup and RAII guard | `EalGuard`, `eal_init`, `eal_cleanup` | detail/logger |
| `platform.hpp` | 704 | Per-port NIC lifecycle (mempool, queues, link-up) | `Platform`, `PlatformConfig`, `Platform::Stats` | eal, detail/logger |
| `tcp.hpp` | 1544 | User-space TCP state machine | `TcpSession<ReorderSlots>`, `TcpConfig`, `TcpState`, `Stats` | arp, net_header |
| `udp.hpp` | 377 | UDP unicast TX | `UdpSender`, `UdpConfig`, `UdpSegment`, `UdpSenderStats` | net_header |
| `arp.hpp` | 291 | Stateless blocking ARP resolver | `ArpPacket`, `arp::resolve` | net_header |
| `dns.hpp` | 666 | DNS A-record resolver over DPDK | `DnsHeader`, `DnsConfig`, `dns::resolve` | net_header, aws-lc |
| `multicast.hpp` | 816 | UDP multicast receiver + MoldUDP64 adapter | `MulticastReceiver`, `MulticastGroup`, `MulticastConfig` | net_header |
| `flow_steering.hpp` | 455 | NIC hardware RX dispatch (rte_flow, RSS) | `RxDispatchMode`, `FlowProtocol`, `FlowRule`, `install_flow_rule` | net_header |
| `reactor.hpp` | 577 | Multiplexed RX for 16 TCP + 8 UDP | `Reactor<bool EnableUdp>`, `ReactorConfig`, `ReactorEntry` | tcp, net_header |
| `connector.hpp` | 828 | One-call connection setup | `DpdkEndpoint`, `ConnectorOptions`, `ConnectResult<T>`, `connect<T>()` | platform, tcp, arp, dns, types |
| `types.hpp` | 89 | Transport type aliases | `DpdkTransport`, `DpdkRawTransport`, `DpdkLargeTransport`, ... | tcp, eph-transport |
| `dpdk.hpp` | 13 | Convenience umbrella | — | eal, platform, connector, udp, types |

Line counts are from the actual headers in the current tree.

---

## Data Flow

### TCP TX (data packet)

```
Application
   |
   v  TcpSession::send(data, len)     [tcp.hpp]
   |
   v  PacketTemplate::build_packet(pool, seq, ack, flags, window, data, len)
   |     1x rte_pktmbuf_alloc
   |     memcpy Eth + IP + TCP headers + payload
   |     software or HW-offload checksum
   v
   rte_eth_tx_burst(port, queue, &mbuf, 1)
```

### UDP TX (UdpSender hot path)

```
Application
   |
   v  UdpSender::send(data, len)   [udp.hpp]
   |
   v  UdpPacketTemplate::build(pool, data, len)
   |     1x rte_pktmbuf_alloc
   |     memcpy 42-byte precomputed header
   |     3 stores: ip_total_length, ip_id++, udp_length
   |     memcpy payload
   |     checksum (software 0 / HW offload)
   v
   rte_eth_tx_burst(port, queue, &mbuf, 1)
```

### Reactor RX (TCP + UDP on a shared queue)

```
rte_eth_rx_burst(pkts[], 32)
     |
     v  for each packet
     |
     +-- if constexpr (EnableUdp)
     |      parse_ip_header(mbuf)
     |      |
     |      +-- proto == UDP ? parse_udp_from_ip(mbuf, ip_hdr)
     |      |                      v
     |      |                  linear scan over 8 UDP entries
     |      |                  hash match -> UdpReactorCallback(payload)
     |      |
     |      +-- else fall through to TCP path
     |
     +-- parse_packet(mbuf)  (wraps parse_ip_header + parse_tcp_from_ip)
     |
     v  linear scan over 16 TCP entries, compare FNV hash (direction-symmetric)
     v  matching entry -> TcpSession::process_rx(&pkt, 1, data_cb)
     v  session.flush_pending_ack()   (delayed-ACK timer)
     v  optional on_burst_complete()  (for batched Transport flush)
```

### Connection setup (connect())

```
connect(host, DpdkEndpoint, opts)
   |
   v  validate DpdkEndpoint and ConnectorOptions
   v  Platform::create(opts.platform)
   |     enumerate_ports -> create_mempool -> configure_port (intersect offloads)
   |     -> setup_queues -> start_port -> wait_link_up
   |
   v  resolve_hostname(host) (kernel DNS, fall back to dns::resolve over DPDK)
   v  rte_eth_macaddr_get(port_id)
   v  arp::resolve(gateway_ip)   (unless opts.gateway_mac is pre-set)
   v  ephemeral port = CSPRNG()
   v  TcpSession::connect(timeout)   (SYN / SYN-ACK / ACK)
   v  TransportType::create(tcp_factory, transport_cfg)
   |
   v  ConnectResult { Platform, Transport, local_mac, gateway_mac }
```

---

## Key Components

### `TcpSession<size_t ReorderSlots>` (`tcp.hpp`)

1544-line user-space TCP state machine with CSPRNG ISN, SYN retransmit,
out-of-order reorder buffer (fixed capacity `ReorderSlots`, default 64),
delayed ACK (40 us window, piggyback-first), TIME_WAIT (2MSL), RFC 5961 RST
validation, and ARP refresh for idle connections. Satisfies the
`TcpTransport` concept from `eph-core` so it plugs into the generic
`eph-transport` layer. Not thread-safe — one lcore per session.

Interface highlights:

```cpp
std::expected<void, std::string> connect(std::chrono::milliseconds timeout);
std::expected<size_t, std::string> send(const void* data, size_t len);
BatchSendResult send_batch(const std::pair<const void*, uint16_t>* segs, uint16_t count);
template <typename F> std::expected<uint16_t, std::string> poll_rx(F&& data_cb);
template <typename F> std::expected<uint16_t, std::string>
    process_rx(rte_mbuf** pkts, uint16_t nb_pkts, F&& data_cb);
void flush_pending_ack() noexcept;
std::expected<void, std::string> close();   // graceful FIN
void reset() noexcept;                       // RST
```

Notes: loss strategy is "detect gap, signal caller to reconnect". No
retransmission, Nagle, or SACK. Sequence arithmetic is wraparound-safe.

### `Reactor<bool EnableUdp>` (`reactor.hpp`)

Template-based RX multiplexer. `Reactor<false>` compiles to identical code
as the original TCP-only Reactor; `Reactor<true>` adds UDP dispatch via the
layered parse API. Fixed-size arrays (16 TCP entries, 8 UDP entries) avoid
heap allocation on the hot path. Direction-symmetric FNV hash gives O(1)
fast-reject for 4-tuple matches. Supports `mark_reconnected()` while running
via a four-step release/acquire protocol.

### `UdpSender` and `UdpPacketTemplate` (`udp.hpp`, `packet_template.hpp`)

Fixed-peer UDP TX handle. `UdpPacketTemplate` stores a cache-line-aligned
42-byte Ethernet+IP+UDP header in network byte order at `init()` time. The
hot path (`fill()`) performs 1x `memcpy(42)`, 3 stores (`ip_total_length`,
`ip_id++`, `udp_length`), 1x payload memcpy, and either a software IP
checksum with `udp.checksum = 0` (legal for IPv4 per RFC 768) or hardware
offload via `RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM`.

`UdpSender::create()` checks `rte_eth_dev_info::tx_offload_capa` before
accepting `hw_cksum = true` and fails fast if the NIC lacks support.
`send_batch()` caps at 32 segments per burst and correctly tracks per-segment
payload lengths even when some builds fail (via `built_lens[]`).

### Layered parse API (`packet_parse.hpp`)

Three parse functions that compose for zero-redundancy dispatch:

```cpp
ParsedIpHeader  parse_ip_header(const rte_mbuf*);                    // L2+L3 only
ParsedPacket    parse_tcp_from_ip(const rte_mbuf*, const ParsedIpHeader&);
ParsedUdpPacket parse_udp_from_ip(const rte_mbuf*, const ParsedIpHeader&);
ParsedPacket    parse_packet(const rte_mbuf*);       // convenience wrapper
ParsedUdpPacket parse_udp_packet(const rte_mbuf*);   // convenience wrapper
```

All returned views are zero-copy — they reference memory inside the original
mbuf and are valid only until the mbuf is freed. This lifetime constraint is
documented in-line in `packet_parse.hpp`.

### `Platform` (`platform.hpp`)

Per-port DPDK NIC manager. Encapsulates the full lifecycle: port enumeration,
mempool creation (pool name is `eph_mbuf_p{port_id}` so multi-port setups
coexist), port configuration with NIC capability intersection, queue setup,
port start, and a polled link-up wait. Move-only with null-safe accessors
for moved-from instances. `PlatformConfig::validate_config()` is `constexpr`
and usable in `static_assert`.

### `connect()` family (`connector.hpp`)

Seven overloads that collapse Platform, MAC, ARP, DNS, TCP, and Transport
setup into one call. Kernel DNS (`getaddrinfo`) is tried first; on failure
the fallback path creates a Platform, ARPs the gateway, sends a UDP DNS
query through the NIC, and reuses the resolved MAC for the subsequent TCP
connection.

### `MulticastReceiver` (`multicast.hpp`)

UDP multicast RX with RFC 1112 MAC derivation and NIC MAC filter management.
Up to 8 groups per receiver. Supports source-specific multicast (SSM)
filtering. Uses `rte_eth_dev_set_mc_addr_list` — the full list is rebuilt on
each `join_group` / `leave_group`, deduplicated because multiple IPs can
hash to the same MAC.

### `install_flow_rule()` and `FlowRule` (`flow_steering.hpp`)

RAII handle for an `rte_flow` 4-tuple steering rule. Supports both TCP and
UDP via the `FlowProtocol` enum. The destructor calls `rte_flow_destroy`,
which is safe to invoke multiple times. `detect_rx_dispatch_mode(port_id)`
probes for rte_flow 5-tuple support, RSS TCP hash, or falls back to the
software Reactor path.

---

## Entry Points and APIs

| Entry point | Type | Description |
|---|---|---|
| `EalGuard::init(argc, argv)` | static factory | Initialize DPDK EAL once per process; RAII cleanup |
| `Platform::create(cfg)` | static factory | Bring up a NIC port (mempool + queues + link) |
| `connect(host, ep[, opts])` | function (7 overloads) | One-call TCP + Transport setup |
| `TcpSession<>{cfg, pool}` | constructor | Low-level TCP session |
| `UdpSender::create(cfg)` | static factory | Fixed-peer UDP TX |
| `MulticastReceiver{cfg}` | constructor | UDP multicast RX |
| `Reactor<bool>{cfg}` | constructor | Multiplexed RX for multiple connections |
| `arp::resolve(...)` | function | Blocking ARP request/reply (pre-connect) |
| `dns::resolve(...)` | function | Blocking DNS A-record query over DPDK |
| `install_flow_rule(...)` | function | rte_flow 4-tuple steering |
| `detect_rx_dispatch_mode(port)` | function | RSS / FlowDirector / Software probe |
| `build_udp_packet(...)` | function | One-shot UDP packet without UdpSender |

---

## Dependencies

### Internal (module graph)

```
dpdk.hpp ------> eal + platform + connector + udp + types
types.hpp -----> tcp
connector ----> tcp + arp + dns + platform + types
reactor ------> tcp + net_header
tcp ----------> arp + net_header
udp ----------> net_header
multicast ----> net_header
dns ----------> net_header
arp ----------> net_header
flow_steering -> net_header
platform -----> eal + detail/logger
eal ----------> detail/logger
net_header ---> packet_core + packet_parse + packet_template
packet_parse -> packet_core
packet_template -> packet_core
packet_core --> detail/logger
```

### External

| Package | Purpose |
|---|---|
| DPDK (vcpkg) | NIC PMD, mbuf, EAL, ethdev, rte_flow |
| aws-lc | CSPRNG via `RAND_bytes` (ISN, ephemeral ports, DNS txid) |
| spdlog | Structured logging with compile-time level filtering |
| fmt | Explicitly linked so vcpkg DPDK's bundled fmt does not shadow spdlog's vendored copy |
| eph-core | `TcpTransport` concept, JSON escape / string check utilities |
| eph-transport | Generic Transport layer and presets |
| eph-utils | TSC timestamps and CPU affinity helpers |
| eph-containers | Bounded queues used by Transport |

Build flags worth noting: `RTE_FORCE_INTRINSICS` is defined unconditionally,
`rte_config.h` is force-included, and on x86 `-mssse3` is added for DPDK's
`rte_memcpy` SSSE3 path (skipped on ARM64, where DPDK uses NEON).

---

## Testing

| Suite | Tests | Location | Coverage focus |
|---|---|---|---|
| `test_net_header` | 102 | `tests/test_net_header.cpp` | Byte order, checksum, parse, `ConnectionTuple`, IPv4 parse/format |
| `test_connector` | 67 | `tests/test_connector.cpp` | Endpoint validation, hostname resolution, options, DNS fallback |
| `test_multicast` | 64 | `tests/test_multicast.cpp` | Multicast MAC, group validation, SSM filter, lifecycle |
| `test_tcp` | 62 | `tests/test_tcp.cpp` | `TcpSession` state machine, config validation, delayed-ACK timer |
| `test_dns` | 61 | `tests/test_dns.cpp` | DNS codec, query/response, security validation |
| `test_dpdk_platform` | 48 | `tests/test_dpdk_platform.cpp` | Config validation, `clamp_desc`, `is_power_of_two_minus_one` |
| `test_udp` | 48 | `tests/test_udp.cpp` | `UdpSender`, `UdpPacketTemplate`, `UdpConfig`, `udp_checksum` |
| `test_reactor` | 45 | `tests/test_reactor.cpp` | Hash symmetry, TCP dispatch, UDP dispatch, lifecycle |
| `test_flow_steering` | 25 | `tests/test_flow_steering.cpp` | `FlowRule` RAII, `RxDispatchMode`, `FlowProtocol` |
| `test_arp` | 20 | `tests/test_arp.cpp` | ARP packet layout constants, protocol numbers |
| `test_eal` | 8 | `tests/test_eal.cpp` | `EalGuard` type traits, bool conversion |

Approximately 550 test cases across 11 suites. All tests run without DPDK
EAL using fake `rte_mbuf` structures and mock helpers in
`tests/dpdk_test_env.hpp` — no root privileges and no NIC required.

Key invariants exercised by the suite:

- Every config type's `validate()` returns documented error strings on
  invalid input and empty on valid input.
- `operator==` handles C structs (`rte_ether_addr`) via `memcmp`.
- `internet_checksum` and `tcp_checksum` match reference values for known
  packet layouts (both host and network byte order).
- `ReactorEntry::hash_tuple` is direction-symmetric.
- DNS parser rejects pointer loops, oversized labels, and claimed lengths
  that exceed the packet.
- ARP parser rejects non-reply opcodes, mismatched hardware types, and
  packets below minimum length.
- Multicast MAC derivation follows RFC 1112.
- `TcpSession` correctly handles RST (RFC 5961 validation), FIN in all
  half-close states, TIME_WAIT 2MSL expiry, and reorder-buffer overflow.

Fuzzing: `fuzzers/fuzz_dns_reply.cpp` targets the DNS response parser under
libFuzzer. Benchmarks under `benchmarks/` cover the TCP header hot path, UDP
send/checksum/parse/dispatch, DNS codec, multicast, `rte_memcpy` vs
`std::memcpy`, `rte_ring` vs `BoundedQueue`, and a composed pipeline.
