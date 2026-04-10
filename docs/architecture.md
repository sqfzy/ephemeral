# ephemeral v3.3 architecture

High-level overview of the v3.3 architecture, written for new contributors. For the
full frozen design spec (including the decision log and rejected alternatives), read
`.artifacts/design-eph-v3.3-architecture-20260410.md`.

## The elevator pitch

ephemeral is a header-only C++23 networking library for HFT. v3.3 was a ground-up
refactor that collapsed the previous `Transport` / `DirectTransport` / `DirectTxTransport`
trio into a single Tokio-style API:

- **`TcpStream<Codec>` / `UdpSocket<Codec>`** — the only per-connection types.
- **`Poller`** — the only I/O driver. Drives heterogeneous streams on a single loop.
- **`StreamCodec` / `DatagramCodec`** — stateful, templated on `PacketView`, can
  inject auto-responses via an `OutputBuffer&`.

Two backends (`eph::net::kernel` and `eph::net::dpdk`) each implement all three
concepts. They never depend on each other, so kernel-only users pay nothing for DPDK
and DPDK users never get kernel fallback paths.

## Module dependency graph

```
                        eph-utils
                           |
                           v
                      eph-containers
                           |
              +------------+-------------------+
              v            v                   v
           eph-core   (consumed by)       parsers:
         (StreamCodec  eph-net          eph-fix, eph-itch,
          /Datagram-                    eph-json, eph-book
          Codec concept,                (plug in as codecs)
          ErrorInfo,
          OutputBuffer)
              |
      +-------+-------+
      v               v
  eph-codec        eph-net
 (WsCodec,       (Stream /
  Mold64Codec,    Datagram /
  Raw*,           Pollable /
  LengthPrefix)   Poller concepts,
                  SocketAddr,
                  ReconnectPolicy,
                  test mocks,
                  TLS + WS wire detail)
                     |
            +--------+--------+
            v                 v
      eph-net-kernel    eph-net-dpdk
      (epoll-based      (lcore burst +
       Kernel*          Dpdk* + Eal +
       types)           flow steering)
```

`eph-net-kernel` and `eph-net-dpdk` are siblings — applications link whichever they
need. The parser modules (`eph-fix`, `eph-itch`, `eph-json`, `eph-book`) never depend
on any networking module; they provide codecs the application composes at link time.

## The three concept layer

### Stream / Datagram (eph-net)

`Stream<T>` and `Datagram<T>` are the user-facing connection types. Any type that
satisfies these concepts can be attached to a `Poller`.

```cpp
template <class T>
concept Stream = Pollable<T> && requires(T& t, std::span<const uint8_t> data) {
    typename T::CodecType;
    typename T::OnMessage;
    { t.send(data) }         -> std::same_as<std::expected<size_t, core::ErrorInfo>>;
    { t.close_gracefully() } -> std::same_as<std::expected<void,   core::ErrorInfo>>;
    { t.is_attached() }      -> std::same_as<bool>;
    { t.state() }            -> std::same_as<TcpState>;
    { t.on_message }         -> std::convertible_to<typename T::OnMessage>;
};
```

Implementations:

- `eph::net::kernel::KernelTcpStream<C, EnableTls>`
- `eph::net::kernel::KernelUdpSocket<C>`
- `eph::net::dpdk::DpdkTcpStream<C, EnableTls>`
- `eph::net::dpdk::DpdkUdpSocket<C>`
- `eph::net::test::FakeStream` / `FakeDatagram` (in-memory, no syscalls)

### Codec (eph-core)

`StreamCodec<T>` and `DatagramCodec<T>` are stateful decoders. They own reassembly
buffers, control-frame FSMs, sequence counters. `decode()` accepts a mutable
`PacketView` plus an `OutputBuffer&` so the codec can inject auto-responses (WS pong,
WS close ack) without the user writing any control-plane code.

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

Implementations live in `eph-codec`:

| Codec | Kind | What it does |
|---|---|---|
| `WsCodec` | stream | RFC 6455; auto-responds ping/close, reassembles fragments |
| `RawStreamCodec` | stream | passthrough |
| `LengthPrefixCodec` | stream | 2-byte BE length prefix |
| `RawDatagramCodec` | datagram | one frame per packet |
| `Mold64Codec` | datagram | MoldUDP64 sequence + gap detection, emits N ITCH frames per packet |

