# eph-dpdk

Header-only C++23 library wrapping DPDK with a safer, higher-level interface for
ultra-low-latency networking. Bypasses the kernel network stack entirely and
provides a full user-space TCP/IP stack, UDP unicast/multicast, ARP/DNS
resolution, NIC flow steering, and a one-call connector for typical HFT
connection setup.

Part of the **eph** ecosystem. Serves as the DPDK backend for the generic
`eph-transport` layer, replacing kernel sockets with direct NIC I/O via DPDK
poll-mode drivers. Also provides the real DPDK client used by the latency
benchmarks at the repository root.

## Features

- **Full user-space TCP**: three-way handshake, seq/ack tracking, out-of-order
  reordering, FIN/RST, TIME_WAIT, CSPRNG ISN generation, delayed ACKs. No
  retransmission, Nagle, congestion control, or SACK (designed for data-center
  environments where packet loss triggers immediate reconnect).
- **UDP unicast sender** with precomputed 42-byte packet template for
  deterministic-latency TX, batch send, and optional NIC checksum offload.
- **UDP multicast receiver** for equity market data feeds (CME MDP3.0, Nasdaq
  TotalView/MoldUDP64), with zero-copy payload delivery and RFC 1112 MAC filter
  management.
- **Protocol-aware Reactor**: `Reactor<bool EnableUdp>` multiplexes up to 16 TCP
  and 8 UDP connections on a single NIC queue. `if constexpr` guarantees
  `Reactor<false>` has zero UDP overhead.
- **Layered zero-copy parsing**: `parse_ip_header()` (L2+L3 only) + separate
  `parse_tcp_from_ip()` / `parse_udp_from_ip()` for zero-redundancy dispatch.
- **ARP and DNS over DPDK**: blocking resolvers that work in exclusive-mode PMDs
  where the kernel network stack is unavailable.
- **NIC hardware RX dispatch**: runtime detection of RSS / Flow Director
  capability and `install_flow_rule()` supporting both TCP and UDP 4-tuple rules
  via RAII `FlowRule` handles.
- **High-level connector**: single `connect()` call that collapses
  Platform → MAC → ARP → DNS → TCP → Transport setup. Multiple overloads from
  simplest (hostname + endpoint) to full control (pre-resolved IP + custom
  TransportConfig).
- **Compile-time validation** of platform config, MSS, pool sizes (must be
  `2^n - 1`), and descriptor counts where possible.

## Quick Start

### Prerequisites

