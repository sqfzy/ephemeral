# New contributor onboarding

Welcome. This document gets you from a fresh clone to a working mental model of the
ephemeral codebase in roughly an afternoon. Read in order.

## 1. Read these first (30 minutes)

1. `README.md` at the repo root — what ephemeral is, who it's for, quickstart.
2. `docs/architecture.md` — the v3.3 concept model (Stream / Codec / Poller) and the
   module graph. This is the single most important document.
3. `summary.md` — module-by-module overview with public API sketches and data flow.

If you want the full decision log (why v3.3 looks the way it does, what alternatives
were weighed and rejected), read
`.artifacts/design-eph-v3.3-architecture-20260410.md`. It's the frozen design spec
that drove the refactor.

## 2. Get a build running (15 minutes)

```bash
git clone https://github.com/sqfzy/ephemeral.git
cd ephemeral

xmake f -m debug
xmake build -g tests
xmake run -g tests          # expect all-pass
```

If the build fails on C++23 features, install GCC 14 (Amazon Linux 2023:
`sudo dnf install gcc14-g++`) and use the gcc14 wrapper at `/tmp/gcc14-wrap/g++`.

DPDK builds are optional. The kernel path builds cleanly on any Linux host with GCC
≥ 13 / Clang ≥ 17. See `docs/dpdk-setup.md` if you want to bring up the DPDK path.

## 3. The 11 modules in one screen

```
eph-utils       TSC timer, CPU pinning, HDR histogram, hugepage, audit log
eph-containers  SPSC queues, evicting queues, ring buffer
eph-core        Error / ErrorInfo / Codec concepts / OutputBuffer / PacketView contract
eph-codec       WsCodec, RawStreamCodec, LengthPrefixCodec, RawDatagramCodec, Mold64Codec
eph-net         Stream/Datagram/Pollable/Poller concepts, SocketAddr, ReconnectPolicy,
                test mocks (FakeStream/FakeDatagram/TestPoller), TLS/WS wire detail
eph-net-kernel  KernelTcpStream, KernelUdpSocket, KernelPoller (epoll)
eph-net-dpdk    DpdkTcpStream, DpdkUdpSocket, DpdkPoller, Eal (lcore burst)
eph-fix         FIX 4.4 parser/builder/session/orders/risk
eph-itch        ITCH 5.0 / SoupBinTCP / MoldUDP64 / OUCH
eph-json        JSON parser + Binance / OKX / Bybit adapters
eph-book        Array / map order books + signals
```

`eph-net-kernel` and `eph-net-dpdk` are sibling backends — pick whichever you need in
your target, never both on the same file. Parser modules (`eph-fix`, `eph-itch`,
`eph-json`, `eph-book`) never depend on any networking module.

## 4. The three concepts

Everything network-shaped in v3.3 pivots on three concepts:

- **`StreamCodec<T>` / `DatagramCodec<T>`** (`eph/core/codec.hpp`) — stateful
  decoders. `decode()` takes a `PacketView&` and an `OutputBuffer&` (for
  auto-responses like WS pong). `StreamCodec::decode` returns
  `expected<optional<Frame>>`. `DatagramCodec::decode` takes a sink and returns the
  number of frames emitted.

- **`Stream<T>` / `Datagram<T>`** (`eph/net/concepts.hpp`) — per-connection
  user-facing types. `send()` / `close_gracefully()` / `on_message` / `on_datagram`.
  Implementations: `KernelTcpStream<C,Tls>`, `KernelUdpSocket<C>`,
  `DpdkTcpStream<C,Tls>`, `DpdkUdpSocket<C>`, `FakeStream`, `FakeDatagram`.

- **`Poller<T>`** (`eph/net/concepts.hpp`) — the I/O driver. `add() / remove() /
  poll()`. One Poller drives many heterogeneous Pollables via P2 function-pointer
  type erase. Implementations: `KernelPoller`, `DpdkPoller<>`, `TestPoller<P>`.

Every user program follows the same shape:

```cpp
auto poller = Poller::create({}).value();
auto stream = TcpStream<Codec>::create(cfg).value();
stream->on_message = [](auto* d, auto n) { … };
poller->add(stream.get()).value();
while (running) poller->poll(100ms);
```

## 5. Where each thing lives

