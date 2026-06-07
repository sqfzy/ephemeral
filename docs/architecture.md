# ephemeral architecture

High-level overview of the architecture, written for new contributors. For the
full frozen design spec (including the decision log and rejected alternatives), read
`.artifacts/design-eph-v3.3-architecture-20260410.md`.

## The elevator pitch

ephemeral is a header-only C++23 networking library for HFT. The architecture
collapsed the previous `Transport` / `DirectTransport` / `DirectTxTransport`
trio into a single Tokio-style API:

- **`TcpStream<Codec>` / `UdpSocket<Codec>`** — the only per-connection types.
- **`Poller`** — the only I/O driver. Drives heterogeneous streams on a single loop.
- **`StreamCodec` / `DatagramCodec`** — stateful, templated on `PacketView`, can
  inject auto-responses via an `OutputBuffer&`.

Two backends (`eph::net::kernel` and `eph::net::dpdk`) each implement all three
concepts. They never depend on each other, so kernel-only users pay nothing for DPDK
and DPDK users never get kernel fallback paths.

## Module dependency graph

Edges point from consumer to dependency (`A → B` means "A depends
on B"). `eph-core` is the leaf — every other module roots there
directly or transitively.

```
  eph-net-kernel       eph-net-dpdk            eph-codec
  ─────┬───────        ─────┬──────            ────┬────
       │ eph-net           │ eph-net,              │ eph-net,
       │ eph-core          │ eph-utils,            │ eph-itch,
       │                   │ eph-containers,       │ eph-core
       │                   │ eph-core              │
       ▼                   ▼                       ▼
                        eph-net
                       ──┬────
                         │ eph-utils, eph-core
                         ▼
       eph-containers ───► eph-utils ───► eph-core ◄─── eph-fix
                                                   ◄─── eph-itch
                                                   ◄─── eph-json
                                                   ◄─── eph-book
```

`eph-net-kernel` and `eph-net-dpdk` are siblings — they never depend
on each other; an application links whichever (or both) it needs.
`eph-codec` is **not** a dependency of `eph-net`: codecs are
template parameters, so each consumer separately picks the codec
modules it needs and links them alongside the chosen backend. The
parser modules (`eph-fix`, `eph-itch`, `eph-json`, `eph-book`) only
depend on `eph-core` (no networking, no `eph-utils`); they provide
the `Codec` types an application composes against the chosen
backend at link time.

## The three concept layer

### Stream / Datagram (eph-net)

`Stream<T>` and `Datagram<T>` are the user-facing connection types. Any type that
satisfies these concepts can be attached to a `Poller`.

```cpp
// eph-net/include/eph/net/concepts.hpp
template <class T>
concept Stream = Pollable<T> && requires(T& t, std::span<const uint8_t> data) {
    typename T::CodecType;
    typename T::OnMessage;
    { t.send(data) }         noexcept -> std::same_as<std::expected<std::size_t, core::ErrorInfo>>;
    { t.close_gracefully() } noexcept -> std::same_as<std::expected<void,        core::ErrorInfo>>;
    { t.is_attached() }      noexcept -> std::convertible_to<bool>;
    { t.state() }            noexcept -> std::same_as<TcpState>;
    { t.on_message }                  -> std::convertible_to<typename T::OnMessage>;
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
| `LengthPrefixCodec` | stream | 4-byte BE length prefix |
| `RawDatagramCodec` | datagram | one frame per packet |
| `Mold64Codec` | datagram | MoldUDP64 sequence + gap detection, emits N ITCH frames per packet |

### Poller (eph-net)

`Poller<T>` is the I/O driver. One Poller hosts any number of Pollables of different
concrete types simultaneously, type-erased via P2 function pointers (not virtual
dispatch).

```cpp
// eph-net/include/eph/net/concepts.hpp
template <class T>
concept Poller = requires(T& p) {
    { p.poll() } noexcept -> std::convertible_to<std::size_t>;
};

template <class T, class Obj>
concept PollerOf = Poller<T> && Pollable<Obj> && requires(T& p, Obj* obj) {
    { p.add(obj) }    noexcept -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.remove(obj) } noexcept -> std::same_as<std::expected<void, core::ErrorInfo>>;
};
```

`Poller<T>` is the bare driver concept (just `poll()`); `PollerOf<T,
Obj>` is the finer-grained refinement that proves a specific Pollable
type can be registered. Function-parameter sites that don't need to
introduce a Pollable type stay constrained on `Poller<T>` — the split
keeps the cheaper concept usable as a parameter constraint without
naming an `Obj`.

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

The user-facing names deliberately mirror Tokio / mio:

| ephemeral | Tokio / mio |
|---|---|
| `eph::net::kernel::KernelTcpStream` | `tokio::net::TcpStream` |
| `eph::net::kernel::KernelUdpSocket` | `tokio::net::UdpSocket` |
| `eph::net::kernel::KernelPoller`    | `mio::Poll` |
| `eph::net::dpdk::DpdkTcpStream`     | (Tokio has no DPDK backend) |
| `eph::net::Stream` concept          | `AsyncRead + AsyncWrite` |
| `eph::net::Poller` concept          | `mio::event::Source`'s host |

`TcpStream` and `UdpSocket` are the only per-connection types. There are no "channels"
or "variants" — the threading model collapsed to "whatever thread owns the
Poller runs the callbacks in that thread."

## Threat model: tenants are trusted

The DPDK backend's daemon-led multi-process model puts three POD layouts
in shared hugepage memory that every attached secondary can `mmap-write`:

| Hugepage layout | Carries | Cardinality |
|---|---|---|
| `MpRegistry` | per-tenant (queue range, src_port range, lcore mask, PID) | up to 64 slots |
| `QueueAllocator::Header` | bitmap + per-queue claim_gen + monotonic generation | single |
| `IcmpDirectory` | per-stream (5-tuple → owner_proc_idx, generation) | 1024 entries |

The project's threat model is **"tenants are trusted; defend against
accidents"** — wild-pointer writes, ABA on PID reuse, half-init slot
reads. Defences against accidents are layered into each layout:

- `magic` + `version` headers — wild writes that clip a header are
  caught at attach time.
- `claim_gen[]` per-queue + monotonic `generation` — distinguishes
  ABA (stale release after re-claim) from a legitimate release.
- atomic `claimed` state machine + 2-step publish in `IcmpDirectory` —
  half-init slots are never visible as "Published".
- bounds-checked indices and small registry sizes (1024 entries cap).

A T2.3 trust-boundary HMAC layer was added in 2026-05-05 (commits
b775310b..d44d7b22) and reverted on 2026-05-06 (see
`eph-net-dpdk/CHANGELOG.md`). It would only have caught wild-pointer
writes that the magic/version primitives miss; that case is rare,
already covered by ASan / hardened malloc in dev, and the toml plumbing
to actually enable HMAC was never wired through to `eph-nicd`. Net cost
~1000 LOC across 11 files for zero production-path coverage. If the
threat model ever shifts to "defend against malicious tenants" the
trust-boundary work can be reintroduced cleanly — the registry
layouts already reserve forward-compatible version numbers (v5
MpRegistry, v3 QueueAllocator, v3 IcmpDirectory).

## Where to go next

- **Want to write a new codec?** → `docs/custom-codec.md`
- **Want to use the Poller with multiple connections?** → `docs/poller-guide.md`
- **Setting up DPDK for the first time?** → `docs/dpdk-setup.md`
- **Running the latency benchmarks?** → `docs/latency-benchmark-fairness.md`
- **Deploying to prod?** → `docs/production-config.md`, `docs/operations-runbook.md`
- **Need the full history?** → `.artifacts/design-eph-v3.3-architecture-20260410.md`
