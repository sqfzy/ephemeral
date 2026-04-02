# eph-containers

Header-only C++23 library providing lock-free and wait-free SPSC (single-producer, single-consumer) data structures for inter-thread communication in HFT pipelines. All containers are fixed-capacity, zero-allocation on the hot path, and use cache-line-aligned memory layouts to eliminate false sharing.

Two queue families are provided, each with a raw typed variant and a byte-oriented wrapper:

- **Bounded** queues block (spin) or fail when full -- no data loss.
- **Evicting** queues overwrite the oldest entry when full -- wait-free writes, with loss tracking.

A **RingBuffer** is also provided for fixed-size tick history lookback with bitmask indexing.

## Key Components

All headers are under `include/eph/containers/`:

- **concepts.hpp** -- `TrivialData<T>` concept requiring trivially copyable, trivially destructible, and default-initializable types. Gate for all queue element types.
- **ring_buffer.hpp** -- `RingBuffer<T, Capacity>` fixed-size circular buffer for tick history lookback. Power-of-two capacity (compile-time enforced) enables bitmask indexing with no division. SPSC-safe with acquire/release atomics. Supports indexed lookback (`at(offset)` where 0 = most recent), `front()`, `back()`, `count()`, `full()`, `empty()`, `clear()`.
- **bounded_queue.hpp** -- `BoundedQueue<T, Capacity>` lock-free SPSC queue. Writer blocks (spins) when full; reader blocks when empty. Power-of-two capacity. Supports zero-copy visitor-based produce/consume, batch operations (`try_produce_n`, `try_consume_n`), peek without advancing head, timed variants (`try_produce_for`, `try_consume_for`), and stats snapshots (`BoundedQueueStats`). Cache-line-padded writer/reader hot zones prevent false sharing.
- **bounded_queue_bytes.hpp** -- `BoundedQueueBytes<MaxDataSize, Capacity>` byte-oriented wrapper around `BoundedQueue`. Each slot carries a `uint8_t[MaxDataSize]` payload plus an optional `uint64_t` timestamp. Provides `try_push`/`try_pop`/`push`/`pop` with both copy and zero-copy (visitor) interfaces, batch push/consume, peek, timed variants, and `BoundedQueueBytesStats` with throughput computation and `std::formatter` support.
- **evicting_queue.hpp** -- `EvictingQueue<T, Capacity>` wait-free-write, lock-free-read SPSC queue using per-slot sequence locks (SeqLock). Writer never blocks -- old data is silently overwritten when full. Reader performs optimistic reads and retries on torn data. Includes an explicit `Capacity == 1` specialization (classic single-slot SeqLock). Supports `produce`, `try_consume_latest`, `try_peek_latest`, batch `produce_n`, timed reads, and `EvictingQueueStats` with loss rate tracking.
- **evicting_queue_bytes.hpp** -- `EvictingQueueBytes<MaxDataSize, Capacity>` byte-oriented wrapper around `EvictingQueue`. Each slot carries payload bytes, a monotonic message ID, and an optional timestamp. Provides `try_push`/`try_pop_latest` with copy and zero-copy interfaces, batch push, peek without updating discard state, timed variants, and `EvictingQueueBytesStats` with loss rate, throughput, and `std::formatter` support.
- **containers.hpp** -- Convenience umbrella header including all of the above (except `ring_buffer.hpp` and `concepts.hpp`, which are included transitively).

## Public API Reference

### Concepts (`eph::containers`)

| Name | Description |
|------|-------------|
| `TrivialData<T>` | Constrains `T` to be trivially copyable, trivially destructible, and default-initializable. Required for all queue/buffer element types. |

### `RingBuffer<T, Capacity>`

Fixed-size circular buffer. Single-writer, any-reader. `T` must be trivially copyable, `Capacity` must be a power of two.

