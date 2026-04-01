# Reactor Multi-Connection Multiplexing Guide

How to use `eph::dpdk::Reactor` to multiplex multiple DPDK connections through a single RX thread with zero ring overhead.

## When to Use Reactor

Use Reactor when multiple TCP sessions share a single NIC RX queue -- common on NICs without RSS/Flow Director (e.g., AWS ENA). Without Reactor, each connection would need its own RX thread polling the same queue through an intermediate `rte_ring`, adding latency.

| Scenario | Recommended Pattern |
|----------|-------------------|
| 1 connection per NIC queue | Direct per-connection RX thread (no Reactor needed) |
| 2-16 connections, shared queue | Reactor |
| RSS-capable NIC, per-flow queues | Direct per-connection RX thread |

See also: [Multi-Connection Patterns](multi-connection.md) for socket-based (non-DPDK) multi-connection strategies.

## Architecture

```
                NIC Port 0, Queue 0
                       |
                  ┌────┴────┐
                  │ Reactor  │  (single RX thread)
                  │ rx_loop  │
                  └────┬────┘
                       |
         ┌─────────────┼─────────────┐
         |             |             |
    ┌────┴────┐  ┌────┴────┐  ┌────┴────┐
    │Session 0│  │Session 1│  │Session 2│
    │ on_data │  │ on_data │  │ on_data │
    └─────────┘  └─────────┘  └─────────┘
```

The Reactor RX thread polls the NIC, matches each packet to a registered connection by 4-tuple, and calls `TcpSession::process_rx` inline. No intermediate ring buffer.

## Basic Usage

```cpp
#include "eph/dpdk/reactor.hpp"

// 1. Create reactor for a specific NIC port and queue
eph::dpdk::Reactor reactor({
    .port_id     = 0,
    .rx_queue_id = 0,
    .rx_cpu      = 2,   // Pin RX thread to core 2 (-1 for no pinning)
});

// 2. Register connections (must be called BEFORE start())
// Each session must already be connected.
for (size_t i = 0; i < sessions.size(); ++i) {
    auto result = reactor.add_connection(&sessions[i],
        [i](const uint8_t* data, uint16_t len, size_t conn_id) {
            // Called inline from the RX thread -- keep it fast
            process_data(i, data, len);
        });

    if (!result) {
        SPDLOG_ERROR("Failed to add connection {}: {}", i, result.error());
    }
}

// 3. Start the RX loop (spawns a dedicated thread)
reactor.start();

// ... connections receive data via callbacks ...

// 4. Stop (blocks until the RX thread joins)
reactor.stop();
```

## Connection Lifecycle

### Adding Connections

Connections must be registered before `start()`. Modifying the connection list while the RX loop is running is not safe (no lock on the hot path by design).

```cpp
auto idx = reactor.add_connection(&session, callback);
if (idx) {
    // idx.value() is the 0-based connection index
}
```

Maximum connections per Reactor: `kReactorMaxConnections` (16).

### Handling Disconnects

Mark a connection as disconnected so the RX loop skips it. Safe to call while running.

```cpp
reactor.mark_disconnected(conn_id);
```

### Reconnecting

Swap the session pointer atomically. The Reactor uses a disable-fence-swap-enable protocol to ensure the RX loop never dereferences a stale pointer.

```cpp
// new_session is an already-connected TcpSession
reactor.mark_reconnected(conn_id, new_session);
```

Internally this:
1. Sets `connected = false` (RX loop skips this entry)
2. Issues a `seq_cst` fence (ensures RX loop observed the flag)
3. Swaps session pointer, 4-tuple, and hash
4. Sets `connected = true` (RX loop resumes processing)

## Packet Dispatch

The RX loop uses linear scan with FNV-1a hash pre-filtering:

1. Poll NIC: `rte_eth_rx_burst()` returns a batch of packets
2. Stamp burst TSC for latency measurement
3. For each packet: parse headers, compute 4-tuple hash
4. Linear scan registered connections: compare hash first (fast reject), then full 4-tuple match
5. On match: call `TcpSession::process_rx` inline, then `flush_pending_ack()`

For 2-4 connections (typical HFT), linear scan is faster than hash map lookup due to cache locality.

## Diagnostics

```cpp
// Connection count
size_t n = reactor.connection_count();

// Running state
bool running = reactor.is_running();

// Access individual entries (for stats / monitoring)
const auto& entry = reactor.entry(0);
// entry.session, entry.tuple, entry.connected
```

## CPU Pinning Strategy

```
Core 0:  OS / monitoring
Core 1:  Application thread (strategy logic)
Core 2:  Reactor RX thread (polls NIC, dispatches to all sessions)
Core 3:  Session 0 TX
Core 4:  Session 1 TX
...
```

The key advantage of Reactor: N connections need only 1 RX core (plus N TX cores), instead of N RX cores with per-connection polling.

## Integration with Transport

Reactor replaces the per-connection RX thread that Transport normally runs. The Transport TX thread remains per-connection. See `bench_market_persymbol_dpdk.cpp` for a complete integration example.

```cpp
// Typical setup:
// 1. Create DPDK transports (for TX path)
// 2. Extract TcpSession pointers from each transport
// 3. Register sessions with Reactor (for RX path)
// 4. Start Reactor
// 5. Application consumes data via Reactor callbacks
```

## Limitations

- Maximum 16 connections per Reactor (compile-time constant `kReactorMaxConnections`)
- Connections must be registered before `start()` -- no dynamic add while running
- All callbacks execute on the single RX thread -- keep them fast to avoid head-of-line blocking
- Not needed when the NIC supports RSS/Flow Director (use per-connection queues instead)