- Linux (for DPDK poll-mode drivers and hugepages)
- C++23 compiler (GCC 14+ or Clang 18+)
- [xmake](https://xmake.io) build system
- [DPDK](https://www.dpdk.org/) runtime (vcpkg package used by the parent project)
- [aws-lc](https://github.com/aws/aws-lc) for CSPRNG
- [spdlog](https://github.com/gabime/spdlog) for structured logging

ARM64 builds are supported — DPDK uses NEON intrinsics in place of the x86 SSSE3
`rte_memcpy` path, and the `RTE_FORCE_INTRINSICS` define is set unconditionally
for the `eph-dpdk` target.

### Build

From the repository root:

```bash
xmake build eph-dpdk     # header-only target, verifies headers compile
xmake build               # build the whole workspace including tests/benches
```

### Test

All tests run without DPDK EAL using fake mbufs and mock structures — no
root privileges and no NIC required:

```bash
xmake run test_net_header      # 102 tests — headers, checksum, parse, ConnectionTuple
xmake run test_tcp             # 62 tests — TcpSession state machine
xmake run test_reactor         # 45 tests — Reactor hash, TCP/UDP dispatch
xmake run test_multicast       # 64 tests — MulticastReceiver, MAC derivation
xmake run test_udp             # 48 tests — UdpSender, UdpConfig, template
xmake run test_connector       # 67 tests — endpoint validation, DNS fallback
xmake run test_dns             # 61 tests — DNS codec, security validation
xmake run test_flow_steering   # 25 tests — FlowRule RAII, dispatch mode
xmake run test_dpdk_platform   # 48 tests — config validation, clamp_desc
xmake run test_arp             # 20 tests — ARP constants, packet layout
xmake run test_eal             # 8  tests — EalGuard traits
```

## Project Structure

```
include/eph/dpdk/
  detail/logger.hpp          compile-time named spdlog factory
  packet_core.hpp            constants, byte order, checksum, ConnectionTuple
  packet_parse.hpp           ParsedIpHeader/ParsedPacket/ParsedUdpPacket + parse functions
  packet_template.hpp        PacketTemplate (TCP) + UdpPacketTemplate
  net_header.hpp             umbrella header — includes the three above
  eal.hpp                    EAL lifecycle + EalGuard RAII
  platform.hpp               PlatformConfig + Platform (port/mempool)
  tcp.hpp                    TcpSession<ReorderSlots> state machine
  udp.hpp                    UdpSender + UdpConfig + build_udp_packet()
  arp.hpp                    ArpPacket + blocking arp::resolve()
  dns.hpp                    DnsHeader + blocking dns::resolve()
  multicast.hpp              MulticastReceiver + MoldUDP64 adapter
  flow_steering.hpp          RxDispatchMode, FlowProtocol, FlowRule, install_flow_rule()
  reactor.hpp                Reactor<EnableUdp> multiplexed RX
  connector.hpp              connect() — one-call connection setup
  types.hpp                  Transport type aliases (DpdkTransport, DpdkRawTransport, ...)
  dpdk.hpp                   convenience umbrella (eal + platform + connector + udp + types)

tests/                       unit tests (run without EAL)
benchmarks/                  micro-benchmarks (tcp header, udp, memcpy, rte_ring, pipeline)
fuzzers/                     libFuzzer targets (DNS reply parser)
scripts/                     dpdk-setup.sh / dpdk-teardown.sh (hugepages, VFIO binding)
```

Layers flow strictly downward: packet_* is the foundation, protocol modules
(tcp/udp/arp/dns/multicast) build on it, and application-level modules
(reactor/connector/flow_steering/types) live on top. `dpdk.hpp` and
`net_header.hpp` are umbrella headers for common include patterns.

## Usage Examples

### Simplest TCP connection (one call)

```cpp
#include <eph/dpdk.hpp>

auto eal = eph::dpdk::EalGuard::init(argc, argv);
if (!eal) return 1;

auto result = eph::dpdk::connect(
    "stream.example.com",
    eph::dpdk::DpdkEndpoint{"10.0.0.100", "10.0.0.1"});
if (!result) return 2;

auto& transport = *result->transport;
transport.send_text(msg, msg_len);
```

### UDP unicast sender

```cpp
#include <eph/dpdk/udp.hpp>

auto sender = eph::dpdk::UdpSender::create({
    .src_ip   = local_ip,  .dst_ip   = remote_ip,
    .src_port = 50000,     .dst_port = 8080,
    .src_mac  = my_mac,    .dst_mac  = gw_mac,
    .port_id  = 0,         .tx_queue_id = 0,
    .pool     = platform.mempool(),
    .hw_cksum = true,      // NIC checksum offload
});
sender->send(payload, payload_len);
```

### Reactor with mixed TCP + UDP on a shared queue

```cpp
#include <eph/dpdk/reactor.hpp>

eph::dpdk::Reactor<true> reactor({.port_id = 0, .rx_queue_id = 0});
reactor.add_connection(&tcp_session, on_tcp_data);
reactor.add_udp(udp_tuple,
    [](const uint8_t* data, uint16_t len, size_t id) { /* process UDP */ });
reactor.set_on_burst_complete([]{ /* flush per-burst work */ });
reactor.start();
```

### Multicast market data feed

```cpp
#include <eph/dpdk/multicast.hpp>

eph::dpdk::MulticastReceiver receiver({.port_id = 0, .rx_queue_id = 0});
receiver.join_group({
    .group_ip   = eph::dpdk::net::parse_ipv4("233.54.12.111"),
    .group_port = 26477,
});
receiver.on_packet([](const uint8_t* data, size_t len) {
    // parse MoldUDP64 or exchange-specific framing
});
receiver.start();
```

### NIC flow steering (per-connection RX queue)

```cpp
#include <eph/dpdk/flow_steering.hpp>

auto mode = eph::dpdk::detect_rx_dispatch_mode(port_id);
if (mode == eph::dpdk::RxDispatchMode::FlowDirector) {
    auto rule = eph::dpdk::install_flow_rule(
        port_id, queue_id, tuple,
        eph::dpdk::FlowProtocol::Tcp);   // or ::Udp
    // RAII — rule is destroyed on scope exit
}
```

## Public API Overview

### Core modules

| Module | Key types | Notes |
|---|---|---|
| `eal.hpp` | `EalGuard`, `eal_init`, `eal_cleanup` | Process-level EAL lifetime |
| `platform.hpp` | `Platform`, `PlatformConfig`, `Platform::Stats` | Per-port init, constexpr-validatable config |
| `tcp.hpp` | `TcpSession<ReorderSlots>`, `TcpConfig`, `TcpState`, `Stats` | Client-side TCP; not thread-safe |
| `udp.hpp` | `UdpSender`, `UdpConfig`, `UdpSegment`, `UdpSenderStats`, `build_udp_packet` | Fixed-peer TX, hw checksum optional |
| `arp.hpp` | `ArpPacket`, `arp::resolve` | Blocking, retries 3× within timeout |
| `dns.hpp` | `DnsHeader`, `DnsConfig`, `dns::resolve` | Over-DPDK A-record; CSPRNG tx_id |
| `multicast.hpp` | `MulticastReceiver`, `MulticastGroup`, `make_moldudp64_adapter` | Up to 8 groups, RFC 1112 MAC |
| `reactor.hpp` | `Reactor<bool>`, `ReactorConfig`, `ReactorEntry`, `UdpReactorEntry` | 16 TCP + 8 UDP, single RX thread |
| `flow_steering.hpp` | `FlowRule`, `FlowProtocol`, `RxDispatchMode`, `install_flow_rule` | RAII rule handle |
| `connector.hpp` | `DpdkEndpoint`, `ConnectorOptions`, `ConnectResult<T>`, `connect<...>()` | 7 `connect()` overloads |
| `types.hpp` | `DpdkTransport`, `DpdkRawTransport`, `DpdkLargeTransport`, ... | Transport type aliases |

### Packet processing (`net::` namespace)

| Header | Contents |
|---|---|
| `packet_core.hpp` | `UdpHeader`, `ConnectionTuple`, `hton*/ntoh*`, `internet_checksum`, `tcp_checksum`, `udp_checksum`, `parse_ipv4`, `format_ipv4`, `format_mac` |
| `packet_parse.hpp` | `ParsedIpHeader`, `ParsedPacket` (TCP), `ParsedUdpPacket`, `parse_ip_header`, `parse_tcp_from_ip`, `parse_udp_from_ip`, `parse_packet`, `parse_udp_packet` |
| `packet_template.hpp` | `PacketTemplate` (TCP), `UdpPacketTemplate` |

All parse functions are zero-copy — the returned views reference memory inside
the original mbuf and are valid only until the mbuf is freed.

## Architecture

```
Layer 3 (application entry)
  reactor.hpp  connector.hpp  flow_steering.hpp  types.hpp

Layer 2 (protocol implementation)
  tcp.hpp  udp.hpp  arp.hpp  dns.hpp  multicast.hpp

Layer 1 (packet processing — net:: namespace)
  packet_core.hpp  packet_parse.hpp  packet_template.hpp  (+ net_header.hpp umbrella)

Layer 0 (DPDK resource management)
  eal.hpp  platform.hpp
```

Dependencies flow strictly downward. Each layer only includes headers from the
same or lower layers.

## TCP Design

The TCP session is intentionally minimal — it targets deterministic latency on
trusted data-center links, not general-purpose networking.

| Implements | Does NOT implement |
|---|---|
| Three-way handshake (SYN/SYN-ACK/ACK) | Retransmission |
| Seq/ack tracking, window management | Nagle algorithm |
| Delayed ACK (40 µs window, piggyback-first) | Congestion control |
| Out-of-order reordering (configurable slots) | SACK / DSACK |
| FIN/RST handling, TIME_WAIT (2MSL) | TCP timestamps |
| CSPRNG ISN, ephemeral port generation | Fast retransmit, RTO backoff |
| ARP refresh on idle connections | Zero-window probes |
| RFC 5961 RST validation | Keepalive |

**Loss strategy**: out-of-order segments are buffered in a fixed-size reorder
buffer (`ReorderSlots`, default 64). On buffer overflow the session signals
`process_rx` error and the caller triggers reconnect (~2 ms).

## Dependencies

| Package | Purpose |
|---|---|
| `eph-core` | `TcpTransport` concept, JSON escape / string check utilities |
| `eph-transport` | Generic Transport layer and presets (`DefaultTransport`, `RawTransport`, etc.) |
| `eph-utils` | TSC timestamps, CPU affinity |
| `eph-containers` | Bounded queues used by Transport |
| DPDK (vcpkg) | PMD, mbuf, EAL, ethdev, rte_flow |
| aws-lc | CSPRNG (`RAND_bytes`) for ISN, ephemeral ports, DNS transaction IDs |
| spdlog | Structured logging with compile-time level filtering via `SPDLOG_ACTIVE_LEVEL` |
| fmt | Explicitly linked because vcpkg DPDK bundles fmt headers that shadow spdlog's vendored fmt |

On ARM64, `RTE_FORCE_INTRINSICS` is defined and the x86-only `-mssse3` flag is
skipped. `rte_config.h` is force-included for all targets so DPDK ARM intrinsics
are visible.

## Scripts

`scripts/dpdk-setup.sh` and `scripts/dpdk-teardown.sh` handle hugepage
allocation and NIC driver binding. They require root and touch real NIC state;
they are not invoked by the test suite.

## License

Part of the ephemeral project. See the parent repository for licensing.