| Question | File |
|---|---|
| How do concepts get defined? | `eph-core/include/eph/core/codec.hpp`, `eph-net/include/eph/net/concepts.hpp` |
| How does a kernel TCP stream connect? | `eph-net-kernel/include/eph/net/kernel/tcp_stream.hpp` |
| How does a DPDK TCP stream connect? | `eph-net-dpdk/include/eph/net/dpdk/tcp_stream.hpp` |
| Where is the TLS handshake? | `eph-net/include/eph/net/detail/tls_session.hpp` |
| Where is WebSocket frame decoding? | `eph-codec/include/eph/codec/ws_codec.hpp` (codec) + `eph-net/include/eph/net/detail/websocket.hpp` (wire helpers) |
| Where is the in-place TLS decrypt for DPDK? | `eph-net-dpdk/include/eph/net/dpdk/detail/tls_state.hpp` |
| How are errors reported? | `eph-core/include/eph/core/error.hpp` (`Error` enum + `ErrorInfo`) |
| How do I write a new codec? | `docs/custom-codec.md` |
| How do I drive many connections? | `docs/multi-connection.md`, `docs/poller-guide.md` |
| How is the latency bench fair? | `docs/latency-benchmark-fairness.md` |

## 6. Build / test / bench commands

```bash
xmake -m release                 # release build
xmake -m debug                   # debug build (SPDLOG_LEVEL_TRACE)
xmake f -m asan && xmake         # ASan + UBSan
xmake f -m tsan && xmake         # TSan

xmake build -g tests             # build all tests
xmake run test_ws_codec          # run a single test (by file basename)
xmake run -g tests               # run every test in the group

xmake build -g benchmarks        # build all microbenchmarks
xmake run bench_ws_codec         # run one microbenchmark

xmake build -g examples          # build all examples

# End-to-end latency benchmarks (handles NIC-B state transitions)
sudo ./benchmarks/latency/lat tcp           # kernel client
sudo ./benchmarks/latency/lat tcp --dpdk    # DPDK client
```

Per-module tests live under `<module>/tests/test_*.cpp` — they are auto-globbed into
one target per file. Drop a new `.cpp` in and it builds automatically.

## 7. Conventions that will catch you off guard

- **Header-only.** No `.cpp` files under `<module>/include/`. New code lives in
  headers, `inline` or template.
- **No exceptions across module boundaries.** Use `std::expected<T, ErrorInfo>`.
  Tests build with `SPDLOG_NO_EXCEPTIONS` and the hot path has `-fno-exceptions`
  flags on the relevant targets.
- **No virtual dispatch.** Constrain templates with the v3.3 concepts; don't add
  abstract base classes.
- **TSC, not `steady_clock`**, for latency measurement. `eph::utils::TSC::now()` is
  the canonical timer. The bench framework assumes it.
- **Compile-time log filtering.** Use the `SPDLOG_TRACE / DEBUG / INFO / …` macros
  (not the runtime spdlog API), so suppressed levels compile out entirely.
- **Per-module README/CHANGELOG/summary/ONBOARDING** are regenerated on big refactors
  — don't hand-edit them in a way that will get blown away. They were regenerated
  wholesale for v3.3.
- **DPDK targets need `apply_dpdk_pmd_linkgroups()`** in their `xmake.lua` to
  whole-archive-link PMDs. See the `*_dpdk` targets in the root xmake.lua for the
  pattern.

## 8. What to read when you're ready for more depth

- `docs/architecture.md` — re-read once the concepts feel familiar
- `docs/poller-guide.md` — the single most useful "how do I use this?" doc
- `docs/custom-codec.md` — before writing a new `StreamCodec` / `DatagramCodec`
- `docs/dpdk-setup.md` — before bringing up DPDK for the first time
- `docs/multi-connection.md` — before scaling past one symbol
- `docs/latency-benchmark-fairness.md` — before quoting kernel-vs-DPDK numbers
- `docs/production-config.md`, `docs/operations-runbook.md`,
  `docs/troubleshooting.md` — before deploying anything

## 9. Where to ask for help

- File-level questions → the relevant header's doxygen comment
- Module-level questions → the module's `README.md` and `summary.md`
- Cross-module / architecture questions → `docs/architecture.md` and the design spec
  at `.artifacts/design-eph-v3.3-architecture-20260410.md`
- Performance questions → `docs/latency-benchmark-fairness.md`, the bench harness in
  `benchmarks/latency/core/`, and the per-module `benchmarks/` directories