| Method | Description |
|--------|-------------|
| `void push(const T&)` | Push element, overwriting oldest when full (writer-only). |
| `optional<T> at(size_t offset)` | Lookback read: 0 = newest, 1 = second newest, etc. Returns `nullopt` if offset >= count. |
| `optional<T> front()` | Most recent element. |
| `optional<T> back()` | Oldest element in buffer. |
| `size_t count()` | Number of stored elements (up to Capacity). |
| `bool full()` | Whether buffer is at capacity. |
| `bool empty()` | Whether buffer is empty. |
| `void clear()` | Reset to empty. **Not thread-safe.** |

### `BoundedQueue<T, Capacity>`

Lock-free SPSC FIFO queue with back-pressure. `T` must satisfy `TrivialData`, `Capacity` must be a power of two. Non-copyable, non-movable.

**Writer operations:**

| Method | Description |
|--------|-------------|
| `bool try_produce(F&&)` | Zero-copy write via visitor `void(T&)`. Returns false if full. |
| `bool try_push(U&&)` | Push by copy/move. Returns false if full. |
| `bool try_emplace(Args&&...)` | Emplace-construct. Returns false if full. |
| `bool try_push_n(span<const T>)` | Batch push (all-or-nothing). Returns false if insufficient space. |
| `bool try_produce_n(n, F&&)` | Batch zero-copy write via visitor `void(T&, size_t)`. All-or-nothing. |
| `void produce(F&&)` | Blocking zero-copy write (spins). |
| `void push(U&&)` | Blocking push (spins). |
| `void emplace(Args&&...)` | Blocking emplace (spins). |
| `void push_n(span<const T>)` | Blocking batch push (spins). |
| `void produce_n(n, F&&)` | Blocking batch zero-copy write (spins). |
| `bool try_produce_for(F&&, timeout)` | Zero-copy write with timeout. |
| `bool try_push_for(U&&, timeout)` | Push with timeout. |
| `bool try_emplace_for(timeout, Args&&...)` | Emplace with timeout. |
| `bool try_push_n_for(span<const T>, timeout)` | Batch push with timeout (all-or-nothing). |
| `bool try_produce_n_for(n, F&&, timeout)` | Batch zero-copy write with timeout. |

**Reader operations:**

| Method | Description |
|--------|-------------|
| `bool try_consume(F&&)` | Zero-copy read via visitor `void(const T&)`. Returns false if empty. |
| `bool try_peek(F&&)` | Zero-copy peek without consuming. |
| `bool try_peek(T&)` | Value-copy peek. |
| `optional<T> try_peek()` | Peek returning optional. |
| `bool try_pop(T&)` | Pop into out-param. Returns false if empty. |
| `optional<T> try_pop()` | Pop returning optional. |
| `size_t try_pop_n(span<T>)` | Batch pop (best-effort). Returns count dequeued. |
| `size_t try_consume_n(n, F&&)` | Batch zero-copy consume via visitor `void(const T&, size_t)`. |
| `size_t try_consume_all(F&&)` | Drain all visible elements (best-effort). |
| `void consume(F&&)` | Blocking zero-copy read (spins). |
| `void pop(T&)` | Blocking pop (spins). |
| `T pop()` | Blocking pop returning by value. |
| `size_t consume_n(n, F&&)` | Blocking batch consume (spins until >= 1). |
| `bool try_consume_for(F&&, timeout)` | Zero-copy read with timeout. |
| `bool try_pop_for(T&, timeout)` | Pop with timeout. |
| `optional<T> try_pop_for(timeout)` | Pop with timeout returning optional. |
| `size_t try_pop_n_for(span<T>, timeout)` | Batch pop with timeout. |
| `size_t try_consume_n_for(n, F&&, timeout)` | Batch zero-copy consume with timeout. |

**State queries:**

