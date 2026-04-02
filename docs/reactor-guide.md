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

## Code Walkthrough

### Data Structures

```
entries_[16]: ReactorEntry    hashes_[16]: uint64_t
┌─────────────────────┐       ┌──────────┐
│ [0] session*        │       │ 0xA3F... │  ← FNV-1a(4-tuple)
│     tuple{src,dst}  │       ├──────────┤
│     on_data callback│       │ 0x7B2... │
│     connected: bool │       ├──────────┤
├─────────────────────┤       │ ...      │
│ [1] ...             │       └──────────┘
```

- Fixed array, max 16 connections — no heap allocation on hot path
- `hashes_[]` stored separately for cache-friendly traversal during linear scan

### Registration Phase (`add_connection`)

```
add_connection(session, callback)
  │
  ├── Guard: running==true? → reject (hot path is lock-free by design)
  ├── Guard: session==null? → reject
  ├── Guard: count>=16? → reject
  │
  ├── entries_[idx].session = session
  ├── entries_[idx].tuple = session->connection_tuple()  // capture 4-tuple
  ├── entries_[idx].on_data = callback
  ├── entries_[idx].connected = session->is_established()
  ├── hashes_[idx] = FNV-1a(tuple)                      // precompute hash
  │
  └── count_.store(idx+1, release)  // publish to RX thread
```

### RX Main Loop (`rx_loop`)

```
while (running_) {
    ┌─── rte_eth_rx_burst(port, queue, pkts, 32) ───┐
    │                                                 │
    │  Batch receive up to 32 packets from NIC        │
    └─────────────────────────────────────────────────┘
                      │
                      │ nb_rx == 0 → continue (busy poll)
                      ▼
    burst_tsc = TSC::now()    ← record batch arrival timestamp
                      │
    ┌─── for each pkt ─────────────────────────────────┐
    │                                                   │
    │  1. parse_packet(pkt)                             │
    │     └── not TCP? → free, skip                     │
    │                                                   │
    │  2. Extract pkt 4-tuple, compute FNV-1a hash      │
    │     pkt_tuple = {src_ip, dst_ip, src_port, dst_port}
    │     pkt_hash = hash_tuple(pkt_tuple)              │
    │                                                   │
    │  3. Linear scan entries_[0..n):                    │
    │     ┌─── for j in 0..n ──────────────────────┐   │
    │     │                                         │   │
    │     │  hashes_[j] != pkt_hash?                │   │
    │     │  └── YES → continue (fast reject)       │   │
    │     │                                         │   │
    │     │  !parsed.matches(tuple)?                │   │
    │     │  └── YES → continue (hash collision)    │   │
    │     │                                         │   │
    │     │  !connected? (acquire load)             │   │
    │     │  └── YES → free pkt, break (skip)       │   │
    │     │                                         │   │
    │     │  ✓ Match found:                         │   │
    │     │  session->set_last_rx_burst_tsc(tsc)    │   │
    │     │  session->process_rx(&pkt, 1, callback) │   │
    │     │     └── TCP state machine               │   │
    │     │     └── payload → on_data(data, len, j) │   │
    │     │  if error → mark disconnected           │   │
    │     │  session->flush_pending_ack()           │   │
    │     │  break                                  │   │
    │     └─────────────────────────────────────────┘   │
    │                                                   │
    │  4. No match? → free pkt                          │
    └───────────────────────────────────────────────────┘
}
```

### Reconnection Protocol (`mark_reconnected`)

Safe session pointer swap while RX loop is running — disable → fence → swap → enable:

```
mark_reconnected(conn_id, new_session)
  │
  │ Step 1: connected = false (release)
  │         ↓ RX loop's next acquire-load sees false, skips this entry
  │
  │ Step 2: atomic_thread_fence(seq_cst)
  │         ↓ Guarantees any in-flight RX iteration that loaded
  │           connected=true (old value) has fully completed before
  │           we touch the session pointer
  │
  │ Step 3: Safe to swap — RX loop is guaranteed to skip this entry
  │         session = new_session
  │         tuple = new_session->connection_tuple()
  │         hash = hash_tuple(tuple)
  │
  │ Step 4: connected = true (release)
  │         ↓ RX loop resumes processing this entry
```

### Design Decisions

| Decision | Rationale |
|----------|-----------|
| Linear scan, not hash map | HFT: 2-4 connections, linear scan is cache-friendly with no indirection overhead |
| FNV-1a hash pre-filter | O(1) fast reject avoids per-field comparison for non-matching entries |
| Fixed array `[16]` | No heap allocation on hot path; compiler can unroll the loop |
| Zero ring | Direct NIC → parse → dispatch → callback, no intermediate queue copy |
| `add_connection` before start only | Lock-free hot path, traded for startup-time-only registration constraint |

## Reactor + Transport Integration (TLS/WebSocket)

Reactor delivers raw TCP payload. To add TLS decryption and WebSocket decoding, pair Reactor with Transport in `kDirect` mode using `feed_rx()` + `process_pending()`:

```cpp
// 1. Create Transport (kDirect — no background threads)
auto tp = DpdkDirectTransport::create(tcp_factory, config);

// 2. Create Reactor
Reactor reactor({.port_id = 0, .rx_queue_id = 0, .rx_cpu = 2});

// 3. Wire: Reactor on_data → Transport feed_rx
reactor.add_connection(session, [&tp](const uint8_t* data, uint16_t len, size_t) {
    tp->feed_rx(data, len);   // accumulates to reassembly buffer (memcpy only)
});

// 4. Wire: burst complete → Transport process_pending
reactor.set_on_burst_complete([&tp]() {
    tp->process_pending();    // TLS decrypt → WS decode → on_message callback
});

// 5. Start — Reactor thread drives the full pipeline
reactor.start();
```

**Data flow**: NIC → Reactor burst → `feed_rx` (memcpy) → `process_pending` (TLS → WS → callback).

Reactor and Transport are **independent modules** (zero `#include` dependency between eph-dpdk and eph-transport). The wiring happens in user application code.

For multiple connections, use one Transport per connection:
```cpp
std::vector<std::unique_ptr<DpdkDirectTransport>> transports;
for (size_t i = 0; i < N; ++i) {
    reactor.add_connection(sessions[i], [&, i](auto* d, auto l, auto) {
        transports[i]->feed_rx(d, l);
    });
}
reactor.set_on_burst_complete([&]() {
    for (auto& tp : transports) tp->process_pending();
});
```

## Limitations

- Maximum 16 connections per Reactor (compile-time constant `kReactorMaxConnections`)
- Connections must be registered before `start()` -- no dynamic add while running
- All callbacks execute on the single RX thread -- keep them fast to avoid head-of-line blocking
- Not needed when the NIC supports RSS/Flow Director (use per-connection queues instead)