### Poller (eph-net)

`Poller<T>` is the I/O driver. One Poller hosts any number of Pollables of different
concrete types simultaneously, type-erased via P2 function pointers (not virtual
dispatch).

```cpp
template <class T>
concept Poller = requires(T& p, Pollable auto* obj) {
    { p.add(obj) }    -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.remove(obj) } -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.poll() }      -> std::convertible_to<size_t>;
};
```

Implementations:

- `KernelPoller` — epoll-based, supports `poll()` and `poll(timeout)`
- `DpdkPoller<>` — lcore burst poll, non-blocking only
- `TestPoller<P>` — drives registered pollables synchronously, no syscalls

See `docs/poller-guide.md` for usage patterns.

## PacketView — the zero-copy contract

Every `Stream` / `Datagram` implementation exposes a `using PacketView = …;` associated
type. Codecs are templated on `PacketView` so the same codec instantiation works
against both kernel and DPDK backends with no runtime branching.

```cpp
uint8_t*       writable_data() noexcept;   // mutable pointer for in-place decrypt
const uint8_t* data()          const noexcept;
size_t         length()        const noexcept;
void           trim_front(size_t n) noexcept;  // skb_pull equivalent
void           trim_back(size_t n)  noexcept;  // skb_trim equivalent
uint64_t       arrival_tsc()   const noexcept;
```

| Backend | PacketView impl | Notes |
|---|---|---|
| `eph::net::kernel` | `SpanView` | contiguous `span<uint8_t>` view into reassembly buffer |
| `eph::net::dpdk`   | `MbufView` | backed by an `rte_mbuf` with pool-managed headroom |

The DPDK path is the payoff: the NIC DMAs the ciphertext into an mbuf,
`aws-lc::EVP_AEAD_CTX_open_scatter` decrypts to plaintext in the same mbuf, and the
codec yields a `span<const uint8_t>` pointing into that same mbuf. There is no memcpy
between wire and user callback on the hot path.

## Tokio naming alignment

The v3.3 user-facing names deliberately mirror Tokio / mio:

| ephemeral | Tokio / mio |
|---|---|
| `eph::net::kernel::KernelTcpStream` | `tokio::net::TcpStream` |
| `eph::net::kernel::KernelUdpSocket` | `tokio::net::UdpSocket` |
| `eph::net::kernel::KernelPoller`    | `mio::Poll` |
| `eph::net::dpdk::DpdkTcpStream`     | (Tokio has no DPDK backend) |
| `eph::net::Stream` concept          | `AsyncRead + AsyncWrite` |
| `eph::net::Poller` concept          | `mio::event::Source`'s host |

`TcpStream` and `UdpSocket` are the only per-connection types. There are no "channels"
or "variants" in v3.3 — the threading model collapsed to "whatever thread owns the
Poller runs the callbacks in that thread."

## Preset aliases

The design doc reserves four short aliases for the most common template combinations,
so application code can avoid repeating the full template parameter list:

```cpp
using KernelWsClient = eph::net::kernel::KernelTcpStream<eph::codec::WsCodec, true>;
using DpdkWsClient   = eph::net::dpdk::DpdkTcpStream<eph::codec::WsCodec, true>;
using KernelWsPoller = eph::net::kernel::KernelPoller;
using DpdkWsPoller   = eph::net::dpdk::DpdkPoller<>;
```

These live alongside the concrete types when added and are purely syntactic sugar.

## Where to go next

- **Want to write a new codec?** → `docs/custom-codec.md`
- **Want to use the Poller with multiple connections?** → `docs/poller-guide.md`
- **Setting up DPDK for the first time?** → `docs/dpdk-setup.md`
- **Running the latency benchmarks?** → `docs/latency-benchmark-fairness.md`
- **Deploying to prod?** → `docs/production-config.md`, `docs/operations-runbook.md`
- **Need the full history?** → `.artifacts/design-eph-v3.3-architecture-20260410.md`
