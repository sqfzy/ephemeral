# eph-dpdk

Header-only C++23 library for ultra-low-latency networking over DPDK, bypassing the kernel network stack entirely. Provides a full user-space TCP/IP implementation, ARP and DNS resolution, multicast reception, NIC flow steering, and a high-level connector API for one-call WebSocket-over-TLS connections.

Part of the **eph** HFT library ecosystem. Serves as the DPDK backend for the generic `eph-transport` layer, replacing kernel sockets with direct NIC I/O via DPDK poll-mode drivers.

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

- **types.hpp** -- DPDK-specific type aliases for the generic `eph-transport` layer. Pre-configured transport presets using `TcpSession<>` as the TCP backend (see transport type aliases table below).
- **connector.hpp** -- High-level connection helper collapsing the Platform -> MAC -> ARP -> DNS -> TCP -> Transport sequence into a single `connect()` call. Multiple overloads from simplest (hostname + endpoint) to full control (pre-resolved IP, custom TransportConfig, existing Platform). `DpdkEndpoint` specifies local/gateway IPs; `ConnectorOptions` nests `PlatformConfig` for full tunability. DNS resolution tries kernel first, falls back to DPDK DNS.

### Layer 4: Multi-Connection & Multicast

- **reactor.hpp** -- Epoll-style multiplexed RX for up to 16 DPDK connections on a single NIC queue. A single RX thread polls the NIC and dispatches packets to the correct `TcpSession` via 4-tuple hash match. Zero ring overhead -- direct inline dispatch. Supports live reconnection (`mark_reconnected()`). Designed for NICs without RSS/Flow Director (e.g., AWS ENA).
- **flow_steering.hpp** -- NIC hardware RX dispatch: runtime detection of NIC capabilities (`detect_rx_dispatch_mode()`) returning `Software`, `RssPartitioned`, or `FlowDirector`. `configure_rss()` sets up RSS hash + RETA table. `install_flow_rule()` creates rte_flow 5-tuple rules for per-connection queue steering. RAII `FlowRule` handle auto-removes rules on destruction.
- **multicast.hpp** -- UDP multicast receiver for equity market data feeds (CME MDP3.0, Nasdaq TotalView/MoldUDP64). `MulticastReceiver` manages group membership via NIC MAC filters (RFC 1112 multicast MAC derivation), delivers zero-copy UDP payloads via callback. Supports source-specific multicast (SSM) filtering. `make_moldudp64_adapter()` bridges to eph-itch parsers.

### Convenience Header

- **dpdk.hpp** -- Includes `eal.hpp`, `platform.hpp`, `connector.hpp`, and `types.hpp` for the common case.

## Public API Reference

### eal.hpp -- `eph::dpdk`

| Symbol | Kind | Description |
|--------|------|-------------|
| `eal_init(argc, argv)` | Function | Initialize DPDK EAL. Returns args consumed or error string. |
| `eal_cleanup()` | Function | Clean up EAL resources. No further rte_* calls after this. |
| `EalGuard` | Class (RAII) | Move-only guard; calls `eal_cleanup()` on destruction. |
| `EalGuard::init(argc, argv)` | Static factory | Returns `expected<EalGuard, string>`. |
| `EalGuard::args_consumed()` | Accessor | Number of argv entries consumed by EAL. |
| `EalGuard::initialized()` | Accessor | Whether this guard holds a valid EAL instance. |

### platform.hpp -- `eph::dpdk`

| Symbol | Kind | Description |
|--------|------|-------------|
| `PlatformConfig` | Struct | Port/queue/mempool configuration. Fields: `port_id`, `nb_rx_queues`, `nb_tx_queues`, `nb_rx_desc`, `nb_tx_desc`, `mbuf_pool_size` (must be 2^n-1), `mbuf_cache_size`, `enable_promiscuous`, `link_timeout_ms`. |
| `validate_config(cfg)` | constexpr fn | Returns empty `string_view` on success, error description otherwise. |
| `config_ok(cfg)` | constexpr fn | Boolean wrapper for `static_assert` use. |
| `Platform` | Class | NIC port manager. Owns configured port + mempool. Move-only. |
| `Platform::create(config)` | Static factory | Returns `expected<Platform, string>`. Full init sequence. |
| `Platform::mempool()` | Accessor | `rte_mempool*` for this port. |
| `Platform::port_id()` | Accessor | DPDK port ID. |
| `Platform::is_running()` | Accessor | Whether the port has been started. |
| `Platform::is_promiscuous()` | Accessor | Whether promiscuous mode is active. |
| `Platform::collect_stats()` | Method | Returns `Platform::Stats` snapshot (rx/tx packets, bytes, errors). |
| `Platform::Stats` | Struct | Cumulative NIC counters. Supports `operator-` for deltas, `dump()`, `to_json()`. |