| Method | Description |
|--------|-------------|
| `void clear()` | Reset to empty. **Not thread-safe.** |
| `size_t size()` | Approximate current occupancy. |
| `bool empty()` | Approximate emptiness check. |
| `bool full()` | Approximate fullness check. |
| `static constexpr size_t capacity()` | Fixed capacity. |
| `size_t available_write()` | Estimated free slots. |
| `size_t available_read()` | Estimated readable elements (alias for `size()`). |
| `Stats stats()` | Point-in-time `BoundedQueueStats` snapshot. |

### `BoundedQueueStats`

| Member / Method | Description |
|-----------------|-------------|
| `size_t total_pushed` | Total items ever pushed (monotonic). |
| `size_t total_popped` | Total items ever popped (monotonic). |
| `size_t current_size` | Approximate current occupancy. |
| `size_t capacity` | Fixed capacity. |
| `string dump()` | Multi-line human-readable format. |
| `string to_json()` | Compact single-line JSON. |
| `operator-(lhs, rhs)` | Compute delta between two snapshots. |
| `double throughput(uint64_t duration_ns)` | Items popped per second over interval. |
| `std::formatter` support | Usable with `std::format` / `std::print`. |

### `EvictingQueue<T, Capacity>`

Wait-free-write, lock-free-read SPSC queue with SeqLock-based consistency. `T` must satisfy `TrivialData`, `Capacity` must be a power of two. Explicit specialization for `Capacity == 1` (classic single-slot SeqLock). Non-copyable, non-movable.

**Writer operations:**

| Method | Description |
|--------|-------------|
| `void produce(F&&)` | Zero-copy write via visitor `void(T&)`. Wait-free, overwrites oldest. |
| `void push(U&&)` | Push by copy/move (wait-free). |
| `void emplace(Args&&...)` | Emplace-construct (wait-free). |
| `void produce_n(count, F&&)` | Batch write via visitor `void(T&, size_t)`. |
| `void push_n(span<const T>)` | Batch push from span. |

**Reader operations:**

| Method | Description |
|--------|-------------|
| `bool try_consume_latest(F&&)` | Zero-copy read of latest via visitor `void(const T&)`. Lock-free. |
| `bool try_pop_latest(T&)` | Pop latest into out-param. |
| `optional<T> try_pop_latest()` | Pop latest returning optional. |
| `bool try_peek_latest(F&&)` | Zero-copy peek without consuming (does not advance read index). |
| `bool try_peek_latest(T&)` | Value-copy peek without consuming. |
| `optional<T> try_peek_latest()` | Peek returning optional. |
| `void consume_latest(F&&)` | Blocking zero-copy read (spins). |
| `void pop_latest(T&)` | Blocking pop (spins). |
| `T pop_latest()` | Blocking pop returning by value. |
| `bool try_consume_latest_for(F&&, timeout)` | Zero-copy read with timeout. |
| `bool try_pop_latest_for(T&, timeout)` | Pop with timeout. |
| `optional<T> try_pop_latest_for(timeout)` | Pop with timeout returning optional. |
| `bool try_peek_latest_for(F&&, timeout)` | Peek with timeout (non-consuming). |
| `bool try_peek_latest_for(T&, timeout)` | Value-copy peek with timeout. |
| `optional<T> try_peek_latest_for(timeout)` | Peek with timeout returning optional. |

**State queries:**

| Method | Description |
|--------|-------------|
| `void clear()` | Reset to empty. **Not thread-safe.** |
| `static constexpr size_t capacity()` | Fixed capacity. |
| `size_t size_approx()` | Approximate unread entries. |
| `bool empty()` | Approximate emptiness check. |
| `bool full()` | Approximate fullness check. |
| `uint64_t write_count()` | Total writes since construction. |
| `uint64_t read_count()` | Total successful reads (reader-thread only for accuracy). |
| `uint64_t overwrite_count_approx()` | Approximate data loss count. |
| `Stats stats()` | Point-in-time `EvictingQueueStats` snapshot. |

### `EvictingQueueStats`

