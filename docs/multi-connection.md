# Multi-Connection Patterns

How to coordinate multiple Transport instances for multi-symbol HFT feeds.

## Architecture Overview

```
                          ┌─────────────┐
  Stream 1 (AAPL) ───────│ Transport 1  │──┐
                          └─────────────┘  │
                          ┌─────────────┐  │   ┌──────────────┐
  Stream 2 (MSFT) ───────│ Transport 2  │──┼──→│  Application  │
                          └─────────────┘  │   │  (Aggregator) │
                          ┌─────────────┐  │   └──────────────┘
  Stream 3 (GOOG) ───────│ Transport 3  │──┘
                          └─────────────┘
```

Each symbol gets its own Transport instance. The application aggregates updates from all streams.

## Pattern 1: Push-Mode Aggregation (Recommended)

Use `on_message` callback to push updates directly from each RX thread into a shared aggregation queue.

```cpp
#include "eph/containers/bounded_queue.hpp"
#include "eph/net/socket_transport.hpp"

// Shared aggregation queue: all RX threads push here, app thread consumes
struct AggMsg {
    uint16_t stream_id;
    uint8_t  data[512];
    uint16_t len;
};
eph::containers::BoundedQueue<AggMsg, 4096> agg_queue;

// Create N transports with push-mode delivery into the shared queue
std::vector<std::unique_ptr<Transport<SocketTransport>>> transports;

for (uint16_t i = 0; i < num_symbols; ++i) {
    auto cfg = make_transport_config(symbols[i]);

    // Push-mode: RX thread delivers directly, no per-transport queue polling
    cfg.on_message = [i, &agg_queue](const uint8_t* data, uint16_t len, uint8_t) {
        agg_queue.try_produce([&](AggMsg& msg) {
            msg.stream_id = i;
            msg.len = std::min(len, uint16_t{512});
            std::memcpy(msg.data, data, msg.len);
        });
    };

    auto result = Transport<SocketTransport>::create(make_factory(symbols[i]), cfg);
    if (result) transports.push_back(std::move(*result));
}

// Application thread: consume from aggregation queue
while (running) {
    agg_queue.try_consume([](const AggMsg& msg) {
        process_update(msg.stream_id, msg.data, msg.len);
    });
}
```

**Pros**: Minimal latency (no intermediate queue hop), simple aggregation.
**Cons**: All RX threads contend on `agg_queue` (but BoundedQueue is SPSC — see Pattern 2 for MPSC).

## Pattern 2: Per-Stream Queue + Round-Robin Poll

Each Transport uses its own RX queue. Application polls them in round-robin.

```cpp
std::vector<std::unique_ptr<Transport<SocketTransport>>> transports;

// Create N transports (default queue-based delivery)
for (auto& sym : symbols) {
    auto cfg = make_transport_config(sym);
    // No on_message — use default queue delivery
    auto result = Transport<SocketTransport>::create(make_factory(sym), cfg);
    if (result) transports.push_back(std::move(*result));
}

// Application thread: round-robin poll
while (running) {
    for (size_t i = 0; i < transports.size(); ++i) {
        transports[i]->recv([&](const uint8_t* data, size_t len) {
            process_update(i, data, len);
        });
    }
}
```

**Pros**: No contention between RX threads. Simple mental model.
**Cons**: Round-robin introduces up to `N × recv_overhead` latency for the last stream polled.

## Pattern 3: EvictingQueue (Latest-Only Per Symbol)

For market data where only the latest price matters, use EvictingQueue to automatically discard stale updates.

```cpp
// Use EvictingQueue RX queue — latest update per symbol, no backpressure
using LatestTransport = eph::net::Transport<
    eph::net::SocketTransport,
    eph::net::WsFramer,
    512,   // MaxPayload
    1,     // QueueDepth = 1 (only latest)
    eph::containers::EvictingQueue,  // RxQueueTmpl
    true   // LastOnlyDeliver = true
>;

for (auto& sym : symbols) {
    auto cfg = make_transport_config(sym);
    auto result = LatestTransport::create(make_factory(sym), cfg);
    if (result) transports.push_back(std::move(*result));
}

// Poll: always gets the latest update, intermediate frames are dropped
for (auto& tp : transports) {
    tp->recv([](const uint8_t* data, size_t len) {
        // This is the LATEST data — no stale intermediate updates
    });
}
```

## Pattern 4: DPDK Reactor (Shared NIC)

When multiple sessions share a single NIC (common on AWS ENA), use Reactor
for zero-ring direct dispatch.

```cpp
#include "eph/dpdk/reactor.hpp"

// Create reactor for port 0, queue 0
eph::dpdk::Reactor reactor({
    .port_id = 0, .rx_queue_id = 0, .rx_cpu = 2,
});

// Register each connected session with a data callback
for (size_t i = 0; i < tcp_sessions.size(); ++i) {
    reactor.add_connection(&tcp_sessions[i],
        [i](const uint8_t* data, uint16_t len, size_t conn_id) {
            process_data(i, data, len);
        });
}

// Start reactor thread (polls NIC, dispatches directly to process_rx)
reactor.start();

// Reactor stamps burst TSC for accurate timing — no ring latency overhead
```

See `bench_market_persymbol_dpdk.cpp` for a complete implementation.

## CPU Pinning Strategy

For N symbols on a multi-core system:

```
Core 0:   OS / monitoring
Core 1:   Application thread (aggregation + processing)
Core 2:   Reactor RX thread (DPDK) or spare
Core 3:   Transport 1 TX
Core 4:   Transport 1 RX
Core 5:   Transport 2 TX
Core 6:   Transport 2 RX
...
```

With push-mode (Pattern 1), TX/RX threads are per-transport. For 50 symbols, this requires 100+ cores — impractical. Solutions:

1. **Shared TX/RX threads** (not yet built in — application-level): Multiplex multiple symbols onto fewer Transport instances using combined streams + frame filtering.
2. **Combined stream + filter**: Connect to one multi-symbol endpoint (e.g., Binance combined stream) and use `on_frame_filter` to select latest-per-symbol.
3. **Per-symbol isolation only for critical symbols**: Pin 2-3 critical symbols to dedicated cores, aggregate the rest.

## Backpressure Handling

When the application can't keep up:

| Queue Type | Behavior | Use Case |
|------------|----------|----------|
| `BoundedQueue` | Blocks RX thread (backpressure) | Order execution (no data loss) |
| `EvictingQueue` | Drops oldest, keeps latest | Market data (latest price is all that matters) |
| `on_message` push | Depends on callback implementation | Custom logic (selective drop, priority queue) |

Monitor `on_rx_drop` callback to detect when drops occur:
```cpp
cfg.on_rx_drop = [](size_t dropped) {
    metrics.increment("rx_drops", dropped);
};
```