### net_header.hpp -- `eph::dpdk::net`

| Symbol | Kind | Description |
|--------|------|-------------|
| `hton16` / `hton32` | constexpr fn | Host-to-network byte order conversion. |
| `ntoh16` / `ntoh32` | constexpr fn | Network-to-host byte order conversion. |
| `internet_checksum(data, len)` | Function | RFC 1071 one's complement checksum. |
| `pseudo_header_sum(...)` | Function | TCP/UDP pseudo-header partial sum. |
| `tcp_checksum(...)` | Function | Full TCP checksum including pseudo-header. |
| `write_syn_options(buf, mss)` | Function | Encode MSS + SACK_PERM + WSCALE into SYN options (12 bytes). |
| `ConnectionTuple` | Struct | TCP 4-tuple: `src_ip`, `dst_ip`, `src_port`, `dst_port` (host order). |
| `PacketTemplate` | Struct | Pre-filled Eth+IP+TCP header template. `build_packet()` (allocating) and `fill_packet()` (zero-alloc). Supports HW checksum offload via `hw_cksum` flag. |
| `ParsedPacket` | Struct | Zero-copy parsed view of a received packet. Accessors: `seq()`, `ack()`, `window()`, `src_port()`, `dst_port()`, `src_ip()`, `dst_ip()`, `tcp_flags()`, `matches(tuple)`, `has_flag(f)`. |
| `parse_packet(mbuf)` | Function | Parse Eth/IPv4/TCP from mbuf. Returns `ParsedPacket` (all-null on invalid). |
| `parse_ipv4(str)` | Function | Parse "a.b.c.d" to host-order `uint32_t`. Returns 0 on failure. |
| `format_ipv4(ip)` | Function | Format host-order IP to `array<char, 16>`. |
| `format_mac(mac)` | Function | Format MAC to `array<char, 18>`. |
| Constants | | `kEtherTypeIpv4`, `kIpProtoTcp`, `kIpProtoUdp`, `kTcpSyn/Ack/Fin/Rst/Psh/Urg`, `kDefaultMss`, `kAllHeadersLen`, etc. |

### tcp.hpp -- `eph::dpdk`

| Symbol | Kind | Description |
|--------|------|-------------|
| `TcpConfig` | Struct | Session config: `tuple`, `src_mac`, `dst_mac`, `mss`, `recv_window`, `port_id`, `tx_queue_id`, `rx_queue_id`, `max_rx_burst`. Has `validate()`, `dump()`, `to_json()`. |
| `TcpSession<ReorderSlots>` | Class template | User-space TCP state machine. Default `ReorderSlots = 64`. |
| `TcpSession::connect(timeout)` | Method | Blocking three-way handshake. Returns `expected<void, string>`. |
| `TcpSession::send(data, len)` | Method | Send data. Returns `expected<size_t, string>` (bytes sent). |
| `TcpSession::process_rx(pkts, n, cb)` | Method | Process received mbufs, deliver payload via callback. Used by Reactor. |
| `TcpSession::poll_rx(cb)` | Method | Poll NIC RX queue and process. Returns data packet count or error. |
| `TcpSession::flush_pending_ack()` | Method | Send pending ACK if any (call after process_rx batch). |
| `TcpSession::close()` | Method | Graceful TCP close (FIN). Returns `expected<void, string>`. |
| `TcpSession::reset()` | Method | Hard reset (RST). |
| `TcpSession::state()` | Accessor | Current `TcpState` enum value. |
| `TcpSession::is_established()` | Accessor | True if state is `Established`. |
| `TcpSession::connection_tuple()` | Accessor | The `ConnectionTuple` for this session. |
| `TcpSession::stats()` / `tcp_stats()` | Accessor | Cumulative `Stats` snapshot. |
| `TcpSession::Stats` | Struct | Counters: `tx/rx_packets`, `tx/rx_bytes`, `acks_sent`, `out_of_order`, `resets_received`, `reorder_hits/overflows`, `max_gap_size`, `gap_histogram[32]`. Supports `operator-`, `dump()`, `to_json()`. |

