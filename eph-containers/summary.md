# Project: eph-containers

> Header-only C++23 library of lock-free SPSC queues and a lookback
> ring buffer for ultra-low-latency inter-thread communication.

**Language**: C++23 &nbsp;|&nbsp; **Build**: xmake &nbsp;|&nbsp; **License**: unspecified

---

## Table of Contents
1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Module Map](#module-map)
4. [Data Flow](#data-flow)
5. [Key Components](#key-components)
6. [Entry Points & APIs](#entry-points--apis)
7. [Dependencies](#dependencies)
8. [Testing](#testing)

---

## Overview

`eph-containers` is a header-only C++23 library that provides the core
inter-thread data structures for the parent `ephemeral_dev` high-
frequency trading / networking stack. Every container is fixed-
capacity, allocation-free on the hot path, and optimised for the
single-producer / single-consumer (SPSC) scenario that dominates HFT
pipelines: one network / feed thread writes, one strategy or logger
thread reads.

Two queue families are offered:

- **Bounded** queues apply back-pressure — `try_push` returns `false`
  when the queue is full and `push` spins until space is available. No
  data is silently dropped. Used for reliable streams between pinned
  threads (e.g. order submissions, WebSocket send queues).
- **Evicting** queues are **wait-free** on the writer side and
  overwrite the oldest unread entry when the queue is full. Reads are
  lock-free, validated by per-slot sequence locks (SeqLock). Used for
  feed-handler tick streams where stale data is worthless anyway.

A companion `RingBuffer<T, Capacity>` provides single-writer / any-
reader lookback (e.g. "the price 5 ticks ago") without acquiring a
lock. Byte-oriented envelope wrappers (`BoundedQueueBytes`,
`EvictingQueueBytes`) sit on top of the typed queues and carry
variable-length binary payloads plus an optional user timestamp.

All containers are gated by the `TrivialData<T>` concept (trivially
copyable, trivially destructible, default initialisable) so that the
underlying `memcpy`-style semantics of the ring buffers are sound.

---

## Architecture

Layered, header-only, compile-time-driven. No runtime configuration;
all sizing is template-parametric.

### Component Diagram

```
              +-----------------------------+
              |  eph/containers.hpp         |
              |  (umbrella header)          |
              +--+-------------+----------+-+
                 |             |          |
                 v             v          v
     +-----------------+ +-------------+ +-----------+
     | ring_buffer.hpp | | bounded_    | | evicting_ |
     | RingBuffer<T,N> | | queue.hpp   | | queue.hpp |
     +--------+--------+ | BoundedQueue| | Evicting  |
              |          | <T,N>       | | Queue<T,N>|
              |          +------+------+ | + <T,1>   |
              |                 ^        +-----+-----+
              |                 |              ^
              |          +------+------+ +-----+-------+
              |          | bounded_    | | evicting_   |
              |          | queue_bytes | | queue_bytes |
              |          | <Max,N>     | | <Max,N>     |
              |          +-------------+ +-------------+
              |
              v
     +-----------------+
     | concepts.hpp    |   (TrivialData<T>)
     +-----------------+

All files ultimately depend on eph-utils (alignment, cpu_relax).
```

---

## Module Map

| Module / File | Responsibility | Key Types | Depends On |
|---|---|---|---|
| `include/eph/containers.hpp` | Umbrella header — pulls in every public type. | — | all sub-headers |
| `include/eph/containers/concepts.hpp` | Element-type constraint. | `TrivialData<T>` | `<concepts>`, `<type_traits>` |
| `include/eph/containers/ring_buffer.hpp` | Single-writer / any-reader lookback buffer. | `RingBuffer<T, Capacity>` | `<array>`, `<atomic>`, `<bit>`, `<optional>` |
| `include/eph/containers/bounded_queue.hpp` | Lock-free SPSC FIFO with back-pressure. | `BoundedQueue<T, Capacity>`, `BoundedQueueStats` | `concepts.hpp`, `eph-utils`, `spdlog` |
| `include/eph/containers/bounded_queue_bytes.hpp` | Byte-envelope wrapper over `BoundedQueue`. | `BoundedQueueBytes<MaxDataSize, Capacity>`, `BoundedQueueBytesStats` | `bounded_queue.hpp` |
| `include/eph/containers/evicting_queue.hpp` | Wait-free-write, lock-free-read SPSC via SeqLock. | `EvictingQueue<T, Capacity>`, `EvictingQueue<T, 1>` specialisation, `EvictingQueueStats` | `concepts.hpp`, `eph-utils` |
| `include/eph/containers/evicting_queue_bytes.hpp` | Byte-envelope wrapper over `EvictingQueue`. | `EvictingQueueBytes<MaxDataSize, Capacity>`, `EvictingQueueBytesStats` | `evicting_queue.hpp` |

All sources live under `include/eph/`. The xmake target is header-only
(`set_kind("headeronly")`) and simply installs these headers.

---

## Data Flow

### BoundedQueue

Classic two-index ring buffer. Writer increments `tail_`, reader
increments `head_`; both maintain a **shadow** copy of the peer's
index in their own cache line to avoid a cross-core atomic read on the
fast path.

```
  Producer                        Consumer
  --------                        --------
  tail = writer.tail_             head = reader.head_
  fast path: tail - shadow_head   fast path: shadow_tail - head
      < Capacity  ?                   > 0  ?
      |                               |
      +- yes: write slot              +- yes: read slot
      |       release(tail+1)         |       release(head+1)
      |                               |
      +- no: acquire(head)            +- no: acquire(tail)
              refresh shadow_head             refresh shadow_tail
```

### EvictingQueue (primary template, Capacity > 1)

Writer is wait-free: compute next global index, lock slot (seq → odd),
write, unlock (seq → even, release), publish global index. Reader is
optimistic: acquire global_index, acquire slot seq, copy data,
re-load seq, retry on mismatch.

```
  Writer (wait-free)               Reader (lock-free)
  ------------------               ------------------
  idx = shadow + 1                 idx = global_index.load(acq)
  s = slots[idx & mask]            if idx <= last_read -> nothing
  s.seq = odd(idx)                 s = slots[idx & mask]
  fence(release)                   seq1 = s.seq.load(acq)
  write s.data                     if locked(seq1) -> retry
  s.seq = even(idx)   (release)    copy data
  global_index = idx  (release)    fence(acq)
  shadow = idx                     seq2 = s.seq.load(rel)
                                   if seq1 == seq2 -> commit
                                   else -> retry
```

The single-slot specialisation (`EvictingQueue<T, 1>`) collapses this
to a classic SeqLock over a single data element — no ring, no
per-slot state.

### Byte envelopes

Both `BoundedQueueBytes` and `EvictingQueueBytes` wrap the typed queue
around a fixed-size `DataWrap` struct holding
`{ id?, ts, len, data[MaxDataSize] }`. Writers `memcpy` the payload
into the slot; readers compute `safe_len = min(msg.len, MaxDataSize)`
to defend against torn SeqLock reads and hand a
`std::span<const uint8_t>` to a visitor. `EvictingQueueBytes`
additionally tracks a monotonic per-message `id` so that readers can
observe the number of messages that were silently overwritten between
successive reads (`discarded` out-parameter).

---

## Key Components

### `BoundedQueue<T, Capacity>`

**File**: `include/eph/containers/bounded_queue.hpp`
**Purpose**: Lock-free SPSC FIFO with back-pressure.
**Notes**: Writer and reader zones each occupy their own cache line
(enforced by `alignas(Align<T>)` and `static_assert`). Shadow
indices eliminate cross-core reads on the fast path; a full index
refresh only happens when the local cache predicts full/empty.
Capacity must be a power of two (compile-time checked) so the modulo
reduces to a bitmask. Exposes `try_produce`/`produce` visitor-based
zero-copy writes, `try_push`/`push`/`try_emplace` value helpers, batch
`try_push_n`/`try_produce_n`, `try_peek` (non-advancing), `try_pop`,
`try_pop_n`, `try_consume_all` drain helper, and `*_for` timed
variants for every operation.

### `EvictingQueue<T, Capacity>` and `EvictingQueue<T, 1>`

**File**: `include/eph/containers/evicting_queue.hpp`
**Purpose**: Wait-free-write, lock-free-read SPSC queue. Writer never
blocks; instead it silently overwrites the oldest unread entry.
Intended for market-data / telemetry streams where stale data is
worthless.
**Notes**: Per-slot `seq_` encodes `{global_index << 1 | lock_flag}`.
Writer emits `stlr` on ARM64 to unlock + publish in one instruction.
Readers detect torn reads via `seq1 == seq2` validation. The
`Capacity == 1` specialisation avoids the slot array entirely and
uses a classic SeqLock over a single data element for minimum
footprint and latency. Observability counters include
`write_count()`, `read_count()`, `overwrite_count_approx()` and a
full `EvictingQueueStats` snapshot with loss-rate computation.

### `BoundedQueueBytes<MaxDataSize, Capacity>`

**File**: `include/eph/containers/bounded_queue_bytes.hpp`
**Purpose**: Byte-oriented envelope on top of `BoundedQueue`, for
reliable streaming of variable-length binary frames.
**Notes**: Oversized payloads (`> MaxDataSize`) cause `try_push` to
return `false` without enqueuing. On the read side the visitor
always receives `span<const uint8_t>` clamped to
`safe_len = min(msg.len, MaxDataSize)` — defence-in-depth even
though the bounded queue is not subject to SeqLock tearing.

### `EvictingQueueBytes<MaxDataSize, Capacity>`

**File**: `include/eph/containers/evicting_queue_bytes.hpp`
**Purpose**: Byte envelope on top of `EvictingQueue`. Adds a 1-based
monotonic `id` per message so consumers can observe silent drops.
**Notes**: `discarded = read_id − last_pop_id − 1` (clamped to
`uint32_t`), computed on every consume. `total_pushed` is tracked by
a writer-side atomic because the underlying `EvictingQueue` counts
slots, not unique messages. The read path clamps `msg.len` to
`MaxDataSize` specifically because SeqLock tearing can produce
garbage lengths.

### `RingBuffer<T, Capacity>`

**File**: `include/eph/containers/ring_buffer.hpp`
**Purpose**: Fixed-size circular buffer for tick-history lookback
("price N ticks ago", "VWAP over last N ticks"). Single writer,
any-reader (no mutex).
**Notes**: Much thinner than the SPSC queues — only `head_` and
`count_` are atomic; element copies are plain array reads. Readers
may briefly observe a stale element when the writer races past, but
never an invalid index. Capacity must be a power of two; enforced at
compile time via `std::has_single_bit`.

### `BoundedQueueStats` / `EvictingQueueStats` (+ *Bytes* variants)

**Files**: standalone structs at the top of each queue header.
**Purpose**: Point-in-time counter snapshots separated from the queue
type so that a `std::formatter` specialisation can be provided
without template parameters. Each header ends with a
`std::formatter<StatsType>` specialisation that delegates to
`.dump()`. Snapshots support `operator-` (delta), `throughput(ns)`,
`to_json()`, `dump()` (multi-line text), and for the evicting
variants `loss_rate()`.

---

## Entry Points & APIs

| Entrypoint | Type | Description |
|---|---|---|
| `eph::containers::BoundedQueue<T, N>` | template class | Reliable SPSC FIFO |
| `eph::containers::BoundedQueueBytes<Max, N>` | template class | Reliable byte-envelope SPSC |
| `eph::containers::EvictingQueue<T, N>` | template class | Wait-free write / lock-free read SPSC |
| `eph::containers::EvictingQueue<T, 1>` | template specialisation | Single-slot SeqLock (latest-value) |
| `eph::containers::EvictingQueueBytes<Max, N>` | template class | Wait-free byte envelope with discard tracking |
| `eph::containers::RingBuffer<T, N>` | template class | Lookback tick history |
| `eph::containers::TrivialData<T>` | concept | Element-type constraint |
| `eph::containers::BoundedQueueStats` / `EvictingQueueStats` (+ *Bytes*) | POD structs | Observability snapshots (also `std::formatter`) |

This sub-project has no `main()`; it is a header-only library consumed
by the rest of `ephemeral_dev` (feed handlers, order gateways, latency
benchmarks, etc.).

---

## Dependencies

### Internal (in-tree)

```
  eph-containers
        |
        v
    eph-utils   <-- Align<T>, cpu_relax, CACHE_LINE_SIZE
```

`eph-containers` depends **only** on `eph-utils` from the parent
workspace (declared as `add_deps("eph-utils", { public = true })`).

### External (xmake packages)

| Package           | Purpose                                                        |
|-------------------|----------------------------------------------------------------|
| `spdlog`          | Internal DEBUG logging from `BoundedQueue::stats()` clamp path |
| `tabulate`        | Benchmark-only; pretty result tables                           |
| `gtest` / `gmock` | Test-only                                                      |
| `benchmark`       | Benchmark-only (Google Benchmark)                              |

`spdlog` is used via `SPDLOG_LOGGER_DEBUG` with `SPDLOG_ACTIVE_LEVEL`
so the call sites compile away when the parent build defines a higher
minimum log level.

---

## Testing

| Test Binary | Source | Size | Coverage Focus |
|---|---|---:|---|
| `test_ring_buffer` | `tests/test_ring_buffer.cpp` | 366 lines | Push ordering, lookback indexing, overflow, concurrent read/write |
| `test_bounded_queue` | `tests/test_bounded_queue.cpp` | 1421 lines | All capacity boundary sizes (1 / 2 / 1024), try_* and blocking paths, batch ops, peek, timed, drain, stats |
| `test_bounded_queue_bytes` | `tests/test_bounded_queue_bytes.cpp` | 634 lines | Byte envelope push/pop, ts variants, batch, oversized rejection, stats |
| `test_evicting_queue` | `tests/test_evicting_queue.cpp` | 1616 lines | Multi-slot SeqLock race patterns, `Capacity==1` specialisation, overwrite counting, peek, loss-rate |
| `test_evicting_queue_bytes` | `tests/test_evicting_queue_bytes.cpp` | 818 lines | Byte envelope wait-free writes, discard counter, ts variants, stats, peek semantics |

Total: **389 tests** across 5 binaries. All passed against the
current head-of-tree at the time of this documentation run
(2026-04-09).

Key test scenarios:

- **Boundary capacities** — `BoundedQueue<T, 1>` (mask = 0; every push
  must overwrite index 0) and `BoundedQueue<T, 2>` stress the minimal
  ring.
- **Concurrent SPSC** — dedicated writer/reader threads pinned via
  `std::thread` push millions of elements and validate strict
  FIFO / monotonic sequence numbers.
- **Torn-read defence** — evicting-queue byte tests deliberately read
  while writers are overwriting and verify that no invalid
  `msg.len > MaxDataSize` leaks to the visitor.
- **Stats correctness** — `total_pushed - total_popped = current_size`
  invariants are checked after each phase; `operator-` delta snapshots
  are verified against direct counter math.
- **Drain / consume_all** — `try_consume_all` / `try_consume_n` drain
  helpers are tested for best-effort semantics (no guarantee the
  queue is empty after return, but everything visible at call time is
  consumed).

Benchmarks (`benchmarks/bench_*.cpp`) are not part of CI and must be
built + run explicitly via xmake. They iterate the
`REGISTER_MATRIX(FUNC)` macro over payload sizes {8, 64, 512} bytes
and buffer sizes {1, 8, 64, 512} slots to quantify single-thread
throughput, ping-pong latency, and batch efficiency.