| Member / Method | Description |
|-----------------|-------------|
| `uint64_t total_pushed` | Total writes since construction. |
| `uint64_t total_popped` | Total successful reads. |
| `uint64_t overwritten` | Approximate overwrites (data loss). |
| `size_t current_size` | Approximate unread entries. |
| `size_t capacity` | Fixed capacity. |
| `string dump()` | Multi-line human-readable format with loss rate. |
| `string to_json()` | Compact single-line JSON. |
| `operator-(lhs, rhs)` | Compute delta between two snapshots. |
| `double throughput(uint64_t duration_ns)` | Items consumed per second over interval. |
| `double loss_rate()` | Data loss fraction in [0.0, 1.0]. |
| `std::formatter` support | Usable with `std::format` / `std::print`. |

### `BoundedQueueBytes<MaxDataSize, Capacity>`

Byte-oriented bounded SPSC queue wrapping `BoundedQueue`. Each slot is a `DataWrap` envelope with `uint8_t[MaxDataSize]` payload, `uint32_t len`, and `uint64_t ts`. Defaults: `MaxDataSize=256`, `Capacity=256`.

**Writer operations:**

| Method | Description |
|--------|-------------|
| `bool try_push(span<const uint8_t>)` | Non-blocking push (ts=0). Returns false if full or oversized. |
| `bool try_push_wts(span<const uint8_t>, uint64_t ts)` | Non-blocking push with timestamp. |
| `bool try_push_n(payloads, count)` | Batch push (all-or-nothing, ts=0). |
| `bool try_push_n_wts(payloads, timestamps, count)` | Batch push with timestamps (all-or-nothing). |
| `bool push(span<const uint8_t>)` | Blocking push (spins, ts=0). |
| `bool push_wts(span<const uint8_t>, uint64_t ts)` | Blocking push with timestamp. |
| `bool try_push_for(span<const uint8_t>, timeout)` | Push with timeout (ts=0). |
| `bool try_push_wts_for(span<const uint8_t>, ts, timeout)` | Push with timestamp and timeout. |

**Reader operations:**

| Method | Description |
|--------|-------------|
| `bool try_consume(F&&)` | Zero-copy consume via visitor `void(span<const uint8_t>)`. |
| `bool try_consume_wts(F&&)` | Zero-copy consume with timestamp `void(span<const uint8_t>, uint64_t)`. |
| `optional<uint32_t> try_pop(span<uint8_t>)` | Pop into buffer. Returns bytes copied. |
| `optional<uint32_t> try_pop_wts(span<uint8_t>, uint64_t&)` | Pop with timestamp. |
| `size_t try_consume_n(n, F&&)` | Batch consume via visitor `void(span<const uint8_t>, size_t)`. |
| `size_t try_consume_n_wts(n, F&&)` | Batch consume with timestamps. |
| `size_t try_consume_all(F&&)` | Drain all visible messages (best-effort). |
| `size_t try_consume_all_wts(F&&)` | Drain with timestamps. |
| `optional<uint32_t> try_peek(span<uint8_t>)` | Peek without consuming. |
| `optional<uint32_t> try_peek_wts(span<uint8_t>, uint64_t&)` | Peek with timestamp. |
| `bool try_peek_visit(F&&)` | Zero-copy peek via visitor. |
| `bool try_peek_visit_wts(F&&)` | Zero-copy peek with timestamp. |
| `void consume(F&&)` | Blocking zero-copy consume (spins). |
| `void consume_wts(F&&)` | Blocking consume with timestamp. |
| `uint32_t pop(span<uint8_t>)` | Blocking pop. Returns bytes copied. |
| `uint32_t pop_wts(span<uint8_t>, uint64_t&)` | Blocking pop with timestamp. |
| `bool try_consume_for(F&&, timeout)` | Zero-copy consume with timeout. |
| `bool try_consume_wts_for(F&&, timeout)` | Consume with timestamp and timeout. |
| `optional<uint32_t> try_pop_for(span<uint8_t>, timeout)` | Pop with timeout. |
| `optional<uint32_t> try_pop_wts_for(span<uint8_t>, uint64_t&, timeout)` | Pop with timestamp and timeout. |