### arp.hpp -- `eph::dpdk::arp`

| Symbol | Kind | Description |
|--------|------|-------------|
| `ArpPacket` | Struct | Wire-format ARP payload (28 bytes, packed). |
| `build_arp_request(pool, src_mac, src_ip, target_ip)` | Function | Build ARP request mbuf. |
| `parse_arp_reply(mbuf, target_ip)` | Function | Extract sender MAC from ARP reply. Returns `optional<rte_ether_addr>`. |
| `resolve(port, queue, pool, src_mac, src_ip, target_ip, timeout)` | Function | Blocking ARP resolution. Returns `expected<rte_ether_addr, string>`. Retries up to 3 times. |
| Constants | | `kEtherTypeArp`, `kArpOpRequest/Reply`, `kBroadcastMac`, etc. |

### dns.hpp -- `eph::dpdk::dns`

| Symbol | Kind | Description |
|--------|------|-------------|
| `DnsConfig` | Struct | Resolver config: `nameserver_ip` (default 8.8.8.8), `port`, `timeout`. Has `validate()`, `dump()`, `to_json()`. |
| `UdpHeader` | Struct | Wire-format UDP header (8 bytes, packed). |
| `DnsHeader` | Struct | Wire-format DNS header (12 bytes, packed). |
| `resolve(port, queue, pool, src_mac, dst_mac, src_ip, hostname, cfg)` | Function | Blocking DNS A-record resolution over DPDK. Returns `expected<uint32_t, string>` (host-order IP). Fast path for dotted-decimal strings. |
| Constants | | `kDnsPort`, `kMaxDnsPacketLen`, `kDnsTypeA`, `kDnsClassIn`, etc. |

### types.hpp -- `eph::dpdk`

| Alias | Framing | Payload | Queue Depth | Notes |
|-------|---------|---------|-------------|-------|
| `DpdkTransport` | WebSocket | 512B | 1024 | Default for exchange WS feeds |
| `DpdkSmallTransport` | WebSocket | 64B | 256 | Control/heartbeat channels |
| `DpdkLargeTransport` | WebSocket | 4KB | 512 | JSON market data / book snapshots |
| `DpdkEvictTransport` | WebSocket | 512B | 1024 | Drops stale under backpressure |
| `DpdkRawTransport` | None | 512B | 1024 | FIX, SBE, custom protocols |
| `DpdkDirectTxTransport` | WebSocket | 512B | 1024 | App sends directly, RX via thread |
| `DpdkDirectTxRawTransport` | None | 512B | 1024 | Direct TX, no framing |
| `DpdkDirectTransport` | WebSocket | 512B | 1024 | No threads, app calls send()+poll() |
| `DpdkDirectRawTransport` | None | 512B | 1024 | No threads, no framing |

Also re-exports: `TransportConfig`, `TransportStats`, `SendError`, `TransportEvent`, `TransportState`.

### connector.hpp -- `eph::dpdk`

| Symbol | Kind | Description |
|--------|------|-------------|
| `DpdkEndpoint` | Struct | Required: `local_ip`, `gateway_ip`. Has `validate()`, `dump()`, `to_json()`. |
| `ConnectorOptions` | Struct | Optional: nested `PlatformConfig`, `local_port` (0 = random ephemeral), `tx/rx_queue_id`, `gateway_mac` (skip ARP), `arp_timeout`, `connect_timeout`, `DnsConfig`. |
| `ConnectResult<T>` | Struct | Result: `platform`, `transport`, `local_mac`, `gateway_mac`. |
| `connect(host, ep, opts)` | Function | Simplest: hostname + endpoint. Builds default TransportConfig (port 443, TLS). |
| `connect(ep, transport_cfg, opts)` | Function | DNS-resolving: kernel first, DPDK DNS fallback. |
| `connect(ep, transport_cfg, server_ip, opts)` | Function | Pre-resolved IP. No DNS needed. |
| `connect(platform, ...)` | Function | Reuse existing Platform for multi-connection setups. Returns `unique_ptr<Transport>`. |
| `resolve_hostname(host)` | Function | Kernel DNS resolution. Returns `expected<uint32_t, string>`. |

