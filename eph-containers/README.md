# eph-containers

Header-only C++23 library providing lock-free and wait-free SPSC (single-producer, single-consumer) data structures for inter-thread communication in HFT pipelines. All containers are fixed-capacity, zero-allocation on the hot path, and use cache-line-aligned memory layouts to eliminate false sharing.

Two queue families are provided, each with a raw typed variant and a byte-oriented wrapper:

- **Bounded** queues block (spin) or fail when full -- no data loss.
- **Evicting** queues overwrite the oldest entry when full -- wait-free writes, with loss tracking.

## Key Components

All headers are under `include/eph/containers/`:

- **concepts.hpp** -- `TrivialData<T>` concept requiring trivially copyable, trivially destructible, and default-initializable types. Gate for all queue element types.
- **ring_buffer.hpp** -- `RingBuffer<T, Capacity>` fixed-size circular buffer for tick history lookback. Power-of-two capacity (compile-time enforced) enables bitmask indexing with no division. SPSC-safe with acquire/release atomics. Supports indexed lookback (`at(offset)` where 0 = most recent), `front()`, `back()`, `count()`, `full()`, `empty()`, `clear()`.
- **bounded_queue.hpp** -- `BoundedQueue<T, Capacity>` lock-free SPSC queue. Writer blocks (spins) when full; reader blocks when empty. Power-of-two capacity. Supports zero-copy visitor-based produce/consume, batch operations (`try_produce_n`, `try_consume_n`), peek without advancing head, timed variants (`try_produce_for`, `try_consume_for`), and stats snapshots (`BoundedQueueStats`). Cache-line-padded writer/reader hot zones prevent false sharing.
- **bounded_queue_bytes.hpp** -- `BoundedQueueBytes<MaxDataSize, Capacity>` byte-oriented wrapper around `BoundedQueue`. Each slot carries a `uint8_t[MaxDataSize]` payload plus an optional `uint64_t` timestamp. Provides `try_push`/`try_pop`/`push`/`pop` with both copy and zero-copy (visitor) interfaces, batch push/consume, peek, timed variants, and `BoundedQueueBytesStats` with throughput computation and `std::formatter` support.
- **evicting_queue.hpp** -- `EvictingQueue<T, Capacity>` wait-free-write, lock-free-read SPSC queue using per-slot sequence locks (SeqLock). Writer never blocks -- old data is silently overwritten when full. Reader performs optimistic reads and retries on torn data. Supports `produce`, `try_consume_latest`, `try_peek_latest`, batch `produce_n`, timed reads, and `EvictingQueueStats` with loss rate tracking.
- **evicting_queue_bytes.hpp** -- `EvictingQueueBytes<MaxDataSize, Capacity>` byte-oriented wrapper around `EvictingQueue`. Each slot carries payload bytes, a monotonic message ID, and an optional timestamp. Provides `try_push`/`try_pop_latest` with copy and zero-copy interfaces, batch push, peek without updating discard state, timed variants, and `EvictingQueueBytesStats` with loss rate, throughput, and `std::formatter` support.
- **containers.hpp** -- Convenience umbrella header including all of the above (except `ring_buffer.hpp` and `concepts.hpp`, which are included transitively).

## Dependencies

- **eph-utils** -- Cache-line alignment utilities (`alignment.hpp`) and `cpu_relax()` spin hint (`cpu.hpp`)
- **spdlog** -- Logging (bounded_queue only)

## Quick Start

```cpp
#include <eph/containers/bounded_queue_bytes.hpp>

// 256-byte max payload, 1024-slot SPSC byte queue
eph::containers::BoundedQueueBytes<256, 1024> queue;

// Producer thread
std::array<uint8_t, 64> msg = { /* ... */ };
bool ok = queue.try_push(std::span{msg});

// Consumer thread -- zero-copy visitor
queue.try_consume([](std::span<const uint8_t> data) {
    // process data without copying
});
```

```cpp
#include <eph/containers/evicting_queue_bytes.hpp>

// Wait-free write queue for market data -- slow readers lose old ticks
eph::containers::EvictingQueueBytes<128, 64> feed;

// Producer (never blocks)
feed.try_push_wts(payload, timestamp_ns);

// Consumer -- get latest, with discard count
feed.try_consume_latest_wts([](std::span<const uint8_t> data,
                               uint64_t ts, uint32_t discarded) {
    if (discarded > 0) { /* log gap */ }
    // process latest tick
});
```

```cpp
#include <eph/containers/ring_buffer.hpp>

// 128-tick lookback buffer for signal computation
eph::containers::RingBuffer<double, 128> prices;

prices.push(100.5);
prices.push(100.7);

auto latest = prices.at(0);  // 100.7
auto prev   = prices.at(1);  // 100.5
```