**State queries:**

| Method | Description |
|--------|-------------|
| `void clear()` | Reset to empty. **Not thread-safe.** |
| `size_t size()` | Approximate current occupancy. |
| `bool empty()` | Approximate emptiness check. |
| `bool full()` | Approximate fullness check. |
| `static constexpr size_t capacity()` | Fixed capacity. |
| `size_t available_write()` | Estimated free slots. |
| `size_t available_read()` | Estimated readable messages. |
| `Stats stats()` | Point-in-time `BoundedQueueBytesStats` snapshot. |

### `BoundedQueueBytesStats`

Same fields as `BoundedQueueStats` (`total_pushed`, `total_popped`, `current_size`, `capacity`) plus `dump()`, `to_json()`, `operator-`, `throughput(duration_ns)`, and `std::formatter` support.

### `EvictingQueueBytes<MaxDataSize, Capacity>`

Byte-oriented evicting SPSC queue wrapping `EvictingQueue`. Each slot is a `DataWrap` envelope with monotonic `uint64_t id`, `uint64_t ts`, `uint32_t len`, and `uint8_t[MaxDataSize]` payload. Defaults: `MaxDataSize=256`, `Capacity=256`.

**Writer operations (wait-free):**

| Method | Description |
|--------|-------------|
| `bool try_push(span<const uint8_t>)` | Push (ts=0). Only returns false if payload > MaxDataSize. |
| `bool try_push_wts(span<const uint8_t>, uint64_t ts)` | Push with timestamp. |
| `bool push_n(payloads, count)` | Batch push (ts=0). |
| `bool push_n_wts(payloads, timestamps, count)` | Batch push with timestamps. |

**Reader operations (lock-free):**

| Method | Description |
|--------|-------------|
| `bool try_consume_latest(F&&)` | Zero-copy consume via visitor `void(span<const uint8_t>)`. |
| `bool try_consume_latest_wts(F&&)` | Consume with timestamp and discard count `void(span<const uint8_t>, uint64_t, uint32_t)`. |
| `optional<uint32_t> try_pop_latest(span<uint8_t>)` | Pop latest into buffer. |
| `optional<uint32_t> try_pop_latest_wts(span<uint8_t>, uint64_t&, uint32_t&)` | Pop with timestamp and discard count. |
| `optional<uint32_t> try_peek_latest(span<uint8_t>)` | Peek without consuming (does not update discard state). |
| `optional<uint32_t> try_peek_latest_wts(span<uint8_t>, uint64_t&)` | Peek with timestamp. |
| `bool try_peek_latest_visit(F&&)` | Zero-copy peek via visitor. |
| `bool try_peek_latest_visit_wts(F&&)` | Zero-copy peek with timestamp. |
| `void consume_latest(F&&)` | Blocking zero-copy consume (spins). |
| `void consume_latest_wts(F&&)` | Blocking consume with timestamp and discard count. |
| `uint32_t pop_latest(span<uint8_t>)` | Blocking pop. Returns bytes copied. |
| `uint32_t pop_latest_wts(span<uint8_t>, uint64_t&, uint32_t&)` | Blocking pop with timestamp and discard count. |
| `bool try_consume_latest_for(F&&, timeout)` | Consume with timeout. |
| `bool try_consume_latest_wts_for(F&&, timeout)` | Consume with timestamp, discard count, and timeout. |
| `optional<uint32_t> try_pop_latest_for(span<uint8_t>, timeout)` | Pop with timeout. |
| `optional<uint32_t> try_pop_latest_wts_for(span<uint8_t>, uint64_t&, uint32_t&, timeout)` | Pop with timestamp, discard count, and timeout. |
| `bool try_peek_latest_visit_for(F&&, timeout)` | Peek with timeout. |
| `bool try_peek_latest_visit_wts_for(F&&, timeout)` | Peek with timestamp and timeout. |