### reactor.hpp -- `eph::dpdk`

| Symbol | Kind | Description |
|--------|------|-------------|
| `kReactorMaxConnections` | Constant | 16. Fixed array avoids heap allocation on hot path. |
| `ReactorDataCallback` | Type alias | `function<void(const uint8_t*, uint16_t, size_t)>` -- data + length + conn_id. |
| `BurstCompleteCallback` | Type alias | `function<void()>` -- called after each NIC burst cycle. |
| `ReactorEntry` | Struct | Per-connection: atomic session pointer, tuple, callback, connected flag, direction-symmetric hash. |
| `Reactor` | Class | Multiplexed RX dispatcher. Owns one RX thread. |
| `Reactor::Config` | Struct | `port_id`, `rx_queue_id`, `rx_cpu` (-1 = no pin). |
| `Reactor::add_connection(session, cb)` | Method | Register a connected TcpSession. Must be called before `start()`. Returns conn index. |
| `Reactor::start()` / `stop()` | Methods | Start/stop the RX polling thread. |
| `Reactor::mark_disconnected(id)` | Method | Skip processing for a connection. Safe while running. |
| `Reactor::mark_reconnected(id, session)` | Method | Atomically swap session pointer and re-enable. Safe while running. |
| `Reactor::set_on_burst_complete(cb)` | Method | Register post-burst callback. Must be called before `start()`. |

### flow_steering.hpp -- `eph::dpdk`

| Symbol | Kind | Description |
|--------|------|-------------|
| `RxDispatchMode` | Enum | `Software`, `RssPartitioned`, `FlowDirector`. |
| `rx_dispatch_mode_name(mode)` | constexpr fn | Human-readable mode name. |
| `detect_rx_dispatch_mode(port_id)` | Function | Probe NIC capabilities. Returns best mode. |
| `configure_rss(port_id, num_queues)` | Function | Set up RSS hash + RETA table. Returns actual queue count or error. |
| `FlowRule` | Struct (RAII) | Move-only handle for an installed rte_flow rule. Auto-removes on destruction via `remove()`. |
| `install_flow_rule(port_id, queue_id, tuple)` | Function | Create 5-tuple flow rule. Returns `expected<FlowRule, string>`. |

### multicast.hpp -- `eph::dpdk`

| Symbol | Kind | Description |
|--------|------|-------------|
| `kMaxMulticastGroups` | Constant | 8. Fixed-size group table. |
| `multicast_mac_from_ip(group_ip)` | constexpr fn | RFC 1112 multicast MAC derivation. |
| `is_multicast_ip(ip)` | constexpr fn | Check if IP is in 224.0.0.0/4 range. |
| `ParsedUdpPacket` | Struct | Zero-copy parsed view of a UDP/IPv4/Ethernet packet. |
| `parse_udp_packet(mbuf)` | Function | Parse UDP from mbuf. Returns `ParsedUdpPacket`. |
| `MulticastGroup` | Struct | Group descriptor: `group_ip`, `group_port`, `source_ip` (0 = any). Has `validate()`. |
| `MulticastConfig` | Struct | `port_id`, `rx_queue_id`, `rx_cpu`, `rx_burst`. Has `validate()`. |
| `MulticastPacketCallback` | Type alias | `function<void(const uint8_t*, size_t)>`. |
| `MulticastReceiver` | Class | Manages NIC MAC filters, owns RX thread, delivers UDP payloads. |
| `MulticastReceiver::join_group(group)` | Method | Add multicast group. Returns group index or error. Idempotent. |
| `MulticastReceiver::leave_group(idx)` | Method | Remove group by index. |
| `MulticastReceiver::on_packet(cb)` | Method | Register payload callback. |
| `MulticastReceiver::start()` / `stop()` | Methods | Start/stop RX polling thread. |
| `make_payload_adapter(parser)` | Function | Generic UDP payload adapter for `on_packet()`. |
| `make_moldudp64_adapter(parse_fn, msg_cb)` | Function | MoldUDP64 protocol adapter. Bridges to eph-itch parsers. |

