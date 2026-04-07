# eph-dpdk

Header-only C++23 library for ultra-low-latency networking over DPDK, bypassing the kernel network stack entirely. Provides a full user-space TCP/IP stack, UDP unicast/multicast, ARP and DNS resolution, NIC flow steering, and a high-level connector API for one-call connection setup.

Part of the **eph** HFT library ecosystem. Serves as the DPDK backend for the generic `eph-transport` layer, replacing kernel sockets with direct NIC I/O via DPDK poll-mode drivers.

## Key Components

All headers are under `include/eph/dpdk/`:

### Layer 0: DPDK Platform

- **eal.hpp** -- EAL lifecycle management. `eal_init()` / `eal_cleanup()` free functions plus `EalGuard` RAII wrapper.
- **platform.hpp** -- Per-port NIC initialization: mempool creation, port/queue configuration, descriptor clamping, link-up polling. `PlatformConfig` is constexpr-validatable.

### Layer 1: Packet Processing

Split into 3 focused files by change frequency (umbrella `net_header.hpp` includes all):

- **packet_core.hpp** -- Constants, `UdpHeader`, byte-order helpers (`hton16/32`, `ntoh16/32`), RFC 1071 `internet_checksum()`, `tcp_checksum()`, `udp_checksum()`, `ConnectionTuple` (protocol-agnostic 4-tuple), IPv4/MAC formatting, ephemeral port generation.
- **packet_parse.hpp** -- Zero-copy packet parsing with layered API:
  - `parse_ip_header()` -- L2+L3 only (protocol dispatch, ~3ns)
  - `parse_tcp_from_ip()` / `parse_udp_from_ip()` -- L4 from pre-parsed IP (zero redundancy)
  - `parse_packet()` / `parse_udp_packet()` -- convenience wrappers (L2+L3+L4 in one call)
  - `ParsedIpHeader`, `ParsedPacket` (TCP), `ParsedUdpPacket` (UDP)
- **packet_template.hpp** -- Precomputed header templates for fast TX:
  - `PacketTemplate` -- TCP: pre-fill Eth+IP+TCP, update seq/ack/flags per packet
  - `UdpPacketTemplate` -- UDP: pre-fill 42-byte header, update 3 fields per packet

### Layer 2: Protocol Stack

- **tcp.hpp** -- Minimal user-space TCP state machine (`TcpSession<ReorderSlots>`). Three-way handshake, seq/ack tracking, ACK generation, window management, FIN/RST, out-of-order reordering. Does NOT implement retransmission, Nagle, congestion control, or SACK. Loss strategy: detect gap, immediate reconnect (~2ms).
- **udp.hpp** -- UDP unicast sender (`UdpSender`). Fixed-peer design with precomputed `UdpPacketTemplate`. `send()` / `send_batch()` with NIC checksum offload support. `build_udp_packet()` convenience function for one-shot sends.
- **arp.hpp** -- Stateless blocking ARP resolution (`arp::resolve()`).
- **dns.hpp** -- User-space DNS A-record resolution over DPDK (`dns::resolve()`). Essential for exclusive-mode PMDs where `getaddrinfo()` is unavailable.

### Layer 3: Transport & Dispatch

- **reactor.hpp** -- `template <bool EnableUdp = false> class Reactor`. Epoll-style multiplexed RX for up to 16 TCP + 8 UDP connections on a single NIC queue. Single RX thread polls NIC, dispatches via 4-tuple hash match. `if constexpr` guarantees TCP-only mode has zero UDP code overhead. `Reactor<true>` uses layered parse API (`parse_ip_header` -> proto dispatch -> `parse_tcp_from_ip` / `parse_udp_from_ip`) for zero-redundancy packet processing.
- **flow_steering.hpp** -- NIC hardware RX dispatch: `detect_rx_dispatch_mode()` probes for FlowDirector/RSS/Software. `install_flow_rule()` supports both TCP and UDP 4-tuple rules via `FlowProtocol` enum. RAII `FlowRule` handle.
- **connector.hpp** -- High-level connection helper. `connect()` collapses Platform -> MAC -> ARP -> DNS -> TCP -> Transport into one call.
- **multicast.hpp** -- UDP multicast receiver for equity market data feeds. `MulticastReceiver` manages NIC MAC filters (RFC 1112), delivers zero-copy UDP payloads via callback. MoldUDP64 adapter for eph-itch integration.
- **types.hpp** -- DPDK-specific Transport type aliases (`DpdkTransport`, `DpdkRawTransport`, etc.).

### Convenience Header

- **dpdk.hpp** -- Includes `eal.hpp`, `platform.hpp`, `connector.hpp`, `udp.hpp`, and `types.hpp`.

## Public API Quick Reference

### UDP Support (New)

| Symbol | Kind | Description |
|--------|------|-------------|
| `UdpConfig` | Struct | Fixed-peer TX config: 4-tuple + MAC + port/queue + pool + hw_cksum. `validate()`, `dump()`, `to_json()`. |
| `UdpSender` | Class | Connected UDP TX handle. `create(cfg)` factory, `send(data, len)`, `send_batch(segs, count)`. |
| `UdpSenderStats` | Struct | `tx_packets`, `tx_bytes`, `tx_errors`. |
| `build_udp_packet(...)` | Function | One-shot UDP packet construction without UdpSender. |
| `UdpPacketTemplate` | Struct | Precomputed 42-byte Eth+IP+UDP header. `fill(mbuf, payload, len)` / `build(pool, payload, len)`. |
| `udp_checksum(...)` | Function | Full UDP checksum including pseudo-header (for test verification). |

### Layered Parse API (New)

