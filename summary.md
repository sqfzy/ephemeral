# Project: ephemeral (eph)

> Ultra-low-latency C++23 header-only networking library with unified WebSocket/TLS transport abstraction spanning POSIX sockets and DPDK user-space networking, plus FIX/ITCH protocol engines and order book infrastructure for HFT systems.

**Language**: C++23 | **Build**: xmake | **Version**: 1.0.0

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

---

## Overview

ephemeral is a C++23 networking library designed for ultra-low-latency financial market data and order entry systems. The project is entirely header-only and organized into ten independent modules:

- **eph-core** -- shared concepts (`TcpTransport`, `MessageFramer`, `MetricsSink`), error types, length-prefix framer
- **eph-utils** -- TSC timing, CPU topology, hugepage allocator, HDR histogram, audit log (MiFID II / Reg NMS), EMA
- **eph-containers** -- SPSC bounded queue, evicting queue, ring buffer
- **eph-transport** -- Generic transport variants (threaded, direct-TX, direct), composition architecture (TransportCore + TxWorker + RxWorker + ReconnectPolicy + FrameProcessor), TLS 1.3, WebSocket, HTTP upgrade, preset aliases
- **eph-net** -- POSIX socket transport, HTTP/REST client, Gateway, KillSwitch, CircuitBreaker, rate limiter, proxy (SOCKS5/HTTP CONNECT)
- **eph-dpdk** -- DPDK EAL, user-space TCP, ARP/DNS, flow steering (RSS + rte_flow), Reactor (muxed RX), connector
- **eph-fix** -- FIX 4.4 parser/builder/session, orders, execution reports, position tracker, risk checks, order manager
- **eph-itch** -- ITCH 5.0 messages/parser, SoupBinTCP, MoldUDP64, OUCH 5.0
- **eph-json** -- zero-copy JSON parser/framer, Binance/OKX/Bybit adapters (WebSocket + REST)
- **eph-book** -- ArrayBook, MapBook (L2/L3), market signals, Binance/ITCH adapters

The core design achieves zero-overhead transport abstraction through C++20 concepts (`TcpTransport`, `MessageFramer`, `MetricsSink`). Three transport variants compose from shared building blocks: `Transport` (threaded TX+RX), `DirectTxTransport` (direct TX, threaded RX), and `DirectTransport` (no threads). All compose `TransportCore` + `ReconnectPolicy` + `FrameProcessor`, with workers added per variant. Each works identically over POSIX sockets and DPDK user-space TCP, with the framing layer (WebSocket, FIX length-prefix, raw passthrough) pluggable at compile time.

In the DPDK path, the entire data pipeline bypasses the kernel: Application -> SPSC queue -> TX thread (frame encode -> TLS AES-GCM encrypt -> TCP/IP header build) -> NIC PMD direct send. Measured end-to-end TX latency is ~164ns and RX ~139ns for 64-byte payloads on Intel Xeon with AES-NI.

TLS data-plane uses aws-lc `EVP_AEAD_CTX_seal/open` (single AEAD call), bypassing `SSL_write/SSL_read` multi-layer indirection, saving ~150ns/record. WebSocket masking uses a bulk-pregenerated CSPRNG key pool (1024-key cache), reducing per-key cost to ~2ns.

---

## Architecture

Layered pipeline architecture: modules compose bottom-up, decoupled through C++ concepts and template parameters.

### Component Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application                              │
│     (ws_echo_client / binance_book / simple_hft / user code)    │
└──────┬──────────────┬──────────────┬──────────────┬─────────────┘
       │              │              │              │
       ▼              ▼              ▼              ▼
 ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
 │ eph-fix  │  │ eph-itch │  │ eph-json │  │ eph-book │
 │ FIX 4.4  │  │ ITCH 5.0 │  │ JSON     │  │ L2/L3    │
 │ session  │  │ OUCH 5.0 │  │ parser   │  │ books    │
 │ orders   │  │ SoupBin  │  │ adapters │  │ signals  │
 └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘
      │              │              │              │
      └──────────────┴──────┬───────┴──────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────────┐
