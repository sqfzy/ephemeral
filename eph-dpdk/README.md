# eph-dpdk

Header-only C++23 library for ultra-low-latency networking over DPDK, bypassing the kernel network stack entirely. Provides a full user-space TCP/IP implementation, ARP and DNS resolution, multicast reception, NIC flow steering, and a high-level connector API for one-call WebSocket-over-TLS connections.

## Key Components

All headers are under `include/eph/dpdk/`:

### Layer 1: DPDK Platform

- **eal.hpp** -- EAL (Environment Abstraction Layer) lifecycle management. `eal_init()` / `eal_cleanup()` free functions plus `EalGuard` RAII wrapper. Must be called once per process before any other DPDK API.
- **platform.hpp** -- Per-port NIC initialization: mempool creation, port/queue configuration, descriptor clamping to NIC capabilities, link-up polling. `PlatformConfig` is constexpr-validatable (`validate_config()`, `config_ok()`). `Platform::Stats` provides packet/byte counters with delta support for interval-based monitoring.
- **net_header.hpp** -- Wire-format Ethernet/IPv4/TCP header structs with constexpr byte-order helpers (`hton16`, `hton32`), RFC 1071 `internet_checksum()`, `tcp_checksum()` with pseudo-header, SYN option encoding, `PacketTemplate` for zero-copy packet construction (software + HW checksum offload), `ParsedPacket` zero-copy packet parser, and IPv4/MAC formatting utilities.

### Layer 2: Protocol Stack

- **tcp.hpp** -- Minimal user-space TCP state machine (`TcpSession<ReorderSlots>`). Implements three-way handshake, seq/ack tracking, ACK generation, window management, FIN/RST handling, and out-of-order packet reordering. Does NOT implement retransmission, Nagle, delayed ACK, congestion control, or SACK. Loss strategy: detect gap, immediate reconnect (~2ms). Satisfies the `eph::net::TcpSessionConcept` for use with the generic Transport layer.
- **arp.hpp** -- Stateless blocking ARP resolution (`arp::resolve()`). Sends broadcast ARP request and busy-polls for reply with configurable timeout and retry. Called once before TCP connection establishment.
- **dns.hpp** -- User-space DNS resolution over DPDK (`dns::resolve()`). Sends UDP A-record queries directly through the NIC, bypassing the kernel. Essential for exclusive-mode PMDs where `getaddrinfo()` is unavailable after EAL init. Configurable nameserver (default 8.8.8.8), timeout, and retry.

### Layer 3: Transport & Connection

- **types.hpp** -- DPDK-specific type aliases for the generic `eph-transport` layer. Pre-configured transport presets using `TcpSession<>` as the TCP backend:
  - `DpdkTransport` -- Default: WS framer, 512B payload, 1024-deep queue
  - `DpdkSmallTransport` -- 64B payload, 256-deep queue (control messages)
  - `DpdkLargeTransport` -- 4KB payload, 512-deep queue (JSON market data)
  - `DpdkEvictTransport` -- Evicting RX queue (drops stale under backpressure)
  - `DpdkRawTransport` -- No WebSocket framing (FIX or custom protocols)
  - `DpdkDirectTxTransport` / `DpdkDirectTxRawTransport` -- App sends directly, RX via background thread
  - `DpdkDirectTransport` / `DpdkDirectRawTransport` -- No background threads, app calls send() + poll()
- **connector.hpp** -- High-level connection helper collapsing the Platform -> MAC -> ARP -> DNS -> TCP -> Transport sequence into a single `connect()` call. Multiple overloads from simplest (hostname + endpoint) to full control (pre-resolved IP, custom TransportConfig, existing Platform). `DpdkEndpoint` specifies local/gateway IPs; `ConnectorOptions` nests `PlatformConfig` for full tunability. DNS resolution tries kernel first, falls back to DPDK DNS.

### Layer 4: Multi-Connection & Multicast

- **reactor.hpp** -- Epoll-style multiplexed RX for up to 16 DPDK connections on a single NIC queue. A single RX thread polls the NIC and dispatches packets to the correct `TcpSession` via 4-tuple hash match. Zero ring overhead -- direct inline dispatch. Supports live reconnection (`mark_reconnected()`). Designed for NICs without RSS/Flow Director (e.g., AWS ENA).
- **flow_steering.hpp** -- NIC hardware RX dispatch: runtime detection of NIC capabilities (`detect_rx_dispatch_mode()`) returning `Software`, `RssPartitioned`, or `FlowDirector`. `configure_rss()` sets up RSS hash + RETA table. `install_flow_rule()` creates rte_flow 5-tuple rules for per-connection queue steering. RAII `FlowRule` handle auto-removes rules on destruction.
- **multicast.hpp** -- UDP multicast receiver for equity market data feeds (CME MDP3.0, Nasdaq TotalView/MoldUDP64). `MulticastReceiver` manages group membership via NIC MAC filters (RFC 1112 multicast MAC derivation), delivers zero-copy UDP payloads via callback. Supports source-specific multicast (SSM) filtering. `make_moldudp64_adapter()` bridges to eph-itch parsers.

### Convenience Header

- **dpdk.hpp** -- Includes `eal.hpp`, `platform.hpp`, `connector.hpp`, and `types.hpp` for the common case.

## Dependencies