| Symbol | Kind | Description |
|--------|------|-------------|
| `ParsedIpHeader` | Struct | Minimal L2+L3 parse result: `eth`, `ip`, `ihl`, `proto`. |
| `parse_ip_header(mbuf)` | Function | L2+L3 only -- protocol dispatch without L4 parsing. |
| `parse_tcp_from_ip(mbuf, ip_hdr)` | Function | TCP L4 from pre-parsed IP. Zero-redundancy. |
| `parse_udp_from_ip(mbuf, ip_hdr)` | Function | UDP L4 from pre-parsed IP. Zero-redundancy. |

### Reactor UDP Support (New)

| Symbol | Kind | Description |
|--------|------|-------------|
| `Reactor<bool EnableUdp>` | Class template | `Reactor<false>` = TCP-only (identical codegen to old Reactor). `Reactor<true>` = TCP + UDP. |
| `UdpReactorCallback` | Type alias | `function<void(const uint8_t*, uint16_t, size_t)>`. |
| `UdpReactorEntry` | Struct | UDP entry: `tuple` + `on_data`. |
| `add_udp(tuple, cb)` | Method | Register UDP entry (requires `EnableUdp`). Before `start()`. |
| `set_udp_active(id, active)` | Method | Enable/disable UDP entry at runtime. |
| `FlowProtocol` | Enum | `Tcp` / `Udp` -- for `install_flow_rule()` protocol selection. |

### Core API (Unchanged)

See full API tables for: [eal.hpp](#), [platform.hpp](#), [tcp.hpp](#), [arp.hpp](#), [dns.hpp](#), [types.hpp](#), [connector.hpp](#), [multicast.hpp](#) in the previous documentation or source headers.

## Usage Examples

### UDP unicast sender

```cpp
#include <eph/dpdk/udp.hpp>

auto sender = eph::dpdk::UdpSender::create({
    .src_ip = local_ip, .dst_ip = remote_ip,
    .src_port = 50000, .dst_port = 8080,
    .src_mac = my_mac, .dst_mac = gw_mac,
    .port_id = 0, .tx_queue_id = 0,
    .pool = platform.mempool(),
});
sender->send(payload, payload_len);
```

### Reactor with TCP + UDP on shared queue

```cpp
#include <eph/dpdk/reactor.hpp>

eph::dpdk::Reactor<true> reactor({.port_id = 0, .rx_queue_id = 0});

// TCP connections
reactor.add_connection(&tcp_session, on_tcp_data);

// UDP entries
reactor.add_udp(
    {.src_ip = local_ip, .dst_ip = remote_ip,
     .src_port = 50000, .dst_port = 8080},
    [](const uint8_t* data, uint16_t len, size_t id) {
        // Process UDP payload
    });

reactor.start();
```

### Flow steering with UDP

```cpp
auto rule = eph::dpdk::install_flow_rule(
    port_id, queue_id, tuple,
    eph::dpdk::FlowProtocol::Udp);  // UDP 4-tuple rule
```

### Simple TCP connection (one call)

```cpp
#include <eph/dpdk.hpp>

auto eal = eph::dpdk::EalGuard::init(argc, argv);
auto result = eph::dpdk::connect(
    "stream.example.com",
    eph::dpdk::DpdkEndpoint{"10.0.0.100", "10.0.0.1"});
auto& transport = *result->transport;
transport.send_text(msg, strlen(msg));
```

### Multicast feed reception

```cpp
eph::dpdk::MulticastReceiver receiver({.port_id = 0, .rx_queue_id = 0});
receiver.join_group({.group_ip = eph::dpdk::net::parse_ipv4("233.54.12.111"),
                     .group_port = 26477});
receiver.on_packet([](const uint8_t* data, size_t len) { /* ... */ });
receiver.start();
```

## Architecture

```
Layer 0: eal.hpp, platform.hpp           (DPDK resource management)
Layer 1: packet_core/parse/template.hpp  (wire-format packet processing)
Layer 2: tcp.hpp, udp.hpp, arp.hpp, dns.hpp, multicast.hpp  (protocol impl)
Layer 3: reactor.hpp, connector.hpp, flow_steering.hpp, types.hpp  (app entry)
```

Dependencies flow strictly downward. `net_header.hpp` is an umbrella for Layer 1.

## Dependencies

| Package | Purpose |
|---------|---------|
| **eph-core** | TCP session concept, JSON utilities |
| **eph-transport** | Generic Transport layer, presets |
| **eph-utils** | TSC timestamps, CPU affinity |
| [DPDK](https://www.dpdk.org/) | NIC PMD, mbuf, EAL, ethdev, rte_flow |
| [aws-lc](https://github.com/aws/aws-lc) | CSPRNG for ephemeral ports and DNS txids |
| [spdlog](https://github.com/gabime/spdlog) | Structured logging with compile-time level filtering |

## Build

```bash
xmake build eph-dpdk

# Tests (no DPDK EAL required)
xmake run test_net_header   # 102 tests — packet parsing, checksum, layered API
xmake run test_udp          # 42 tests — UdpSender, UdpPacketTemplate, UdpConfig
xmake run test_reactor      # 45 tests — Reactor TCP + UDP dispatch
xmake run test_multicast    # 64 tests — multicast receiver, ParsedUdpPacket
xmake run test_tcp          # 56 tests — TCP session state machine
```

## TCP Design

Minimal user-space TCP for low-latency exchange feeds, not general-purpose networking.

| Implements | Does NOT Implement |
|---|---|
| seq/ack tracking, ACK generation | Retransmission, Nagle |
| Window management, FIN/RST | Congestion control, SACK |
| Out-of-order reordering (configurable slots) | TCP timestamps |

**Loss strategy**: detect gap -> immediate reconnect (~2ms). Acceptable for datacenter environments with near-zero packet loss.

## License

Part of the [ephemeral](https://github.com/sqfzy/ephemeral) project.