## Dependencies

| Package | Purpose |
|---------|---------|
| **eph-core** | TCP session concept (`tcp_concept.hpp`), JSON escape utilities |
| **eph-transport** | Generic Transport layer, presets, transport type aliases |
| **eph-utils** | TSC timestamps (`time.hpp`), CPU affinity (`cpu.hpp`) |
| [DPDK](https://www.dpdk.org/) | NIC PMD, mbuf, EAL, ethdev, rte_flow |
| [aws-lc](https://github.com/aws/aws-lc) | CSPRNG (`RAND_bytes`) for ephemeral ports and DNS transaction IDs |
| [spdlog](https://github.com/gabime/spdlog) | Structured logging with compile-time level filtering |

Optional (for multicast adapter integration):
- **eph-itch** -- Required only when using `make_moldudp64_adapter()` in `multicast.hpp`

## Usage Examples

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

### Connection with custom options

```cpp
#include <eph/dpdk.hpp>

auto result = eph::dpdk::connect(
    "stream.example.com",
    eph::dpdk::DpdkEndpoint{"10.0.0.100", "10.0.0.1"},
    eph::dpdk::ConnectorOptions{
        .platform = {.nb_rx_queues = 2, .nb_tx_queues = 2},
        .local_port = 5000,
        .connect_timeout = std::chrono::milliseconds{3000},
    });
```

### Multi-connection with shared Platform

```cpp
auto result1 = eph::dpdk::connect("feed1.example.com", ep);
// Reuse Platform for second connection (same NIC port)
auto transport2 = eph::dpdk::connect(
    result1->platform, "feed2.example.com", ep,
    eph::dpdk::ConnectorOptions{.gateway_mac = result1->gateway_mac});
```

### Raw TCP (no WebSocket framing)

```cpp
auto result = eph::dpdk::connect<eph::dpdk::DpdkRawTransport>(
    "fix-gateway.example.com",
    eph::dpdk::DpdkEndpoint{"10.0.0.100", "10.0.0.1"});
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

### Flow steering (per-connection NIC queue)

```cpp
#include <eph/dpdk/flow_steering.hpp>

auto mode = eph::dpdk::detect_rx_dispatch_mode(port_id);
if (mode == eph::dpdk::RxDispatchMode::FlowDirector) {
    auto rule = eph::dpdk::install_flow_rule(port_id, queue_id, tuple);
    if (rule) {
        // Session polls from dedicated queue -- zero dispatcher overhead
        // FlowRule auto-removes on destruction
    }
} else if (mode == eph::dpdk::RxDispatchMode::RssPartitioned) {
    auto queues = eph::dpdk::configure_rss(port_id, 4);
}
```

### Compile-time config validation

```cpp
constexpr eph::dpdk::PlatformConfig cfg{
    .nb_rx_queues = 2,
    .mbuf_pool_size = 4095,  // 2^12 - 1
};
static_assert(eph::dpdk::config_ok(cfg), "bad platform config");
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

// Native PMD (production -- bind NIC first: dpdk-devbind.py -b vfio-pci 0000:03:00.0)
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
| TIME_WAIT (2MSL) | |

**Loss strategy**: detect out-of-order/loss -> immediate reconnect (~2ms). Acceptable for datacenter environments with near-zero packet loss.

**Reorder buffer**: `TcpSession<ReorderSlots>` template parameter (default 64) controls out-of-order buffering depth. Stats include `reorder_hits`, `reorder_overflows`, `max_gap_size`, and a log2 gap histogram for diagnosing loss patterns.

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
# Disable GRO -- merged packets create TCP sequence gaps that trigger reconnect
sudo ethtool -K eth0 gro off

# Block kernel RST -- kernel doesn't know about eph-dpdk's TCP connections
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
