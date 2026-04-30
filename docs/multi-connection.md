# Multi-connection patterns

A single `Poller` hosts any number of streams — kernel or DPDK, TCP or UDP,
different codecs, different TLS settings. This document covers the three common
layouts for multi-symbol HFT feeds.

For a reminder of the `Poller` concept and basic usage, see `docs/poller-guide.md`.

## Layout 1 — single Poller, N TcpStreams, one thread

This is the default. All streams share a Poller on a single thread. The thread calls
`poller->poll(timeout)` in a loop and each stream's `on_message` fires synchronously
in that thread.

```cpp
#include "eph/net/kernel/poller.hpp"
#include "eph/net/kernel/tcp_stream.hpp"
#include "eph/codec/ws_codec.hpp"

namespace en = eph::net::kernel;
namespace ec = eph::codec;
using namespace std::chrono_literals;

auto poller = en::KernelPoller::create({}).value();

using WsStream = en::KernelTcpStream<ec::WsCodec, /*EnableTls=*/true>;
std::vector<std::unique_ptr<WsStream>> streams;

for (auto& symbol : symbols) {
    auto s = WsStream::create({
        .remote = en::SocketAddr{ /* resolved IPv4 */, 443 },
        .tls    = { .hostname = "fstream.binance.com" },
        .ws     = { .path = std::format("/ws/{}@bookTicker", symbol) },
    }).value();

    s->on_message = [sym = symbol](std::span<const uint8_t> app_frame) {
        route_tick(sym, app_frame.data(), app_frame.size());  // runs on the poller thread
    };

    poller->add(s.get()).value();
    streams.push_back(std::move(s));
}

while (running) {
    poller->poll(100ms);
}
```

**Pros**: Simple. Zero cross-thread synchronization. `route_tick` runs on the same
thread that did the TLS decrypt, so cache lines are still hot.

**Cons**: If `route_tick` is slow, it blocks every other stream on the same poller.

## Layout 2 — Poller thread + BoundedQueue → application thread

When `on_message` must hand off expensive work to a different thread (trading logic,
order management, persistence), use an `eph-containers` SPSC queue. The poller thread
becomes a pure I/O and codec thread; the consumer thread does the heavy lifting.

```cpp
#include "eph/containers/bounded_queue_bytes.hpp"

struct TickMsg {
    uint16_t symbol_id;
    uint16_t len;
    uint8_t  data[512];
};
eph::containers::BoundedQueue<TickMsg, 4096> tick_queue;

for (size_t i = 0; i < symbols.size(); ++i) {
    auto s = WsStream::create(make_cfg(symbols[i])).value();
    s->on_message = [i](const uint8_t* d, uint16_t n) {
        tick_queue.try_produce([&](TickMsg& m) {
            m.symbol_id = static_cast<uint16_t>(i);
            m.len       = std::min<uint16_t>(n, 512);
            std::memcpy(m.data, d, m.len);
        });
    };
    poller->add(s.get()).value();
    streams.push_back(std::move(s));
}

// Poller thread
std::thread io_thread([&] {
    pin_to_cpu(2);
    while (running) poller->poll(100ms);
});

// Application thread
pin_to_cpu(3);
while (running) {
    tick_queue.try_consume([](const TickMsg& m) {
        process_update(m.symbol_id, m.data, m.len);
    });
}
```

**Pros**: Isolates I/O cost from processing cost. Each thread can be pinned to its
own core. Backpressure is explicit (`try_produce` returns false when full).

**Cons**: One memcpy per tick into the queue. Use `EvictingQueue` instead for
latest-only semantics (below).

## Layout 3 — EvictingQueue for latest-only market data

For top-of-book data where only the latest price matters, use `EvictingQueue` —
producing overwrites the oldest unconsumed entry, so the consumer always sees the
freshest tick.

```cpp
#include "eph/containers/evicting_queue.hpp"

eph::containers::EvictingQueue<TickMsg, 16> latest;
// ... same producer shape as Layout 2 ...

while (running) {
    poller->poll(100ms);
    latest.try_consume([](const TickMsg& m) { process_latest(m); });
}
```

`EvictingQueue` never drops the producer — it drops the oldest waiting entry
instead. No backpressure, no queue-full checks. Use when "stale data is worse than
dropped data."

## Heterogeneous: TcpStream + UdpSocket on one Poller

The Poller doesn't care whether the Pollable is a TCP stream or a UDP socket, or
whether the codec is `WsCodec`, `RawStreamCodec`, or `Mold64Codec`. Mix them freely:

```cpp
#include "eph/net/dpdk/poller.hpp"
#include "eph/net/dpdk/tcp_stream.hpp"
#include "eph/net/dpdk/udp_socket.hpp"
#include "eph/codec/ws_codec.hpp"
#include "eph/codec/mold64_codec.hpp"

auto poller = eph::net::dpdk::DpdkPoller<>::create({
    .port_id = 0, .queue_id = 0, .lcore = 4,
}).value();

// Order channel: TLS WebSocket over DPDK TCP
auto orders = eph::net::dpdk::DpdkTcpStream<eph::codec::WsCodec>::create(order_cfg).value();
orders->on_message = handle_exec_report;
poller->add(orders.get()).value();

// Market data: MoldUDP64 over DPDK UDP multicast
auto md = eph::net::dpdk::DpdkUdpSocket<eph::codec::Mold64Codec>::create(md_cfg).value();
md->on_datagram = handle_itch_message;
md->join_multicast(mcast_group).value();
poller->add(md.get()).value();

while (running) poller->poll();
```

Type erase at `add()` time uses P2 function-pointer tables — no virtual dispatch,
no vtable. The DPDK burst poll walks `rte_eth_rx_burst` output through flow
steering, picks the right Pollable's `process_burst_fn` per packet, and calls it
directly.

## CPU pinning strategy

| Role | Where | Typical core |
|---|---|---|
| OS / housekeeping | any | 0 |
| Poller thread(s) | `std::thread` or `DpdkPoller::create({.lcore=…})` | isolated cores (e.g. 2–3) |
| Application logic | `std::thread` consuming from queue | isolated cores (e.g. 4–5) |

Use `isolcpus=2-5` at boot and `eph::utils::cpu::pin_to_cpu()` at runtime. See
`examples/perf_tuning_basics.cpp` for a worked example.

## Backpressure

| Pattern | Behavior | Use case |
|---|---|---|
| `BoundedQueue::try_produce` | Returns false when full — the poller thread drops | Critical data where the consumer is expected to keep up |
| `EvictingQueue::try_produce` | Overwrites oldest — never drops producer | Latest-tick market data |
| Direct `on_message` work | Blocks the poller — backpressure propagates to TCP via the kernel/DPDK send window | Simple apps with tight latency budgets and fast handlers |

Instrument drop counters with an `eph::utils::MetricsSink` (e.g. `ConsoleSink` in
development) so you notice when a consumer starts falling behind.

## See also

- `docs/poller-guide.md` — single-connection and basic multi-connection patterns
- `docs/architecture.md` — why the Poller is heterogeneous by design
- `examples/perf_tuning_basics.cpp` — TSC calibration + CPU pinning