│                       eph-transport                             │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Transport / DirectTxTransport / DirectTransport         │   │
│  │  ┌────────────┐ ┌──────────┐ ┌──────────┐ ┌───────────┐ │   │
│  │  │TransportCore│ │TxWorker  │ │RxWorker  │ │Reconnect  │ │   │
│  │  │(shared conn │ │(TX thd + │ │(RX thd + │ │Policy     │ │   │
│  │  │ state, TLS) │ │ SPSC q)  │ │ SPSC q)  │ │(exp.backoff│ │   │
│  │  └────────────┘ └──────────┘ └──────────┘ └───────────┘ │   │
│  │  FrameProcessor (WS frag reassembly, control frames)     │   │
│  └──────────┬────────────────────────────┬──────────────────┘   │
│             │                            │                      │
│  ┌──────────▼──────────┐  ┌──────────────▼──────────────────┐   │
│  │ eph-net              │  │ eph-dpdk                        │   │
│  │ SocketTransport     │  │ TcpSession (user-space TCP)     │   │
│  │ (POSIX/poll)        │  │ Reactor (muxed RX dispatch)     │   │
│  │ HttpClient (REST)   │  │ FlowSteering (RSS/rte_flow)    │   │
│  │ Gateway / KillSwitch│  │ Platform / EAL                 │   │
│  │ Proxy (SOCKS5/HTTP) │  │                                │   │
│  └──────────┬──────────┘  └──────────────┬──────────────────┘   │
└─────────────┼────────────────────────────┼──────────────────────┘
              │                            │
       ┌──────▼──────┐             ┌───────▼──────┐
       │ Kernel TCP  │             │   NIC HW     │
       │   Stack     │             │ (PMD direct) │
       └─────────────┘             └──────────────┘
```

### Concept-based Interfaces

```
┌──────────────────────────────────────────────────┐
│       concept TcpTransport                       │
│  connect() | send() | poll_rx() | close()        │
│  reset()   | mss()  | state()                    │
└──────────┬───────────────────────┬───────────────┘
           │                       │
   SocketTransport           TcpSession (DPDK)

┌──────────────────────────────────────────────────┐
│       concept MessageFramer                      │
│  encode() | decode() | max_overhead()            │
└──────┬──────────┬──────────┬──────────┬──────────┘
       │          │          │          │
   WsFramer  RawFramer  LenPrefix  JsonFramer
                            Framer

┌──────────────────────────────────────────────────┐
│       concept MetricsSink                        │
│  push_counter() | push_gauge() | push_histogram()│
│  flush()                                         │
└──────────┬───────────────────────┬───────────────┘
           │                       │
       NullSink              ConsoleSink
    (zero-cost)           (spdlog-based)
```

### Shared Infrastructure

```
┌─────────────────────────────────────────────┐
│             eph-containers                  │
│  BoundedQueue<T,N>   EvictingQueue<T,N>     │
│  BoundedQueueBytes   EvictingQueueBytes     │
│  RingBuffer<T,N>                            │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│               eph-utils                     │
│  TSC | HdrHistogram | Recorder | CPU        │
│  HugePage | Alignment | AuditLog | EMA      │
│  ConsoleSink | SystemStats | Timestamp      │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│               eph-core                      │
│  TcpTransport concept  | MessageFramer      │
│  MetricsSink concept   | ErrorEnum trait     │
│  Transport errors      | LengthPrefixFramer │
│  NullSink              | json_escape         │
└─────────────────────────────────────────────┘
```

---

## Module Map

| Module / File | Responsibility | Key Types | Depends On |
|---|---|---|---|
| **eph-core** | Shared concepts, error types, framing interfaces | | |
| `core/tcp_concept.hpp` | TCP transport backend concept definition | `TcpState`, `TcpTransport` concept | stdlib |
| `core/framer_concept.hpp` | Pluggable message framing concept | `MessageFramer` concept, `DecodedFrame`, `FrameError` | `error_traits.hpp` |
| `core/metrics_concept.hpp` | Metrics sink concept (Prometheus-style counters/gauges/histograms) | `MetricsSink` concept, `MetricTag`, `NullSink` | stdlib |
| `core/transport_errors.hpp` | Typed transport error enums with JSON serialization | `SendError`, `ConnectionError`, `ConnectionErrorInfo` | `error_traits.hpp` |
| `core/error_traits.hpp` | `ErrorEnum` concept and generic `std::formatter` base | `ErrorEnum` concept, `ErrorEnumFormatter<E>` | stdlib |
| `core/length_prefix_framer.hpp` | 2-byte big-endian length-prefix framer (ITCH, binary protocols) | `LengthPrefixFramer` | `framer_concept.hpp` |
| **eph-utils** | Low-level utilities: timing, profiling, audit | | |
| `utils/time.hpp` | TSC hardware timer (rdtscp/cntvct_el0), nanosecond precision | `TSC` | stdlib, spdlog |
| `utils/cpu.hpp` | CPU topology detection, thread affinity, spin-pause hints | `CpuTopologyInfo` | stdlib, spdlog |
| `utils/alignment.hpp` | Cache line alignment constants and templates | `CACHE_LINE_SIZE`, `Align<T>` | stdlib |
| `utils/hugepage.hpp` | Huge page memory allocation (mmap with aligned_alloc fallback) | `HugePage` | stdlib, spdlog |
| `utils/hdr_histogram.hpp` | Gil Tene HDR histogram with TSC-based timing helpers | `HdrHistogram`, `measure_tsc()`, `ScopedTSC` | `time.hpp` |
| `utils/recorder.hpp` | Single-thread and concurrent performance recorders | `Recorder`, `ConcurrentRecorder` | `hdr_histogram.hpp` |
| `utils/record.hpp` | Aggregate include for histogram + recorder + stats | *(convenience header)* | `hdr_histogram.hpp`, `recorder.hpp`, `system_stats.hpp` |
| `utils/system_stats.hpp` | RAII system resource profiler (getrusage: CPU, page faults, ctx switches) | `SystemStats` | stdlib, spdlog |
| `utils/audit_log.hpp` | Structured audit trail for regulatory compliance (MiFID II / Reg NMS) | `AuditLog<N>`, `AuditEvent`, `AuditEntry` | `time.hpp`, spdlog |
| `utils/console_sink.hpp` | Logging-based MetricsSink for development/debugging | `ConsoleSink` | `metrics_concept.hpp`, spdlog |
| `utils/ema.hpp` | Exponential moving average for signal smoothing | `Ema` | spdlog |
| `utils/timestamp.hpp` | Timestamp conversion utilities (ms/ns/epoch/midnight) | `ms_to_ns()`, `ns_to_ms()`, `wall_clock_ns()` | stdlib |
| `version.hpp` | Compile-time version number (1.0.0) | `kVersion*` | stdlib |
| **eph-containers** | Lock-free SPSC queues and ring buffers | | |
| `containers/concepts.hpp` | `TrivialData` concept constraint | `TrivialData<T>` | stdlib |
| `containers/bounded_queue.hpp` | SPSC lock-free bounded queue (blocks on full) | `BoundedQueue<T, Capacity>` | `concepts.hpp`, `alignment.hpp`, `cpu.hpp` |
| `containers/bounded_queue_bytes.hpp` | Variable-length byte payload wrapper (with timestamp) | `BoundedQueueBytes<MaxDataSize, Capacity>` | `bounded_queue.hpp` |
| `containers/evicting_queue.hpp` | SPSC wait-free write queue (overwrites oldest via seqlock) | `EvictingQueue<T, Capacity>` | `concepts.hpp`, `alignment.hpp`, `cpu.hpp` |
| `containers/evicting_queue_bytes.hpp` | Variable-length byte payload wrapper (with eviction count) | `EvictingQueueBytes<MaxDataSize, Capacity>` | `evicting_queue.hpp` |
| `containers/ring_buffer.hpp` | Fixed-size circular buffer for tick history lookback | `RingBuffer<T, Capacity>` | stdlib |
| **eph-transport** | Transport variants, TLS, WebSocket, framing, composition components | | |
| `transport/transport.hpp` | Threaded transport (TX thread + RX thread + SPSC queues) | `Transport<TcpImpl, Framer, MaxPayload, QueueDepth>` | `transport_core.hpp`, `tx_worker.hpp`, `rx_worker.hpp`, `reconnect_policy.hpp` |
| `transport/direct_tx_transport.hpp` | Direct TX + threaded RX (no TX thread/queue, app calls send directly) | `DirectTxTransport<TcpImpl, Framer, ...>` | `transport_core.hpp`, `rx_worker.hpp`, `reconnect_policy.hpp` |
| `transport/direct_transport.hpp` | Fully direct transport (no threads, no queues, app owns polling) | `DirectTransport<TcpImpl, Framer, ...>` | `transport_core.hpp`, `frame_processor.hpp`, `reconnect_policy.hpp` |
| `transport/transport_core.hpp` | Shared connection state (TCP, TLS, config, lifecycle atomics) | `TransportCore<TcpImpl>` | `tls_session.hpp`, `http.hpp`, eph-core |
| `transport/tx_worker.hpp` | TX thread, TX SPSC queue, TX stats, ping/pong | `TxWorker<...>` | `transport_core.hpp`, eph-containers |
| `transport/rx_worker.hpp` | RX thread, RX SPSC queue, RX stats, frame processing | `RxWorker<...>` | `transport_core.hpp`, `frame_processor.hpp`, eph-containers |
| `transport/frame_processor.hpp` | Frame decode, WS fragmentation reassembly, control frame handling | `FrameProcessor<Framer, DeliverPolicy>` | `framer_concept.hpp`, spdlog |
| `transport/reconnect_policy.hpp` | Exponential backoff reconnect with jitter | `ReconnectPolicy` | stdlib |
| `transport/transport_types.hpp` | Transport config, stats, enums (re-exports core error types) | `TransportConfig`, `TransportEvent`, `TransportStats` | `tcp_concept.hpp`, `transport_errors.hpp` |
| `transport/presets.hpp` | Canonical payload/depth/framer preset aliases | `DefaultTransport<T>`, `DirectDefaultTransport<T>`, ... | `transport.hpp`, `direct_tx_transport.hpp`, `direct_transport.hpp` |
| `transport/tls_session.hpp` | TLS 1.3 handshake with custom BIO bridging any TCP backend | `TlsSession<TcpImpl>`, `TlsHotState`, `TlsKeyMaterial` | `tcp_concept.hpp`, aws-lc, spdlog |
| `transport/tls_record.hpp` | TLS record layer AES-GCM AEAD encrypt/decrypt | `seal_record()`, `open_record()` | `tls_session.hpp`, aws-lc |
| `transport/tls_encryptor.hpp` | TLS encryption component | `TlsEncryptor` | aws-lc |
| `transport/tls_decryptor.hpp` | TLS decryption component | `TlsDecryptor` | aws-lc |
| `transport/tls_constants.hpp` | TLS protocol constants | TLS constants | stdlib |
| `transport/websocket.hpp` | RFC 6455 WebSocket frame encode/decode, CSPRNG mask key pool | `DecodedFrame`, `MaskKeyCache` | aws-lc (RAND), spdlog |
| `transport/ws_framer.hpp` | WebSocket MessageFramer adapter (wraps ws encode/decode) | `WsFramer` | `websocket.hpp`, `framer_concept.hpp` |
| `transport/raw_framer.hpp` | Pass-through framer (no framing overhead) | `RawFramer` | `framer_concept.hpp` |
| `transport/http.hpp` | Minimal HTTP/1.1 (WebSocket Upgrade handshake only) | `UpgradeResponse` | aws-lc (EVP, RAND), spdlog |
| **eph-net** | POSIX socket transport, HTTP/REST client, Gateway, KillSwitch | | |
| `net/socket_transport.hpp` | POSIX socket TCP backend (non-blocking, poll I/O) | `SocketConfig`, `SocketTransport` | `tcp_concept.hpp`, spdlog, POSIX |
| `net/http_client.hpp` | Synchronous HTTP/1.1 client for REST API calls (TLS via aws-lc) | `HttpClient`, `HttpResponse` | aws-lc (SSL), spdlog, POSIX |
| `net/gateway.hpp` | Multi-connection lifecycle manager (start/stop/reconnect/health) | `Gateway`, `GatewayConnection`, `ConnHealth` | spdlog |
| `net/kill_switch.hpp` | Centralized emergency shutdown coordinator (signal-safe) | `KillSwitch`, `TransportHandle` | spdlog |
| `net/circuit_breaker.hpp` | Three-state circuit breaker for endpoint protection | `CircuitBreaker`, `CircuitState` | spdlog |
| `net/rate_limiter.hpp` | Token bucket rate limiter for exchange API throttling | `RateLimiter` | spdlog |
| `net/proxy.hpp` | SOCKS5 and HTTP CONNECT proxy support | `ProxyConfig`, `make_proxied_factory()` | `socket_transport.hpp`, spdlog |
| `net/hmac.hpp` | HMAC-SHA256 signing for authenticated exchange REST APIs | `hmac_sha256()`, `to_hex()`, `to_base64()` | aws-lc (HMAC), spdlog |
| **eph-dpdk** | DPDK user-space TCP, flow steering, multiplexed RX | | |
| `dpdk/eal.hpp` | DPDK EAL lifecycle management (process-level singleton) | `EalGuard`, `eal_init()` | DPDK, spdlog |
| `dpdk/platform.hpp` | NIC port/queue/mempool initialization | `Platform`, `PlatformConfig` | DPDK, spdlog |
| `dpdk/net_header.hpp` | Ethernet/IPv4/TCP header build and parse, constexpr checksum | `PacketTemplate`, `ParsedPacket`, `ConnectionTuple` | DPDK |
| `dpdk/tcp.hpp` | User-space minimal TCP state machine (3-way handshake, seq/ack, no retransmit) | `TcpSession`, `TcpConfig` | `net_header.hpp`, `tcp_concept.hpp`, DPDK |
| `dpdk/arp.hpp` | Stateless ARP resolution (blocking broadcast request/reply) | `ArpPacket`, `resolve()` | `net_header.hpp`, DPDK |
| `dpdk/dns.hpp` | User-space DNS A-record resolution over DPDK data plane | `DnsConfig`, `resolve()` | `net_header.hpp`, DPDK, aws-lc (RAND) |
| `dpdk/flow_steering.hpp` | NIC hardware RX dispatch: RSS config and rte_flow 5-tuple steering | `RxDispatchMode`, `detect_rx_dispatch_mode()`, `install_flow_rule()` | DPDK, `net_header.hpp` |
| `dpdk/reactor.hpp` | Epoll-style multiplexed RX for multiple connections (replaces SharedRxDispatcher) | `Reactor`, `ReactorEntry` | `tcp.hpp`, `net_header.hpp`, DPDK |
| `dpdk/connector.hpp` | One-stop connection helper (Platform -> ARP -> TCP -> Transport) | `ConnectorConfig`, `ConnectResult`, `connect()` | all dpdk/* modules |
| `dpdk/types.hpp` | DPDK Transport type aliases | `DpdkTransport`, `DpdkSmallTransport`, `DpdkLargeTransport` | `tcp.hpp`, `net/transport.hpp` |
| **eph-fix** | FIX 4.4 protocol: parsing, building, session, order management | | |
| `fix/tags.hpp` | FIX tag constants and field value enums | `tag::*`, `OrdType`, `Side`, `ExecType` | stdlib |
| `fix/parser.hpp` | Zero-copy FIX tag-value parser | `MessageView`, `FieldView`, `parse()` | `error_traits.hpp`, spdlog |
| `fix/builder.hpp` | FIX message builder with automatic checksum/body-length | `MessageBuilder` | `tags.hpp`, spdlog |
| `fix/framer.hpp` | FIX message framer (SOH-delimited, satisfies MessageFramer) | `FixFramer` | `framer_concept.hpp` |
| `fix/orders.hpp` | Typed builders for NewOrderSingle (D), Cancel (F), Replace (G) | `new_order_single()`, `cancel_order()`, `replace_order()` | `builder.hpp`, `tags.hpp` |
| `fix/execution_report.hpp` | Typed Execution Report (8) parser | `ExecutionReport`, `parse_execution_report()` | `parser.hpp` |
| `fix/session.hpp` | FIX 4.4 session layer (logon/logout, heartbeat, seqnum, gap fill) | `FixSession`, `SessionConfig` | `builder.hpp`, `parser.hpp`, spdlog |
| `fix/position.hpp` | Per-symbol position tracker (avg-cost PnL) | `Position`, `PositionTracker` | spdlog |
| `fix/risk_check.hpp` | Pre-trade risk checks (qty/notional/position/rate limits) | `RiskLimits`, `RiskChecker` | `position.hpp` |
| `fix/order_manager.hpp` | Order lifecycle manager (pending/new/partial/filled/canceled) | `OrderManager`, `OrderState` | `execution_report.hpp`, `position.hpp` |
| **eph-itch** | ITCH 5.0, SoupBinTCP, MoldUDP64, OUCH 5.0 protocols | | |
| `itch/messages.hpp` | ITCH 5.0 message type definitions (zero-copy views) | `MessageView`, `AddOrder`, `OrderExecuted`, ... | stdlib |
| `itch/parser.hpp` | Zero-copy ITCH message parser | `parse()`, `ParseError` | `messages.hpp`, spdlog |
| `itch/framer.hpp` | ITCH message framer (satisfies MessageFramer) | `ItchFramer` | `framer_concept.hpp` |
| `itch/soupbintcp.hpp` | SoupBinTCP framer (Nasdaq TCP transport for ITCH feeds) | `SoupBinTcpFramer`, `soupbin::*` | `framer_concept.hpp` |
| `itch/moldudp64.hpp` | MoldUDP64 packet parser (Nasdaq multicast transport) | `MoldUdp64Header`, `parse_header()`, `MessageIterator` | `messages.hpp` |
| `itch/ouch.hpp` | OUCH 5.0 order entry protocol (enter/replace/cancel/executed) | `EnterOrder`, `OrderAccepted`, `OrderExecuted` | stdlib |
| **eph-json** | Zero-copy JSON parser with exchange adapters | | |
| `json/parser.hpp` | Zero-copy JSON parser for flat key-value objects (O(n) single-pass) | `JsonView`, `parse()`, `ParseError` | `error_traits.hpp` |
| `json/framer.hpp` | JSON-over-WebSocket pass-through framer (satisfies MessageFramer) | `JsonFramer` | `framer_concept.hpp` |
| `json/adapters/binance.hpp` | Binance bookTicker/trade/depth WebSocket adapters | `BookTicker`, `Trade`, `DepthUpdate` | `parser.hpp` |
| `json/adapters/binance_depth_types.hpp` | Binance orderbook depth types | `DepthSnapshot`, `DepthLevel` | stdlib |
| `json/adapters/binance_rest.hpp` | Typed Binance REST client (depth snapshots, server time) | `BinanceRestClient` | `parser.hpp`, `net/http_client.hpp` |
| `json/adapters/bybit.hpp` | Bybit WebSocket message adapters | Bybit-specific types | `parser.hpp` |
| `json/adapters/okx.hpp` | OKX WebSocket message adapters | OKX-specific types | `parser.hpp` |
| **eph-book** | L2/L3 order books, exchange adapters, market signals | | |
| `book/array_book.hpp` | Fixed-size sorted-array L2 book (optimal for 5-20 levels) | `ArrayBook<MaxLevels>`, `PriceLevel` | spdlog |
| `book/map_book.hpp` | Dynamic-depth L3 book backed by std::map (1000+ levels) | `MapBook` | spdlog |
| `book/signals.hpp` | Market microstructure signals (imbalance, microprice, spread, VWAP, depth ratio) | `order_imbalance()`, `weighted_mid()`, `spread_bps()`, `vwap()` | `array_book.hpp` |
| `book/binance_adapter.hpp` | Binance bookTicker -> ArrayBook bridge | `BinanceBookAdapter<MaxLevels>` | `array_book.hpp`, `json/adapters/binance.hpp` |
| `book/itch_adapter.hpp` | ITCH order-level events -> aggregated L2 ArrayBook | `ItchBookBuilder<MaxLevels>` | `array_book.hpp`, `itch/messages.hpp` |

---

## Data Flow

### Send Path (TX)

Application calls `Transport::send()` which writes the message into the TX SPSC queue (`BoundedQueue`). The TX thread dequeues and executes: Framer encode (e.g., WebSocket frame with mask XOR) -> TLS 1.3 AES-GCM encrypt (`seal_record`) -> TCP segment send. On the socket path this goes through `send()` syscall into the kernel; on the DPDK path it builds Ethernet/IP/TCP headers (`PacketTemplate::build_packet`) and dispatches directly to the NIC via `rte_eth_tx_burst()`.

### Receive Path (RX)

The RX thread polls the TCP layer (socket: `poll()` + `recv()`; DPDK: `rte_eth_rx_burst()` + `process_rx()`, or Reactor for multiplexed connections). Received data flows through: TCP seq/ack processing -> TLS record reassembly and AEAD decrypt (`open_record`) -> Framer decode (e.g., WebSocket frame decode) -> write to RX SPSC queue. Application consumes via `Transport::recv(callback)` (non-blocking).

### Flow Diagram

```
  Application
      |
      | send(data, len)
      v
 +----------+
 | TX Queue |  BoundedQueue<TxMessage, QueueDepth>
 |  (SPSC)  |
 +----+-----+
      | TX Thread
      v
 +----------+
 | Framer   |  WsFramer / RawFramer / LengthPrefixFramer / FixFramer
 | Encode   |
 +----+-----+
      v
 +----------+
 |TLS Seal  |  tls_record::seal_record() (AES-256-GCM)
 +----+-----+
      v
 +----------+     +--------------+
 |TCP Send  |---->| Kernel / NIC |
 +----------+     +------+-------+
                         |
                         | (wire)
                         |
                  +------v-------+
                  | Kernel / NIC |
 +----------+     +------+-------+
 |TCP Poll  |<-----------+
 +----+-----+     RX Thread (or Reactor for DPDK mux)
      v
 +----------+
 |TLS Open  |  tls_record::open_record()
 +----+-----+
      v
 +----------+
 | Framer   |  WsFramer / RawFramer / LengthPrefixFramer / FixFramer
 | Decode   |
 +----+-----+
      v
 +----------+
 | RX Queue |  BoundedQueue<RxMessage, QueueDepth>
 |  (SPSC)  |
 +----+-----+
      | recv(callback)
      v
  Application
```

---

## Key Components

### Transport Variants (eph-transport)

Three transport variants share a composition architecture built from independent components:

| Variant | File | Threads | Use Case |
|---|---|---|---|
| `Transport` | `transport/transport.hpp` | TX thread + RX thread | General-purpose, app thread decoupled from I/O |
| `DirectTxTransport` | `transport/direct_tx_transport.hpp` | RX thread only | Low-latency TX (app sends directly), background RX |
| `DirectTransport` | `transport/direct_transport.hpp` | None | Single-threaded event loops (Reactor, DPDK poll-mode) |

**Composition**: All variants compose from `TransportCore` (shared connection state: TCP, TLS, config, lifecycle) + `ReconnectPolicy` (exponential backoff). `Transport` adds `TxWorker` + `RxWorker`; `DirectTxTransport` adds `RxWorker` only; `DirectTransport` adds `FrameProcessor` inline.

**Interface** (shared across variants):
```cpp
static std::expected<std::unique_ptr<Variant>, ConnectionErrorInfo>
    create(TcpFactory factory, const TransportConfig& config);
SendError send(const void* data, size_t len, uint8_t opcode);
bool recv(auto&& callback);  // callback(const uint8_t* data, uint16_t len, uint8_t msg_type)
void close_gracefully(uint16_t code, std::string_view reason, duration timeout);
```
**Notes**: `TcpImpl` can be `SocketTransport` (socket path) or `TcpSession` (DPDK path). `Framer` can be `WsFramer`, `RawFramer`, `LengthPrefixFramer`, `FixFramer`, `JsonFramer`, etc. All concept-constrained at compile time. Preset aliases (`DefaultTransport`, `DirectDefaultTransport`, etc.) in `presets.hpp` provide canonical configurations.

### `Gateway`

**File**: `eph-net/include/eph/net/gateway.hpp`
**Purpose**: Multi-connection lifecycle manager. Orchestrates multiple Transport instances across exchanges/venues with centralized start/stop/reconnect, health monitoring, per-connection tagging, and KillSwitch integration.
**Interface**:
```cpp
Gateway gw;
auto id = gw.add("binance-btc", std::move(transport));
gw.start_all();
gw.stop_all();
```

### `KillSwitch`

**File**: `eph-net/include/eph/net/kill_switch.hpp`
**Purpose**: Centralized emergency shutdown coordinator. Registers multiple Transport instances, installs signal handlers (SIGINT/SIGTERM), provides graceful and emergency shutdown paths. Fixed-size array (no heap) ensures signal-handler safety.
**Interface**:
```cpp
KillSwitch ks;
ks.register_transport(&tp1);
ks.install_signal_handlers();
while (!ks.is_shutdown_requested()) { ... }
ks.shutdown();  // graceful
ks.kill();      // emergency
```

### `AuditLog<Capacity>`

**File**: `eph-utils/include/eph/utils/audit_log.hpp`
**Purpose**: Structured audit trail for regulatory compliance (MiFID II / Reg NMS). Records order lifecycle events (submit, fill, cancel, reject) with nanosecond TSC timestamps into a fixed-size ring buffer. Zero allocation on hot path. Optional file flush for post-trade reporting.
**Interface**:
```cpp
AuditLog<8192> log;
log.record(AuditEvent::NewOrder, order_id, price, qty, Side::Buy, flags);
log.flush_to_file("/var/log/audit/session.bin");
```

### `MetricsSink` concept + `NullSink` / `ConsoleSink`

**Files**: `eph-core/include/eph/core/metrics_concept.hpp`, `eph-utils/include/eph/utils/console_sink.hpp`
**Purpose**: Pluggable metrics interface following Prometheus conventions (counter, gauge, histogram with dimensional tags). `NullSink` compiles to zero overhead; `ConsoleSink` logs to spdlog for development.
**Interface**:
```cpp
sink.push_counter("tx_packets", count, {{"transport", "dpdk"}});
sink.push_gauge("rx_queue_depth", 7.5, {{"symbol", "btcusdt"}});
sink.flush();
```

### `Reactor` (DPDK)

**File**: `eph-dpdk/include/eph/dpdk/reactor.hpp`
**Purpose**: Epoll-style multiplexed RX for multiple DPDK connections. A single RX thread polls the NIC, identifies packets via 4-tuple match, and invokes `TcpSession::process_rx` inline. Zero ring overhead (replaces SharedRxDispatcher). Designed for NICs without RSS/Flow Director.
**Interface**:
```cpp
Reactor reactor({.port_id = 0, .rx_queue_id = 0}, pool);
reactor.add_connection(&session1, on_data1);
reactor.start();
reactor.stop();
```

### Flow Steering (DPDK)

**File**: `eph-dpdk/include/eph/dpdk/flow_steering.hpp`
**Purpose**: NIC hardware RX dispatch detection and configuration. Probes NIC capabilities at runtime and selects the best strategy: Software (Reactor), RSS Partitioned, or Flow Director (rte_flow 5-tuple per-connection queue).
**Interface**:
```cpp
auto mode = detect_rx_dispatch_mode(port_id);
auto rule = install_flow_rule(port_id, queue_id, tuple);
```

### `BoundedQueue<T, Capacity>` / `EvictingQueue<T, Capacity>`

**Files**: `eph-containers/include/eph/containers/bounded_queue.hpp`, `evicting_queue.hpp`
**Purpose**: SPSC lock-free queues for TX/RX thread <-> application data transfer. `BoundedQueue` blocks on full (spin-wait). `EvictingQueue` overwrites oldest (wait-free write, seqlock optimistic read). Both are cache-line aligned with shadow indices to minimize cross-core atomics.

### `FixSession`

**File**: `eph-fix/include/eph/fix/session.hpp`
**Purpose**: Complete FIX 4.4 session layer. Logon/logout handshake, automatic heartbeat, TestRequest response, bidirectional MsgSeqNum tracking, gap detection with ResendRequest, SequenceReset/GapFill handling. Thread-safe via atomics.

### `ArrayBook<MaxLevels>` / `MapBook`

**Files**: `eph-book/include/eph/book/array_book.hpp`, `map_book.hpp`
**Purpose**: L2 order book (`ArrayBook`: fixed-size sorted arrays, optimal for 5-20 levels, cache-friendly) and L3 order book (`MapBook`: std::map-backed, scales to 1000+ levels). Both support crypto exchange-style explicit updates and order-level implicit updates.

---

## Entry Points & APIs

| Entrypoint | Type | Description |
|---|---|---|
| `examples/ws_echo_client.cpp` | Binary | WebSocket echo client (socket backend) |
| `examples/ws_echo_client_dpdk.cpp` | Binary | WebSocket echo client (DPDK backend) |
| `examples/minimal_ws_client.cpp` | Binary | Minimal WebSocket client example |
| `examples/production_client.cpp` | Binary | Production-style WebSocket client |
| `examples/binance_book.cpp` | Binary | Binance orderbook example (eph-net + eph-json + eph-book) |
| `examples/fix_trading_demo.cpp` | Binary | FIX protocol trading demo |
| `examples/itch_feed_demo.cpp` | Binary | ITCH feed processing demo |
| `examples/simple_hft.cpp` | Binary | Simple HFT example (socket) |
| `examples/simple_hft_dpdk.cpp` | Binary | Simple HFT example (DPDK) |
| `examples/framer_showcase.cpp` | Binary | MessageFramer showcase (WsFramer, RawFramer, etc.) |
| `examples/ws_via_proxy.cpp` | Binary | WebSocket via SOCKS5/HTTP proxy |
| `examples/dpdk_quickstart.cpp` | Binary | DPDK quickstart guide |
| `examples/spsc_queue_demo.cpp` | Binary | SPSC queue usage demo |
| `examples/perf_tuning_basics.cpp` | Binary | Performance tuning utilities demo |
| `Transport::create(factory, config)` | Library API | Create threaded WebSocket transport |
| `DirectTransport::create(factory, config)` | Library API | Create threadless transport (event-loop use) |
| `DirectTxTransport::create(factory, config)` | Library API | Create direct-TX transport (threaded RX only) |
| `eph::dpdk::connect<T>(cfg, tcfg)` | Library API | DPDK one-stop connection (Platform -> ARP -> TCP -> Transport) |
| `eph::dpdk::eal_init(argc, argv)` | Library API | DPDK EAL initialization (process-level singleton) |
| `Platform::create(config)` | Library API | DPDK NIC port initialization |
| `Gateway::add(tag, transport)` | Library API | Register transport with multi-connection manager |
| `KillSwitch::register_transport(tp)` | Library API | Register transport for emergency shutdown |
| `FixSession::logon()` / `logout()` | Library API | FIX session lifecycle |
| `eph::json::parse(data, len)` | Library API | Zero-copy JSON parse |
| `eph::net::HttpClient::get/post()` | Library API | Synchronous HTTPS REST client |

---

## Dependencies

### Internal (module graph)

```
eph-book ───► eph-json (binance_adapter only, user must link both)
         ───► eph-itch (itch_adapter only, user must link both)

eph-transport ───► eph-core
              ───► eph-utils
              ───► eph-containers

eph-dpdk ───► eph-core
         ───► eph-utils
         ───► eph-containers
         ───► eph-transport (for Transport type aliases / presets)

eph-net  ───► eph-core
         ───► eph-utils

eph-fix  ───► eph-core
eph-itch ───► eph-core
eph-json ───► eph-core
eph-book ───► eph-core

eph-containers ───► eph-utils
eph-utils      ───► eph-core
```

### External

| Package | Version | Purpose |
|---|---|---|
| `spdlog` | -- | Structured logging with compile-time level filtering (SPDLOG_ACTIVE_LEVEL) |
| `aws-lc` | -- | TLS 1.3 handshake (SSL), AES-GCM AEAD (EVP_AEAD), HMAC-SHA256, SHA-1 (EVP), CSPRNG (RAND) |
| `DPDK` | -- | NIC PMD, mbuf, EAL, ethdev, rte_flow (eph-dpdk only) |
| `numactl` | -- | NUMA support (optional) |
| `gtest` | -- | Unit test framework |
| `benchmark` | -- | Google Benchmark microbenchmarks |
| `tabulate` | -- | Terminal table formatting (benchmark output) |

---

## Testing

| Test Suite | Location | Coverage Focus |
|---|---|---|
| Core unit tests | `tests/core/` | MetricsSink concept satisfaction |
| Utils unit tests | `tests/utils/` | TSC, CPU topology, huge pages, alignment, HdrHistogram, Recorder, SystemStats, AuditLog, EMA, Timestamp, version |
| Containers unit tests | `tests/containers/` | BoundedQueue, EvictingQueue, and their Bytes variants |
| Net unit tests | `tests/net/` | TLS record, HTTP upgrade, WebSocket frames, TCP concept, socket transport, transport types, transport, Gateway, KillSwitch, circuit breaker, rate limiter, proxy, HMAC, HTTP client, framer concept |
| DPDK unit tests | `tests/dpdk/` | Net header build/parse, ARP, DNS, EAL, connector, platform, TCP, flow steering, Reactor |
| FIX unit tests | `tests/fix/` | FIX parser/builder, orders, execution report, session, position, risk check, order manager |
| ITCH unit tests | `tests/itch/` | ITCH parse, MoldUDP64, OUCH 5.0, SoupBinTCP |
| JSON unit tests | `tests/json/` | JSON parser, Binance/Bybit/OKX adapters, Binance REST client |
| Book unit tests | `tests/book/` | ArrayBook, MapBook, signals, Binance adapter, ITCH adapter |

Key test scenarios:
- BoundedQueue: full/empty boundaries, batch push/pop, timeout semantics, zero-copy produce/consume
- EvictingQueue: seqlock tear detection, overwrite counting, single-slot specialization
- TLS record: nonce construction, encrypt/decrypt round-trip, boundary payload sizes
- WebSocket: frame encode/decode round-trip, mask XOR, UTF-8 validation, control frame limits, close code validity
- FIX session: logon/logout handshake, heartbeat timing, sequence gap detection, ResendRequest
- ITCH: message parse round-trip, SoupBinTCP framing, MoldUDP64 batch iteration, OUCH order entry/execution
- JSON: zero-copy parse, exchange-specific field extraction, malformed input handling
- Book: level insert/remove/overwrite, BBO tracking, signal calculations, adapter integration
- Gateway: connection add/remove, health monitoring, start/stop lifecycle
- KillSwitch: transport registration, signal handler installation, graceful/emergency shutdown
- Reactor: connection add/remove, 4-tuple dispatch, concurrent start/stop
- Flow steering: dispatch mode detection, flow rule install/remove

### Benchmarks

| Benchmark Suite | Location | What It Measures |
|---|---|---|
| `bench_bq_pingpong` | `benchmarks/containers/` | SPSC cross-core round-trip latency |
| `bench_bq_throughput` | `benchmarks/containers/` | SPSC peak throughput |
| `bench_bq_pushpop` | `benchmarks/containers/` | Single-thread push/pop overhead |
| `bench_bq_batch` | `benchmarks/containers/` | Batch operation performance |
| `bench_eq_*` | `benchmarks/containers/` | EvictingQueue equivalent benchmarks |
| `bench_time` | `benchmarks/utils/` | TSC timing overhead |
| `bench_ws` | `benchmarks/net/` | WebSocket mask/encode/decode microbench |
| `bench_tls` | `benchmarks/net/` | TLS seal/open microbench |
| `bench_transport_pipeline` | `benchmarks/net/` | End-to-end pipeline latency (PlainWS / WSS / WSS Burst) |
| `bench_rx_pipeline` | `benchmarks/net/` | RX pipeline: decrypt -> decode -> filter (in-memory) |
| `bench_pipeline` | `benchmarks/dpdk/` | DPDK pipeline benchmark |
| `bench_tcp_header` | `benchmarks/dpdk/` | Net header build/checksum performance |
| `bench_fix_parse` | `benchmarks/fix/` | FIX message parse throughput |
| `bench_itch_parse` | `benchmarks/itch/` | ITCH message parse throughput |
| `bench_json_parse` | `benchmarks/json/` | JSON parse throughput |
| `bench_array_book` | `benchmarks/book/` | ArrayBook update/query performance |
| `bench_market` / `_dpdk` | `benchmarks/latency/` | Single-symbol market data latency (Socket / DPDK), DirectTransport-based |
| `bench_order_rtt` / `_dpdk` | `benchmarks/latency/` | Order round-trip time measurement (Socket / DPDK) |
| `bench_mock_server` | `benchmarks/latency/` | In-process mock WS server for latency benchmarks |
| `bench_impl.hpp` | `benchmarks/latency/` | Shared benchmark logic (DirectTransport, no threads/queues) |
| `mock/mock_ws_server.hpp` | `benchmarks/latency/mock/` | Mock WS server (market tick + order response modes) |
| `mock/mock_ws_handshake.hpp` | `benchmarks/latency/mock/` | WS upgrade handshake for mock server |
| `mock/mock_data_gen.hpp` | `benchmarks/latency/mock/` | Synthetic market data generator |

### Tools

| Tool | Location | Purpose |
|------|----------|--------|
| `mock_binance_server.py` | `tools/` | Mock Binance WebSocket server with configurable batch-size/rate/jitter |
| `bench_latency.sh` | `scripts/` | Latency benchmark runner (network namespace, PCI detection) |
| `dpdk-setup.sh` | `scripts/` | DPDK environment setup (hugepages, device binding) |
| `dpdk-teardown.sh` | `scripts/` | DPDK environment teardown |
