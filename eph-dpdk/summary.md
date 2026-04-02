# Project: eph-dpdk

> Header-only C++23 library for ultra-low-latency networking over DPDK, providing a full user-space TCP/IP stack, ARP/DNS resolution, multicast reception, NIC flow steering, and a high-level connector API that collapses the entire NIC-to-WebSocket connection sequence into a single call.

**Language**: C++23 | **Build**: xmake | **Style**: Header-only | **Namespace**: `eph::dpdk`

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Module Map](#module-map)
4. [Data Flow](#data-flow)
5. [Key Components](#key-components)
6. [Entry Points & APIs](#entry-points--apis)
7. [Dependencies](#dependencies)
8. [Testing](#testing)

## Overview

eph-dpdk is the DPDK backend for the eph HFT library ecosystem. It replaces kernel sockets with direct NIC I/O via DPDK poll-mode drivers, targeting the sub-microsecond latency requirements of high-frequency trading systems. The library implements a minimal user-space TCP/IP stack purposefully omitting retransmission, Nagle, delayed ACK, congestion control, and SACK -- trading general-purpose robustness for deterministic low latency in datacenter environments where packet loss is near zero.

The library serves two primary use cases: unicast TCP connections to exchange WebSocket/TLS feeds (the most common path through the `connect()` API), and UDP multicast reception for equity market data feeds such as CME MDP3.0 and Nasdaq TotalView/MoldUDP64. It provides a layered API: power users can compose `Platform`, `TcpSession`, and `Transport` directly, while the common case is a single `connect("hostname", endpoint)` call that handles EAL lifecycle, NIC initialization, ARP, DNS, TCP handshake, and TLS/WebSocket setup.

eph-dpdk plugs into the generic `eph-transport` layer via the `TcpSessionConcept` from `eph-core`. The type aliases in `types.hpp` instantiate transport presets (from `eph-transport`) with `TcpSession<>` as the TCP backend, producing ready-to-use types like `DpdkTransport`, `DpdkRawTransport`, and `DpdkDirectTransport`. This design allows application code to swap between DPDK and kernel-socket transports without changing business logic.

## Architecture

eph-dpdk follows a layered architecture with four distinct tiers, each building on the one below. All layers are header-only and live under `include/eph/dpdk/`. The design prioritizes zero-copy packet paths, compile-time configuration validation, and RAII resource management. Every component uses `spdlog` with compile-time level filtering (`SPDLOG_ACTIVE_LEVEL`) for production-grade observability without hot-path overhead.

### Component Diagram

```
+-------------------------------------------------------+
|                   Layer 4: Multi-Conn                  |
|  +-------------+  +---------------+  +-------------+  |
|  |   Reactor    |  | FlowSteering  |  |  Multicast  |  |
|  | (SW mux RX) |  | (HW rte_flow) |  | (UDP mcast) |  |
|  +------+------+  +-------+-------+  +------+------+  |
+---------|-----------------|-----------------|---------+
          |                 |                 |
+-------------------------------------------------------+
|              Layer 3: Transport & Connection            |
|  +-------------+              +---------------------+  |
|  |  Connector  |-- builds --> |    types.hpp        |  |
|  | (one-call)  |              | DpdkTransport, etc. |  |
|  +------+------+              +----------+----------+  |
+---------|---------------------------------|-----------+
          |   uses                          | wraps
+-------------------------------------------------------+
|              Layer 2: Protocol Stack                    |
|  +-----------+    +----------+    +-----------+        |
|  | TcpSession|    |   ARP    |    |    DNS    |        |
|  | (tcp.hpp) |    | (arp.hpp)|    | (dns.hpp) |        |
|  +-----+-----+    +----+----+    +-----+-----+        |
+--------|----------------|---------------|-------------+
         |                |               |
+-------------------------------------------------------+
|              Layer 1: DPDK Platform                     |
|  +----------+    +------------+    +---------------+   |
|  |   EAL    |    |  Platform  |    |  NetHeader    |   |
|  | (eal.hpp)|    |(platform)  |    | (net_header)  |   |
|  +----------+    +------------+    +---------------+   |
+-------------------------------------------------------+
         |                |               |
    rte_eal_*       rte_ethdev_*     rte_mbuf / wire
```

## Module Map

| Module / File | Responsibility | Key Types | Depends On |
|---|---|---|---|
| `eal.hpp` | EAL lifecycle (once per process) | `EalGuard`, `eal_init()`, `eal_cleanup()` | DPDK `rte_eal` |
| `platform.hpp` | Per-port NIC init, mempool, stats | `Platform`, `PlatformConfig`, `Platform::Stats` | `rte_ethdev`, `rte_mempool` |
| `net_header.hpp` | Wire-format headers, checksums, packet build/parse | `PacketTemplate`, `ParsedPacket`, `ConnectionTuple` | `rte_mbuf`, `rte_ether`, `rte_ip`, `rte_tcp` |
| `tcp.hpp` | User-space TCP state machine | `TcpSession<ReorderSlots>`, `TcpConfig`, `TcpSession::Stats` | `net_header.hpp`, `eph-core/tcp_concept.hpp`, `eph-utils/time.hpp`, `aws-lc` |
| `arp.hpp` | Stateless blocking ARP resolution | `ArpPacket`, `arp::resolve()` | `net_header.hpp`, `rte_ethdev` |
| `dns.hpp` | User-space DNS over DPDK UDP | `DnsConfig`, `DnsHeader`, `UdpHeader`, `dns::resolve()` | `net_header.hpp`, `rte_ethdev`, `aws-lc` |
| `types.hpp` | DPDK transport type aliases | `DpdkTransport`, `DpdkRawTransport`, `DpdkDirectTransport`, etc. (9 aliases) | `tcp.hpp`, `eph-transport/presets.hpp` |
| `connector.hpp` | High-level one-call connection setup | `DpdkEndpoint`, `ConnectorOptions`, `ConnectResult<T>`, `connect()` overloads | `arp.hpp`, `dns.hpp`, `platform.hpp`, `tcp.hpp`, `types.hpp`, `eph-core/json_escape` |
| `reactor.hpp` | Epoll-style multiplexed RX (up to 16 conns) | `Reactor`, `ReactorEntry`, `ReactorDataCallback` | `tcp.hpp`, `net_header.hpp`, `eph-utils/cpu.hpp` |
| `flow_steering.hpp` | NIC hardware RX dispatch (RSS, rte_flow) | `RxDispatchMode`, `FlowRule`, `detect_rx_dispatch_mode()` | `net_header.hpp`, `rte_flow`, `rte_ethdev` |
| `multicast.hpp` | UDP multicast receiver for market data | `MulticastReceiver`, `MulticastGroup`, `MulticastConfig`, `ParsedUdpPacket` | `net_header.hpp`, `eph-utils/cpu.hpp`, `rte_ethdev` |
| `dpdk.hpp` | Convenience umbrella header | (re-exports) | `eal.hpp`, `platform.hpp`, `connector.hpp`, `types.hpp` |

## Data Flow

The primary data path for unicast TCP connections runs from NIC hardware through DPDK PMD to the user-space TCP session and up into the Transport layer. The `connect()` API orchestrates the setup sequence; once established, the hot path is a tight poll loop with zero kernel transitions and zero memory allocation.

For multicast, the path is simpler: NIC MAC filter delivers matching frames to an RX queue, the `MulticastReceiver` RX thread parses UDP headers, and delivers payloads via callback.

### Flow Diagram

```
 NIC Hardware (PMD)
       |
       v  rte_eth_rx_burst()
 +-----------+
 | RX Queue  |  (descriptor ring, mbufs from mempool)
 +-----------+
       |
       +--- [Unicast TCP] --------+--- [Multicast UDP] ---+
       |                          |                        |
       v                          v                        v
 +-----------+             +------------+          +--------------+
 | Reactor   |  or direct  | TcpSession |          | MulticastRx  |
 | (4-tuple  |  poll_rx()  | process_rx |          | parse_udp    |
 |  dispatch)|             | (seq/ack)  |          | group match  |
 +-----------+             +-----+------+          +------+-------+
       |                         |                        |
       v                         v                        v
  on_data callback         payload bytes           on_packet callback
       |                         |                        |
       v                         v                        v
 +------------------------------------------+    +----------------+
 | eph-transport (Transport<TcpSession<>>)   |    | eph-itch or    |
 | TLS decrypt -> WS deframe -> app recv()   |    | app parser     |
 +------------------------------------------+    +----------------+

 TX Path (unicast):
 app send() -> Transport -> TLS encrypt -> WS frame
     -> TcpSession::send() -> PacketTemplate::fill_packet()
     -> rte_eth_tx_burst() -> NIC
```

## Key Components

### `TcpSession<ReorderSlots>` (tcp.hpp)

**File**: `eph-dpdk/include/eph/dpdk/tcp.hpp`
**Purpose**: Minimal user-space TCP state machine for DPDK data plane. Handles three-way handshake, seq/ack tracking, ACK generation, window management, FIN/RST, out-of-order segment reordering, and TIME_WAIT.
**Interface**:
```cpp
explicit TcpSession(const TcpConfig& config, rte_mempool* pool) noexcept;
std::expected<void, std::string> connect(std::chrono::milliseconds timeout);
std::expected<size_t, std::string> send(const void* data, size_t len);
std::expected<int, std::string> process_rx(rte_mbuf** pkts, uint16_t n, auto cb);
std::expected<int, std::string> poll_rx(auto cb);
void flush_pending_ack();
std::expected<void, std::string> close();
void reset();
TcpState state() const noexcept;
bool is_established() const noexcept;
const net::ConnectionTuple& connection_tuple() const noexcept;
Stats tcp_stats() const noexcept;
```
**Notes**: Template parameter `ReorderSlots` (default 64, max 255) controls out-of-order buffering depth. Loss strategy is "detect gap, reconnect immediately (~2ms)" rather than retransmit. Satisfies `eph::net::TcpSessionConcept` for use with generic Transport layer. ISN generated via `RAND_bytes` (aws-lc CSPRNG). Not thread-safe -- single lcore only.

### `Platform` (platform.hpp)

**File**: `eph-dpdk/include/eph/dpdk/platform.hpp`
**Purpose**: DPDK NIC port manager. Encapsulates full port lifecycle: enumeration, mempool creation, port configuration with NIC capability intersection, queue setup, port start, and link-up polling.
**Interface**:
```cpp
static std::expected<Platform, std::string> create(const PlatformConfig& config);
rte_mempool* mempool() const noexcept;
uint16_t port_id() const noexcept;
bool is_running() const noexcept;
Stats collect_stats() const;
```
**Notes**: Uses pimpl pattern (`Platform::Impl`) to isolate DPDK C struct details. Move-only; moved-from instances return safe defaults (nullptr/0/false). `PlatformConfig` supports constexpr validation via `validate_config()` / `config_ok()` for `static_assert` use. Descriptor counts are automatically clamped to NIC-reported limits via `clamp_desc()`.

### `Connector` (connector.hpp)

**File**: `eph-dpdk/include/eph/dpdk/connector.hpp`
**Purpose**: High-level connection helper that collapses Platform -> MAC -> ARP -> DNS -> TCP -> Transport into a single `connect()` call. Multiple overloads from simplest (hostname + endpoint) to full control (pre-resolved IP, existing Platform).
**Interface**:
```cpp
// Simplest: hostname + required endpoint
template <typename T = DpdkTransport>
std::expected<ConnectResult<T>, std::string>
connect(std::string_view host, const DpdkEndpoint& ep,
        const ConnectorOptions& opts = {});

// DNS-resolving: kernel first, DPDK DNS fallback
template <typename T = DpdkTransport>
std::expected<ConnectResult<T>, std::string>
connect(const DpdkEndpoint& ep, const TransportConfig& cfg,
        const ConnectorOptions& opts = {});

// Pre-resolved IP, no DNS needed
template <typename T = DpdkTransport>
std::expected<ConnectResult<T>, std::string>
connect(const DpdkEndpoint& ep, const TransportConfig& cfg,
        uint32_t server_ip, const ConnectorOptions& opts = {});

// Reuse existing Platform (multi-connection)
template <typename T = DpdkTransport>
std::expected<std::unique_ptr<T>, std::string>
connect(Platform& platform, ...);
```
**Notes**: DNS resolution tries kernel `getaddrinfo()` first, falls back to DPDK DNS (UDP over NIC) for exclusive-mode PMDs. Ephemeral source port generated via CSPRNG. `ConnectResult` exposes all intermediate products (Platform, Transport, MACs) for reuse.

### `Reactor` (reactor.hpp)

**File**: `eph-dpdk/include/eph/dpdk/reactor.hpp`
**Purpose**: Epoll-style multiplexed RX for up to 16 DPDK connections on a single NIC queue. Single RX thread polls NIC and dispatches packets to correct TcpSession via direction-symmetric 4-tuple hash match, then inline `process_rx()`.
**Interface**:
```cpp
explicit Reactor(Config config) noexcept;
std::expected<size_t, std::string>
add_connection(TcpSession<>* session, ReactorDataCallback on_data);
void start();
void stop();
void mark_disconnected(size_t conn_id) noexcept;
void mark_reconnected(size_t conn_id, TcpSession<>* new_session) noexcept;
void set_on_burst_complete(BurstCompleteCallback cb);
```
**Notes**: Zero ring overhead -- direct inline dispatch (no `rte_ring`). Fixed-size array (`kReactorMaxConnections = 16`) avoids heap allocation on hot path. Linear scan with FNV hash pre-filter is optimal for typical HFT deployments (2-4 connections). Live reconnection supported via atomic session pointer swap with careful release/acquire ordering. Designed for NICs without RSS/Flow Director (e.g., AWS ENA).

### `PacketTemplate` / `ParsedPacket` (net_header.hpp)

**File**: `eph-dpdk/include/eph/dpdk/net_header.hpp`
**Purpose**: Zero-copy packet construction and parsing for Ethernet/IPv4/TCP headers. `PacketTemplate` pre-fills static fields once at connection setup; only dynamic fields (seq, ack, flags, payload) change per packet. `ParsedPacket` provides zero-copy views into mbuf data.
**Interface**:
```cpp
// PacketTemplate
rte_mbuf* build_packet(rte_mempool* pool, uint32_t seq,
    uint32_t ack, uint8_t flags, uint16_t window,
    const void* payload = nullptr,
    uint16_t payload_len = 0) noexcept;
uint16_t fill_packet(rte_mbuf* mbuf, uint32_t seq,
    uint32_t ack, uint8_t flags, uint16_t window,
    const void* payload = nullptr,
    uint16_t payload_len = 0) noexcept;

// Free function
ParsedPacket parse_packet(const rte_mbuf* mbuf) noexcept;
```
**Notes**: `build_packet()` allocates an mbuf and is used for SYN; `fill_packet()` reuses a pre-allocated mbuf for the hot path (no allocation). Supports both software and HW checksum offload via `hw_cksum` flag. Three safety guards in `parse_packet()` prevent buffer over-read and integer underflow. Constexpr byte-order helpers (`hton16`, `hton32`) work at compile time.

### `MulticastReceiver` (multicast.hpp)

**File**: `eph-dpdk/include/eph/dpdk/multicast.hpp`
**Purpose**: UDP multicast receiver for equity market data feeds (CME MDP3.0, Nasdaq TotalView/MoldUDP64). Manages group membership via NIC MAC filters using RFC 1112 multicast MAC derivation.
**Interface**:
```cpp
explicit MulticastReceiver(MulticastConfig config) noexcept;
std::expected<size_t, std::string>
join_group(const MulticastGroup& group);
std::expected<void, std::string> leave_group(size_t group_idx);
void on_packet(MulticastPacketCallback cb);
std::expected<void, std::string> start();
void stop();
```
**Notes**: Supports up to 8 groups (`kMaxMulticastGroups`). Source-specific multicast (SSM) filtering supported. `make_moldudp64_adapter()` bridges to eph-itch parsers without creating a hard dependency. NIC MAC filter list rebuilt on every join/leave. Join is idempotent. Group management must occur before `start()` (no hot-path locking).

### `FlowRule` / `detect_rx_dispatch_mode()` (flow_steering.hpp)

**File**: `eph-dpdk/include/eph/dpdk/flow_steering.hpp`
**Purpose**: Runtime detection of NIC RX dispatch capabilities and hardware flow steering via rte_flow. Selects between Software (Reactor), RSS, or FlowDirector modes.
**Interface**:
```cpp
RxDispatchMode detect_rx_dispatch_mode(uint16_t port_id) noexcept;
std::expected<uint16_t, std::string>
configure_rss(uint16_t port_id, uint16_t num_queues) noexcept;
std::expected<FlowRule, std::string>
install_flow_rule(uint16_t port_id, uint16_t queue_id,
                  const net::ConnectionTuple& tuple) noexcept;
```
**Notes**: `FlowRule` is RAII -- auto-removes the NIC rule on destruction via `rte_flow_destroy()`. Detection probes in order: rte_flow 5-tuple validate -> RSS TCP hash -> Software fallback. RSS configuration includes RETA table setup for even distribution. `ConnectionTuple` fields are in host byte order; the functions handle the src/dst swap for the NIC ingress perspective.

### `EalGuard` (eal.hpp)

**File**: `eph-dpdk/include/eph/dpdk/eal.hpp`
**Purpose**: RAII wrapper for DPDK EAL lifecycle. Ensures `eal_cleanup()` is called on destruction even on error paths.
**Interface**:
```cpp
static std::expected<EalGuard, std::string>
init(int argc, char** argv);
int args_consumed() const noexcept;
bool initialized() const noexcept;
```
**Notes**: Move-only. EAL is once-per-process global, intentionally separated from per-port Platform. Move-assignment calls `eal_cleanup()` on the target before transferring ownership.

## Entry Points & APIs

| Entrypoint | Type | Description |
|---|---|---|
| `eph::dpdk::connect(host, ep, opts)` | Free function template | Simplest one-call connection: hostname + DPDK endpoint -> connected Transport |
| `eph::dpdk::connect(ep, cfg, ip, opts)` | Free function template | Full-control: pre-resolved IP + custom TransportConfig |
| `eph::dpdk::connect(platform, ...)` | Free function template | Reuse existing Platform for multi-connection setups |
| `EalGuard::init(argc, argv)` | Static factory | Initialize DPDK EAL, returns RAII guard |
| `Platform::create(config)` | Static factory | Create and initialize a NIC port with mempool |
| `TcpSession::connect(timeout)` | Method | Blocking TCP three-way handshake |
| `TcpSession::send(data, len)` | Method | Send data over established TCP connection |
| `TcpSession::poll_rx(callback)` | Method | Poll NIC RX queue and deliver payload via callback |
| `arp::resolve(port, queue, pool, ...)` | Free function | Blocking ARP resolution (MAC from IP) |
| `dns::resolve(port, queue, pool, ...)` | Free function | Blocking DNS A-record resolution over DPDK UDP |
| `Reactor::add_connection(session, cb)` | Method | Register TCP session for multiplexed RX |
| `Reactor::start()` / `stop()` | Methods | Start/stop the single RX polling thread |
| `MulticastReceiver::join_group(group)` | Method | Subscribe to a multicast group via NIC MAC filter |
| `MulticastReceiver::start()` / `stop()` | Methods | Start/stop multicast RX polling thread |
| `detect_rx_dispatch_mode(port_id)` | Free function | Probe NIC for best RX dispatch strategy |
| `install_flow_rule(port, queue, tuple)` | Free function | Install rte_flow 5-tuple rule, returns RAII FlowRule |
| `configure_rss(port_id, num_queues)` | Free function | Set up RSS hash + RETA table |
| `resolve_hostname(host)` | Free function | Kernel DNS resolution (getaddrinfo wrapper) |

## Dependencies

### Internal (module graph)

```
eph-dpdk
  |
  +---> eph-transport (include path only, not xmake dep)
  |       |
  |       +---> eph-core
  |       +---> eph-net
  |
  +---> eph-core      (tcp_concept.hpp, json_escape)
  +---> eph-utils     (time.hpp / TSC, cpu.hpp / affinity)
  +---> eph-containers (public dep via xmake)
  |
  +- - -> eph-itch    (optional, for make_moldudp64_adapter)
```

### External

| Package | Version | Purpose |
|---|---|---|
| [DPDK](https://www.dpdk.org/) | via vcpkg | NIC PMD, mbuf, EAL, ethdev, rte_flow, rte_mempool |
| [aws-lc](https://github.com/aws/aws-lc) | via vcpkg | CSPRNG (`RAND_bytes`) for ISN, ephemeral ports, DNS tx_id |
| [spdlog](https://github.com/gabime/spdlog) | via vcpkg | Structured logging with `SPDLOG_ACTIVE_LEVEL` compile-time filtering |
| [fmt](https://github.com/fmtlib/fmt) | via vcpkg DPDK | Linked to satisfy vcpkg DPDK's bundled fmt symbols |

## Testing

| Test Suite | Location | Coverage Focus |
|---|---|---|
| `test_eal` | `tests/dpdk/test_eal.cpp` | EAL init/cleanup lifecycle, EalGuard RAII, move semantics |
| `test_dpdk_platform` | `tests/dpdk/test_dpdk_platform.cpp` | PlatformConfig validation, constexpr checks, Platform::create, Stats |
| `test_net_header` | `tests/dpdk/test_net_header.cpp` | Byte-order helpers, checksums, packet build/parse, IPv4/MAC formatting |
| `test_tcp` | `tests/dpdk/test_tcp.cpp` | TcpSession state machine, handshake, seq/ack, reorder buffer, FIN/RST |
| `test_arp` | `tests/dpdk/test_arp.cpp` | ARP request building, reply parsing, resolve timeout/retry |
| `test_dns` | `tests/dpdk/test_dns.cpp` | DNS query encoding, response parsing, QNAME encoding, resolve flow |
| `test_connector` | `tests/dpdk/test_connector.cpp` | DpdkEndpoint/ConnectorOptions validation, connect() error paths, DNS resolution |
| `test_reactor` | `tests/dpdk/test_reactor.cpp` | Connection registration, 4-tuple dispatch, mark_disconnected/reconnected |
| `test_flow_steering` | `tests/dpdk/test_flow_steering.cpp` | RxDispatchMode detection, RSS configuration, FlowRule RAII |
| `test_multicast` | `tests/dpdk/test_multicast.cpp` | Multicast MAC derivation, group join/leave, SSM filter, ParsedUdpPacket |
| `dpdk_test_env.hpp` | `tests/dpdk/dpdk_test_env.hpp` | Shared test harness/mock environment for DPDK tests (no real EAL needed) |
| `bench_tcp_header` | `benchmarks/dpdk/bench_tcp_header.cpp` | Packet construction/parsing throughput |
| `bench_pipeline` | `benchmarks/dpdk/bench_pipeline.cpp` | Full pipeline benchmark (RX parse -> TCP -> transport) |
| `bench_market_dpdk` | `benchmarks/latency/bench_market_dpdk.cpp` | End-to-end market data latency over DPDK |
| `bench_order_rtt_dpdk` | `benchmarks/latency/bench_order_rtt_dpdk.cpp` | Order round-trip time measurement over DPDK |
