# Poller guide

The `eph::net::Poller` concept is the single I/O driver in the architecture.
Every network program — kernel or DPDK, single-connection or multi-connection —
attaches its `TcpStream` / `UdpSocket` objects to a `Poller` and drives them from a
loop. This document walks through the common patterns.

For the underlying concept definition, see `docs/architecture.md`. For the legacy
`RxDispatcher`-centric guide from earlier architectures: that's gone — the `Poller`
supersedes it.

## Contents

1. [The concept](#the-concept)
2. [Single connection, kernel](#single-connection-kernel)
3. [Single connection, DPDK](#single-connection-dpdk)
4. [Multi-connection (many streams, one poller)](#multi-connection-many-streams-one-poller)
5. [Heterogeneous (TcpStream + UdpSocket on the same poller)](#heterogeneous)
6. [Embedding inside an existing main loop](#embedding-inside-an-existing-main-loop)
7. [Unit testing with TestPoller](#unit-testing-with-testpoller)
8. [Choosing between blocking and non-blocking poll](#choosing-between-blocking-and-non-blocking-poll)
9. [Lifecycle and ownership](#lifecycle-and-ownership)

## The concept

```cpp
template <class T>
concept Poller = requires(T& p, Pollable auto* obj) {
    { p.add(obj) }    -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.remove(obj) } -> std::same_as<std::expected<void, core::ErrorInfo>>;
    { p.poll() }      -> std::convertible_to<size_t>;
};
```

`poll()` returns the number of packets / frames processed in that iteration. A
Pollable is anything that has a private `poll_once_()` method the Poller can invoke
via a friend relationship. In practice the Pollables users deal with are `TcpStream<C>`
and `UdpSocket<C>`.

Concrete implementations:

| Type | Header | Backend |
|---|---|---|
| `eph::net::kernel::KernelPoller` | `eph/net/kernel/poller.hpp` | `epoll_wait`; supports `poll()` and `poll(timeout)` |
| `eph::net::dpdk::DpdkPoller<>`   | `eph/net/dpdk/poller.hpp`   | `rte_eth_rx_burst` on a pinned lcore; non-blocking only |
| `eph::net::test::TestPoller<P>`  | `eph/net/test/test_poller.hpp` | synchronous drive, no syscalls |

## Single connection, kernel

The minimum viable program:

```cpp
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/codec/ws_codec.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

int main() {
    auto poller = en::KernelPoller::create({}).value();

    auto stream = en::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>::create({
        .remote = en::SocketAddr{ /* resolved IPv4 */, 443 },
        .tls    = { .hostname = "echo.websocket.org" },
        .ws     = { .path = "/" },
    }).value();

    stream->on_message = [](std::span<const uint8_t> app_frame) {
        spdlog::info("rx: {}", std::string_view{
            reinterpret_cast<const char*>(app_frame.data()),
            app_frame.size()});
    };

    poller->add(stream.get()).value();

    // Send a ping every second, drive the loop at 100ms granularity
    auto next_ping = std::chrono::steady_clock::now();
    while (running) {
        poller->poll(100ms);
        if (std::chrono::steady_clock::now() >= next_ping) {
            const char msg[] = "hello";
            stream->send(std::as_bytes(std::span{msg}));
            next_ping += 1s;
        }
    }

    stream->close_gracefully();
}
```

Notes:

- `create()` performs the TCP connect, TLS handshake, and WebSocket upgrade
  synchronously before returning.
- The `on_message` callback is invoked on the same thread that called `poller->poll()`.
- `send()` runs the TLS encrypt + `write(2)` synchronously on the caller's stack. It
  is safe to call from inside `on_message` or from the same thread between polls.

## Single connection, DPDK

Same shape, different namespaces. One additional step: RAII-initialize the EAL:

```cpp
#include "eph/net/dpdk/eal.hpp"
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/codec/ws_codec.hpp"

namespace en = eph::net::dpdk;
namespace ec = eph::codec;

int main(int argc, char** argv) {
    // RAII EAL init. EalGuard::init takes (argc, argv); EalConfig +
    // build_eal_argv is the typed alternative when you want lcore pins
    // and named --proc-type. See eph-net-dpdk/docs/lcore-pin-integration.md.
    auto eal = ::eph::dpdk::EalGuard::init(argc, argv).value();

    auto poller = en::DpdkPoller<>::create({
        .port_id = 0, .rx_queue_id = 0, .max_connections = 16,
    }).value();

    // DpdkTcpStream's create_and_attach is the production factory — it
    // takes a much fuller StreamConfig (dpdk.tcp_low_level 4-tuple, MAC,
    // mempool, queue selection) plus a Platform reference. See
    // examples/binance_latency.cpp and examples/simple_hft_dpdk_rss.cpp
    // for runnable end-to-end DPDK setups.
    auto stream = /* see binance_latency.cpp for the full DpdkTcpStream
                     setup — irreducibly more involved than the kernel
                     snippet above */;

    stream->on_message = handle_message;
    poller->add(stream.get()).value();

    while (running) {
        poller->poll();                      // non-blocking lcore burst
    }
}
```

On the DPDK path there is no `poll(timeout)` — lcores are expected to busy-poll at
full core utilization. If you need to yield for housekeeping, interleave your own
`sched_yield()` or `rte_pause()` calls between `poll()` invocations.

## Multi-connection (many streams, one poller)

A single Poller handles any number of streams. Adding / removing is free of template
magic:

```cpp
auto poller = en::KernelPoller::create({}).value();

std::vector<std::unique_ptr<en::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>>> streams;
for (auto& symbol : symbols) {
    auto s = en::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>::create({
        .remote = en::SocketAddr{ /* resolved IPv4 */, 443 },
        .tls    = { .hostname = "fstream.binance.com" },
        .ws     = { .path = std::format("/ws/{}@trade", symbol) },
    }).value();
    s->on_message = [&, sym = symbol](std::span<const uint8_t> app_frame) {
        route_trade(sym, app_frame.data(), app_frame.size());
    };
    poller->add(s.get()).value();
    streams.push_back(std::move(s));
}

while (running) {
    poller->poll(100ms);
}
```

The Poller walks its registered entries in FIFO order each tick. With epoll, only the
fds the kernel reports as readable do any work. With DPDK, the burst poll checks all
registered Pollables every tick.

## Heterogeneous

One of the design goals is that a single Poller can drive a mix of `TcpStream`
and `UdpSocket` objects — the classic HFT layout where orders go over TCP and market
data over UDP multicast:

```cpp
#include "eph/codec/ws_codec.hpp"
#include "eph/codec/mold64_codec.hpp"

auto poller = en::DpdkPoller<>::create({...}).value();

// Order channel: TLS WebSocket over DPDK TCP
auto orders = en::DpdkTcpStream<ec::WsCodec>::create({...}).value();
orders->on_message = handle_exec_report;
poller->add(orders.get()).value();

// Market data: MoldUDP64 over DPDK UDP multicast
auto md = en::DpdkUdpSocket<ec::Mold64Codec>::create({
    .bind_addr = eph::net::SocketAddr{{0,0,0,0}, 30000},
}).value();
md->on_datagram = handle_itch_message;
md->join_multicast({{233,54,12,111}, 30001}).value();
poller->add(md.get()).value();

while (running) {
    poller->poll();
}
```

Type erase at `add()` time uses a P2 function-pointer table (one
`process_burst_fn(void*, rte_mbuf**, uint16_t, uint64_t)` per Pollable). There is no
virtual dispatch, no vtable, and no runtime branching inside the poll loop beyond
"which table entry matches this packet's 5-tuple?". Flow steering answers that lookup
in O(1).

## Embedding inside an existing main loop

If your application already owns a main loop (game engine, ROS node, trading
framework), drive the Poller non-blockingly:

```cpp
while (app.running()) {
    app.tick();
    poller->poll(0ms);   // returns immediately, processes whatever's ready
}
```

`poll(0ms)` on `KernelPoller` calls `epoll_wait(timeout=0)` which never blocks.
`DpdkPoller<>::poll()` is always non-blocking, so there's no overload needed.

## Unit testing with TestPoller

`eph::net::test::TestPoller<P>` drives a set of `FakeStream` / `FakeDatagram` objects
synchronously. No syscalls, no network, no thread scheduling variance — ideal for
testing codec + application logic together.

```cpp
#include "eph/net/test/fake_stream.hpp"
#include "eph/net/test/test_poller.hpp"

namespace ent = eph::net::test;

TEST(WsCodec, AutoRespondsToPing) {
    auto poller = ent::TestPoller<ent::FakeStream>::create();
    auto fake   = ent::FakeStream::create();
    poller->add(fake.get());

    fake->inject_rx({0x89, 0x00});     // WS ping, no payload
    poller->poll();                    // drives fake->poll_once_()

    auto tx = fake->collect_tx();
    ASSERT_EQ(tx.size(), 2);
    EXPECT_EQ(tx[0], 0x8A);             // pong opcode
    EXPECT_EQ(tx[1], 0x00);             // zero-length payload
}
```

`FakeStream::inject_rx()` stages bytes to be delivered on the next poll;
`collect_tx()` returns everything the stream has tried to send. Nothing escapes the
test binary.

## Choosing between blocking and non-blocking poll

| Scenario | Use |
|---|---|
| Standalone kernel daemon, single thread | `poller->poll(100ms)` or similar — yields the CPU when idle |
| Kernel daemon with background work between ticks | `poller->poll(0ms)` + your own sleep / timer |
| DPDK (always) | `poller->poll()` — lcore is assumed pinned and busy-polling |
| Embedded in an external main loop | `poller->poll(0ms)` — never block someone else's loop |
| Tests / synthetic drives | `TestPoller::poll()` — synchronous, deterministic |

## Lifecycle and ownership

- `Poller::add(obj)` records a raw pointer. The user owns the Stream/Datagram and
  must keep it alive until `remove()` is called or the Poller is destroyed.
- `Stream::~TcpStream()` (and `Datagram::~UdpSocket()`) automatically detach from
  whatever Poller they are attached to, so `unique_ptr<KernelTcpStream<…>>` is
  perfectly safe — dropping it first is fine.
- `Poller::~Poller()` tolerates leftover Pollables — it will detach them in an
  undefined order. Prefer explicit `remove()` when the order matters.

## See also

- `docs/architecture.md` — the three concept layer and module graph
- `docs/custom-codec.md` — writing a new `StreamCodec` / `DatagramCodec`
- `.artifacts/design-eph-v3.3-architecture-20260410.md` — frozen design spec