| Package | Purpose |
|---|---|
| **eph-core** | TCP session concept (`tcp_concept.hpp`), JSON escape utilities |
| **eph-transport** | Generic Transport layer, presets, transport type aliases |
| **eph-utils** | TSC timestamps (`time.hpp`), CPU affinity (`cpu.hpp`) |
| [DPDK](https://www.dpdk.org/) | NIC PMD, mbuf, EAL, ethdev, rte_flow |
| [aws-lc](https://github.com/aws/aws-lc) | CSPRNG (`RAND_bytes`) for ephemeral ports and DNS transaction IDs |
| [spdlog](https://github.com/gabime/spdlog) | Structured logging with compile-time level filtering |

Optional (for multicast adapter integration):
- **eph-itch** -- Required only when using `make_moldudp64_adapter()` in `multicast.hpp`

## Quick Start

### Simple connection (one call)

```cpp
#include <eph/dpdk.hpp>

int main(int argc, char** argv) {
    auto eal = eph::dpdk::EalGuard::init(argc, argv);
    if (!eal) { std::cerr << eal.error() << '\n'; return 1; }

    auto result = eph::dpdk::connect(
        "stream.example.com",
        eph::dpdk::DpdkEndpoint{"10.0.0.100", "10.0.0.1"});
    if (!result) { std::cerr << result.error() << '\n'; return 1; }

    auto& transport = *result->transport;

    const char* msg = R"({"method":"SUBSCRIBE","params":["ticker"]})";
    transport.send_text(msg, strlen(msg));

    while (transport.is_running()) {
        transport.recv([](const uint8_t* data, uint16_t len) {
            // Process market data
        });
    }
}
```

### Multicast feed reception

```cpp
#include <eph/dpdk/multicast.hpp>

eph::dpdk::MulticastConfig cfg{.port_id = 0, .rx_queue_id = 0};
eph::dpdk::MulticastReceiver receiver(cfg);
receiver.join_group({.group_ip = eph::dpdk::net::parse_ipv4("233.54.12.111"),
                     .group_port = 26477});
receiver.on_packet([](const uint8_t* data, size_t len) {
    // Process UDP payload (e.g., MoldUDP64 -> ITCH messages)
});
receiver.start();
```

### Reactor (multi-connection, single RX thread)

```cpp
#include <eph/dpdk/reactor.hpp>

eph::dpdk::Reactor reactor({.port_id = 0, .rx_queue_id = 0});
reactor.add_connection(&session1, on_data1);
reactor.add_connection(&session2, on_data2);
reactor.start();
// Both connections receive data via callbacks on a single RX thread
reactor.stop();
```

## PMD (Poll Mode Driver)

eph-dpdk is **PMD-agnostic**: it uses DPDK's abstract port/queue API. PMD selection happens at `eal_init()` time:

| PMD | When to Use | Notes |
|-----|-------------|-------|
| **af_packet** | Development, any Linux NIC | Works everywhere, ~10us overhead vs native |
| **mlx5** | Production (Mellanox/NVIDIA ConnectX) | Zero-copy, HW checksum, lowest latency |
| **net_pcap** | Offline testing, CI | Read/write pcap files, no NIC needed |

```cpp
// AF_PACKET (development)
const char* args[] = {"myapp", "--vdev", "net_af_packet0,iface=eth0", "--no-huge", nullptr};
eal_init(4, const_cast<char**>(args));

// Native PMD (production — bind NIC first: dpdk-devbind.py -b vfio-pci 0000:03:00.0)
const char* args[] = {"myapp", "-a", "0000:03:00.0", nullptr};
eal_init(3, const_cast<char**>(args));
```

## TCP Design

Minimal user-space TCP -- designed for low-latency exchange feeds, not general-purpose networking.

| Implements | Does NOT Implement |
|---|---|
| seq/ack tracking | Retransmission |
| ACK generation | Nagle / delayed ACK |
| Window management | Congestion control |
| FIN/RST handling | SACK |
| Out-of-order reordering (configurable slots) | TCP timestamps |

**Loss strategy**: detect out-of-order/loss -> immediate reconnect (~2ms). Acceptable for datacenter environments with near-zero packet loss.

## RX Dispatch Modes

`detect_rx_dispatch_mode()` probes the NIC and selects the best strategy:

| Mode | NIC Requirement | How It Works |
|---|---|---|
| **FlowDirector** | rte_flow 5-tuple support | Per-connection dedicated RX queue via hardware rules |
| **RssPartitioned** | RSS TCP hash | Traffic hashed across queues, reduced contention |
| **Software (Reactor)** | None | Single thread polls NIC, dispatches by 4-tuple match |

## AF_PACKET Deployment Notes

When using `net_af_packet` PMD on a real NIC:

```bash
# Disable GRO — merged packets create TCP sequence gaps that trigger reconnect
sudo ethtool -K eth0 gro off

# Block kernel RST — kernel doesn't know about eph-dpdk's TCP connections
sudo nft add table inet eph_filter
sudo nft add chain inet eph_filter output '{ type filter hook output priority 0; }'
sudo nft add rule inet eph_filter output \
    tcp sport 12345 tcp flags rst counter drop
```

## Build

```bash
xmake build eph-dpdk

# Tests (no DPDK EAL required)
xmake build -g tests
xmake run test_net_header
xmake run test_tls_record
xmake run test_websocket
xmake run test_http

# Benchmarks
xmake build bench_ws_pipeline
xmake run bench_ws_pipeline
```

## License

Part of the [ephemeral](https://github.com/user/ephemeral) project.