**State queries:**

| Method | Description |
|--------|-------------|
| `void clear()` | Reset to empty. **Not thread-safe.** |
| `static constexpr size_t capacity()` | Fixed capacity. |
| `uint64_t total_pushed()` | Monotonic push count (writer-thread only for accuracy). |
| `size_t size_approx()` | Approximate unread entries. |
| `bool empty()` | Approximate emptiness check. |
| `Stats stats()` | Point-in-time `EvictingQueueBytesStats` snapshot. |

### `EvictingQueueBytesStats`

| Member / Method | Description |
|-----------------|-------------|
| `uint64_t total_pushed` | Total messages pushed (writer-side). |
| `uint64_t last_pop_id` | ID of last consumed message (reader-side). |
| `uint64_t total_popped` | Total messages consumed. |
| `size_t current_size` | Approximate unread entries. |
| `size_t capacity` | Fixed capacity. |
| `uint64_t total_overwritten` | Messages overwritten before being read. |
| `string dump()` | Multi-line human-readable format. |
| `string to_json()` | Compact single-line JSON. |
| `operator-(lhs, rhs)` | Compute delta between two snapshots. |
| `double throughput(uint64_t duration_ns)` | Messages consumed per second. |
| `double loss_rate()` | Data loss fraction in [0.0, 1.0]. |
| `std::formatter` support | Usable with `std::format` / `std::print`. |

## Dependencies

- **eph-utils** -- Cache-line alignment utilities (`alignment.hpp`) and `cpu_relax()` spin hint (`cpu.hpp`)
- **spdlog** -- Logging (bounded_queue only, compile-time filtered via `SPDLOG_ACTIVE_LEVEL`)

## Usage Examples

### Bounded byte queue -- reliable inter-thread messaging

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

### Evicting byte queue -- market data feed with loss tracking

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

### Ring buffer -- tick history lookback

```cpp
#include <eph/containers/ring_buffer.hpp>

// 128-tick lookback buffer for signal computation
eph::containers::RingBuffer<double, 128> prices;

prices.push(100.5);
prices.push(100.7);

auto latest = prices.at(0);  // 100.7
auto prev   = prices.at(1);  // 100.5
```

### Typed bounded queue -- struct-level SPSC

```cpp
#include <eph/containers/bounded_queue.hpp>

struct OrderMsg {
    uint64_t order_id;
    double   price;
    uint32_t qty;
};

eph::containers::BoundedQueue<OrderMsg, 512> order_queue;

// Producer -- zero-copy write
order_queue.try_produce([](OrderMsg& slot) {
    slot = {.order_id = 42, .price = 100.25, .qty = 100};
});

// Consumer -- batch drain
order_queue.try_consume_all([](const OrderMsg& msg, size_t idx) {
    // process msg
});
```

### Typed evicting queue -- latest-value semantics

```cpp
#include <eph/containers/evicting_queue.hpp>

struct Quote {
    double bid, ask;
    uint64_t seq;
};

eph::containers::EvictingQueue<Quote, 8> quote_feed;

// Producer (wait-free, never blocks)
quote_feed.push(Quote{.bid = 99.5, .ask = 100.5, .seq = 1});

// Consumer -- read latest with timeout
Quote q;
if (quote_feed.try_pop_latest_for(q, std::chrono::microseconds(100))) {
    // process latest quote
}
```

### Observability -- stats and monitoring

```cpp
#include <eph/containers/bounded_queue_bytes.hpp>
#include <format>

eph::containers::BoundedQueueBytes<256, 1024> queue;
// ... produce/consume ...

auto s = queue.stats();
std::println("{}", s);                   // human-readable via std::formatter
std::println("{}", s.to_json());         // JSON for monitoring systems

// Interval-based throughput
auto s1 = queue.stats();
// ... wait ...
auto s2 = queue.stats();
auto delta = s2 - s1;
double ops_per_sec = delta.throughput(elapsed_ns);
```
