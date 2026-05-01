# Project: ephemeral (eph)

> Ultra-low-latency C++23 header-only networking library for HFT. Eleven modules,
> three narrow concepts (Stream / Datagram / Codec), one `Poller` driving
> heterogeneous kernel + DPDK connections, zero virtual dispatch.

**Language**: C++23 | **Build**: xmake

The authoritative design spec for the current architecture is
`.artifacts/design-eph-v3.3-architecture-20260410.md`. This document is the
project-level overview — for the full narrative (history, decisions, alternatives
weighed), read the design doc. For a narrower concept-level guide, see
`docs/architecture.md`.

---

## Table of Contents

1. [Overview](#overview)
2. [Module Map](#module-map)
3. [The Three Core Concepts](#the-three-core-concepts)
4. [Dependency Graph](#dependency-graph)
5. [Backend Implementations](#backend-implementations)
6. [Data Flow (RX and TX)](#data-flow-rx-and-tx)
7. [PacketView — the zero-copy contract](#packetview--the-zero-copy-contract)
8. [Threading Model](#threading-model)
9. [Error Handling](#error-handling)
10. [Tokio naming alignment](#tokio-naming-alignment)
11. [Build & Test Layout](#build--test-layout)

---

## Overview

ephemeral is a C++23 networking library for HFT order entry and market data. It is
entirely header-only and organized into eleven independent modules. The
architecture pivots the whole stack on three narrow concepts and a single I/O driver:

- `eph::core::StreamCodec<T>` / `DatagramCodec<T>` — stateful, templated over an
  associated `PacketView` type.
- `eph::net::Stream<T>` / `Datagram<T>` — per-connection user-facing type; combines a
  byte socket, a codec, optional TLS, and a `Poller` attachment.
- `eph::net::Poller<T>` — the single I/O driver. Heterogeneous `Pollable` objects live
  on one poller via P2 function-pointer type erase.

Every user program — kernel or DPDK, single-connection or multi-connection — follows
the same shape:

```cpp
auto poller = Poller::create({}).value();
auto stream = TcpStream<WsCodec>::create(cfg).value();
stream->on_message = [](std::span<const uint8_t> app_frame) { … };
poller->add(stream.get()).value();
while (running) poller->poll(100ms);
```

The kernel (`eph::net::kernel`) and DPDK (`eph::net::dpdk`) backends supply concrete
implementations of all three concepts. They never depend on each other — applications
link whichever is needed. The DPDK build weight (vfio-pci, hugepages, whole-archive
PMD linking) is confined entirely to `eph-net-dpdk`; kernel-only users and CI hosts
are immune.

### Observability

Both backends share a two-layer pull-model metrics path (details in
`docs/observability-guide.md`):

- **Layer 1 (hot path)**: each stream owns an `alignas(64) std::atomic<uint64_t>`
  array indexed by `eph::net::StreamMetric`. The hot path increments via a private
  template `inc_<M>()` that compiles to a single `lock add` on x86 — no virtual
  dispatch, no branch, no sink reference.
- **Layer 2 (reader)**: `eph::net::publish_metrics(stream, sink, tags)` iterates
  every counter and pushes it into any `eph::core::MetricsSink` (`NullSink`,
  `eph::utils::ConsoleSink`, or user-defined `PrometheusSink` / `OtelSink` / …).
  Applications choose the publish cadence (typical: 100 ms - 1 s).

Six built-in counters: `kBytesSent` / `kBytesRecv` / `kFramesDecoded` /
`kReasmOverflows` (TCP only) / `kCodecErrors` / `kTlsCrossRecordFrames` (DPDK
TLS only). Adding a new counter is one enum entry + one name-table entry + N
hot-path `inc_<M>()` calls; no template signature changes anywhere.

---

## Module Map

| Module | Role | Depends on |
|---|---|---|
| **eph-utils** | TSC timing, CPU pinning, hugepage allocator, HDR histogram, audit log, recorder, EMA | spdlog |
| **eph-containers** | SPSC bounded queue, evicting queue, ring buffer, byte-level variants | eph-utils |
| **eph-core** | `Error` / `ErrorInfo` / `StreamCodec` / `DatagramCodec` / `OutputBuffer` / `PacketView` contract + legacy framer primitives still used by parser modules | spdlog |
| **eph-codec** | `WsCodec`, `RawStreamCodec`, `LengthPrefixCodec`, `RawDatagramCodec`, `Mold64Codec` — all stateful, all satisfying `Codec` | eph-core, eph-net (TLS / WS wire helpers), eph-itch (Mold64 wraps `parse_moldudp64`), aws-lc, zlib |
| **eph-net** | `Stream` / `Datagram` / `Pollable` / `Poller` concepts, `SocketAddr`, `ReconnectPolicy`, `TcpState`, test mocks, shared TLS / WebSocket wire helpers | eph-core, eph-utils, aws-lc |
| **eph-net-kernel** | `KernelTcpStream<C,Tls>`, `KernelUdpSocket<C>`, `KernelPoller` (epoll). Contiguous `SpanView` `PacketView`. | eph-net |
| **eph-net-dpdk** | `DpdkTcpStream<C,Tls>`, `DpdkUdpSocket<C>`, `DpdkPoller<>`, `Eal`, internal DPDK primitives (arp, dns, flow_steering, packet templates). `MbufView` `PacketView` with in-place TLS decrypt. | eph-net, dpdk, aws-lc |
| **eph-fix** | FIX 4.4 parser/builder/session, orders, execution reports, position, risk checks, order manager | eph-core |
| **eph-itch** | ITCH 5.0 messages/parser, SoupBinTCP, MoldUDP64, OUCH | eph-core |
| **eph-json** | Zero-copy JSON parser/framer, Binance / OKX / Bybit adapters (REST + WS) | eph-core |
| **eph-book** | ArrayBook / MapBook (L2/L3), market signals, Binance / ITCH adapters | eph-core, eph-json, eph-itch |

`eph-net-kernel` and `eph-net-dpdk` are **sibling backends** that never depend on each
other. `eph-fix`, `eph-itch`, `eph-json`, `eph-book` never depend on any networking
module — they provide codecs that satisfy `eph::core::Codec` and are composed by the
application at link time.

---

## The Three Core Concepts

### `eph::core::StreamCodec<T>` / `eph::core::DatagramCodec<T>`

Stateful pluggable wire format. `decode()` is a non-const instance method so codecs
can carry reassembly state, control-frame FSMs, sequence counters, etc. Both variants
accept an `OutputBuffer&` so a codec can inject auto-responses (`WsCodec` writes pong
frames and close acks; `Mold64Codec` could inject retransmit requests).

```cpp
template <class T>
concept StreamCodec = requires(T& t, typename T::PacketViewRef view,
                               core::OutputBuffer& out,
                               uint8_t* buf, size_t cap) {
    typename T::Frame;
    typename T::PacketViewRef;
    { t.decode(view, out) } -> std::same_as<
        std::expected<std::optional<typename T::Frame>, core::ErrorInfo>>;
    { t.encode(buf, cap, std::declval<typename T::Frame>()) }
        -> std::same_as<std::expected<size_t, core::ErrorInfo>>;
    { T::max_overhead } -> std::convertible_to<size_t>;
    { T::is_streaming } -> std::convertible_to<bool>;
};
```

`DatagramCodec` is similar but `decode()` takes a sink callback and returns the number
of frames emitted — one UDP datagram may carry 0/1/N frames (MoldUDP64 wraps many ITCH
messages in a single packet).

Implementations (all in `eph-codec`):

| Codec | Kind | Notes |
|---|---|---|
| `WsCodec` | Stream | RFC 6455; owns reassembly + ping/pong/close FSM. |
| `RawStreamCodec` | Stream | Passes bytes through untouched. |
| `LengthPrefixCodec` | Stream | 4-byte big-endian length prefix, payloads up to `kMaxFrameLen` = 16 MiB. |
| `RawDatagramCodec` | Datagram | One frame per datagram, zero overhead. |
| `Mold64Codec` | Datagram | NASDAQ MoldUDP64 with gap detection; emits N ITCH frames per packet. |

### `eph::net::Stream<T>` / `eph::net::Datagram<T>`

The per-connection user-facing type. Concept requirements:

- `send()` / `close_gracefully()` / `is_attached()` / `state()` on `Stream`
- `send_to()` / `join_multicast()` / `leave_multicast()` / `is_attached()` on `Datagram`
- `on_message` / `on_datagram` callback field
- `using PacketView = …;` and `using CodecType = …;` associated types

Private `poll_once_()` / `notify_attached_()` / `notify_detached_()` methods are
exposed to friend Pollers but not to user code. Both concepts inherit from `Pollable`,
which is what `Poller::add()` accepts.

### `eph::net::Poller<T>`

The single I/O driver. One `Poller` can host many `TcpStream` and `UdpSocket` objects
of different concrete types simultaneously. Type erase is done at `add()` time via P2
function pointers (`poll_once_fn(void*)`), not virtual dispatch — there is no vtable.

- `KernelPoller` uses `epoll_wait()` and supports `poll(timeout)`.
- `DpdkPoller<>` is a lcore burst poll — `rte_eth_rx_burst` → flow-steering lookup →
  per-connection `process_burst_()` — and is non-blocking only.
- `TestPoller<P>` drives `poll_once_()` on all registered Pollables synchronously, no
  syscalls.

---

## Dependency Graph

```
                         eph-utils
                            |
                            v
                       eph-containers
                            |
               +------------+------------------+
               v            v                  v
            eph-core    (consumed by)     parsers:
          (Codec         eph-net        eph-fix, eph-itch,
           concept,                     eph-json, eph-book
           ErrorInfo)                   (use Codec concept)
               |
       +-------+-------+
       v               v
   eph-codec         eph-net
  (codec impls)   (concepts +
                   SocketAddr +
                   ReconnectPolicy +
                   FakeStream/Datagram +
                   TestPoller +
                   TLS/WS wire detail)
                      |
             +--------+--------+
             v                 v
       eph-net-kernel    eph-net-dpdk
       (epoll +          (lcore +
        KernelTcp/       DpdkTcp/Udp +
        Udp/Poller)      Poller + Eal +
                         flow steering +
                         internal primitives)
```

---

## Backend Implementations

### `eph::net::kernel` — epoll-based

```cpp
template <core::StreamCodec C, bool EnableTls = true>
class KernelTcpStream {
public:
    using CodecType  = C;
    using PacketView = detail::SpanView;   // contiguous span<const uint8_t>
    using OnMessage  = std::function<void(const uint8_t*, uint16_t)>;

    static std::expected<std::unique_ptr<KernelTcpStream>, core::ErrorInfo>
    create(StreamConfig cfg);              // TCP + TLS handshake + WS upgrade

    OnMessage on_message;
    std::expected<size_t, core::ErrorInfo> send(std::span<const uint8_t>);
    std::expected<void,   core::ErrorInfo> close_gracefully();
    bool     is_attached() const noexcept;
    TcpState state() const noexcept;
    int      fd() const noexcept;
};

class KernelPoller {
public:
    static std::expected<std::unique_ptr<KernelPoller>, core::ErrorInfo>
    create(PollerConfig cfg = {});

    template <Pollable P>
    std::expected<void, core::ErrorInfo> add(P* obj);
    template <Pollable P>
    std::expected<void, core::ErrorInfo> remove(P* obj);

    size_t poll() noexcept;                             // non-blocking
    size_t poll(std::chrono::milliseconds to) noexcept; // epoll_wait(timeout)
};
```

`KernelUdpSocket<C>` follows the same shape with `send_to` / `join_multicast` /
`leave_multicast`.

### `eph::net::dpdk` — lcore burst poll

```cpp
template <core::StreamCodec C, bool EnableTls = true>
class DpdkTcpStream {
public:
    using PacketView = detail::MbufView;   // mbuf-backed, in-place mutation
    // ... same public surface as KernelTcpStream
};

template <class P = void>  // void = type-erased heterogeneous mode
class DpdkPoller {
public:
    static std::expected<std::unique_ptr<DpdkPoller>, core::ErrorInfo>
    create(PollerConfig cfg);

    template <Pollable T> std::expected<void, core::ErrorInfo> add(T* obj);
    template <Pollable T> std::expected<void, core::ErrorInfo> remove(T* obj);
    size_t poll() noexcept;                // lcore burst
};
```

Internally `eph-net-dpdk` still builds on a rich set of low-level primitives (ARP
resolution, DNS, flow steering via RTE flow rules, pre-computed UDP/IP packet
templates, multicast helpers, the EAL RAII wrapper). They live under
`eph-net-dpdk/include/eph/dpdk/` as implementation detail — users only touch the
`eph::net::dpdk::*` public surface under `eph/net/dpdk/`.

---

## Data Flow (RX and TX)

### Kernel RX

```
epoll_wait -> fd readable -> KernelTcpStream::poll_once_()
        -> ByteSocket::read()  (into ReassemblyBuffer)
        -> [TLS decrypt]       (in-place if EnableTls, via aws-lc EVP_AEAD)
        -> Codec::decode(SpanView, OutputBuffer)
             - control frames -> written to OutputBuffer, codec returns nullopt
             - app frames     -> yielded via on_message callback
        -> (if OutputBuffer non-empty) ByteSocket::write()
```

### DPDK RX

```
rte_eth_rx_burst -> FlowSteeringTable::lookup(5-tuple) -> process_burst_()
        -> DpdkTcpSession::process_packet() -> rebuild RX byte stream in mbuf chain
        -> [TLS decrypt in-place into the same mbuf payload via aws-lc]
        -> Codec::decode(MbufView, OutputBuffer)
        -> on_message
```

### TX (kernel and DPDK)

```
user code -> Stream::send(span) -> Codec::encode(frame) -> [TLS encrypt]
        -> ByteSocket::send() / DpdkTcpSession::send_burst()
```

---

## PacketView — the zero-copy contract

Every `Stream` / `Datagram` implementation exposes an associated type
`using PacketView = …;` with this interface:

```cpp
uint8_t*       writable_data() noexcept;
const uint8_t* data() const noexcept;
size_t         length() const noexcept;
void           trim_front(size_t n) noexcept;   // skb_pull equivalent
void           trim_back(size_t n) noexcept;    // skb_trim equivalent
uint64_t       arrival_tsc() const noexcept;
```

`KernelTcpStream::PacketView` is `SpanView`, a contiguous `span<uint8_t>` view over
the reassembly buffer. `DpdkTcpStream::PacketView` is `MbufView`, backed by an
`rte_mbuf` with pool-managed headroom so TLS decrypt can run in place via aws-lc's
`EVP_AEAD_CTX_open_scatter`. The codec is templated on `PacketView` so the same
`WsCodec` instantiation works against both with no runtime branching.

The design goal: a DPDK RX path where the NIC DMAs the ciphertext into an mbuf, TLS
decrypts to plaintext in the same mbuf, and the codec yields a `span<const uint8_t>`
pointing into that same mbuf — zero memcpy between wire and user callback.

---

## Threading Model

The current architecture intentionally avoids the earlier `Transport` / `DirectTransport`
/ `DirectTxTransport` trio (which composed a matrix of RX / TX thread choices). The
model is:

- **One `Poller` drives I/O.** Whatever thread owns the poller runs the codec, TLS
  decrypt, and `on_message` callback in that same thread.
- **`send()` is called synchronously by the user.** There is no TX worker queue. On
  the kernel path, `send()` runs TLS encrypt and `write(2)` on the caller's stack. On
  the DPDK path, `send()` builds an mbuf burst and calls `rte_eth_tx_burst` directly.
- **For multi-threaded apps**, the user is responsible for handing data to the poller
  thread via a `BoundedQueue` / `EvictingQueue` from `eph-containers`. The library does
  not impose a threading model.

This collapses the architectural surface and matches the Tokio / mio convention (the
runtime owns the loop; user code calls `send()`/`recv()` from within a task).

---

## Error Handling

- `eph::core::Error` — single enum covering connection lifecycle, TLS, WebSocket,
  codec, I/O, and internal errors.
- `eph::core::ErrorInfo` — `{Error code; const char* detail}` with a static-storage
  `detail` string so it never dangles.
- All fallible APIs return `std::expected<T, ErrorInfo>`. No exceptions cross module
  boundaries (`SPDLOG_NO_EXCEPTIONS` is also set in tests).
- Parser modules (`eph-fix`, `eph-itch`, `eph-json`) keep their domain-specific enums
  (`FrameError`, `FixError`, etc.) because the error unification did not touch them.

---

## Tokio naming alignment

The public API deliberately mirrors Tokio:

| ephemeral | Tokio |
|---|---|
| `eph::net::kernel::KernelTcpStream` | `tokio::net::TcpStream` |
| `eph::net::kernel::KernelUdpSocket` | `tokio::net::UdpSocket` |
| `eph::net::kernel::KernelPoller` | `mio::Poll` |
| `eph::net::dpdk::DpdkTcpStream` | (no direct equivalent — Tokio has no DPDK backend) |
| `eph::net::Stream` concept | `tokio::io::AsyncRead + AsyncWrite` bound |
| `eph::net::Poller` concept | `mio::event::Source`'s host |

The concept names (`Stream`, `Datagram`, `Pollable`, `Poller`) read the same way in
both languages. `TcpStream` and `UdpSocket` are the only per-connection types in the
user-facing API; there are no "channels" or "variants" left.

---

## Build & Test Layout

- Per-module `xmake.lua` auto-globs `tests/**.cpp` into one test target per file
  using the `eph-test` rule, and `benchmarks/**.cpp` into benchmark targets using
  `eph-bench`. Drop a new `.cpp` into the directory and it builds automatically.
- Cross-module integration tests live in `tests/integration/`.
- DPDK e2e tests live in `eph-net-dpdk/tests/integration/test_dpdk_e2e.cpp`. They skip
  cleanly when NIC_B is not bound to vfio-pci.
- The legacy test suite (unit tests for internal primitives) lives in
  `eph-net-dpdk/tests/legacy/` to preserve coverage of ARP /
  DNS / flow steering / TCP state machine / net header helpers.
- End-to-end latency benches are in `benchmarks/latency/` with a canonical wrapper
  script (`benchmarks/latency/lat`). See `docs/latency-benchmark-fairness.md`.

Release builds exclude all tests / benchmarks / examples by default (`set_default(false)`
on every such target). Build them explicitly with `xmake build -g tests`,
`xmake build -g benchmarks`, `xmake build -g examples`, or individually by target
name.

---

For a detailed decision log (including alternatives weighed and rejected during
design), see `.artifacts/design-eph-v3.3-architecture-20260410.md`. For the
concept-level new-contributor overview, see `docs/architecture.md`.
