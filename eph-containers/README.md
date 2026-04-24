# eph-containers

Header-only C++23 library providing lock-free and wait-free
**single-producer / single-consumer (SPSC)** data structures for
inter-thread communication in HFT pipelines. Every container is
fixed-capacity, allocation-free on the hot path, and laid out with
cache-line aligned writer/reader zones to eliminate false sharing.

Two queue families are offered (each with a raw-typed variant and a
byte-oriented envelope wrapper), plus a fixed-size ring buffer for
tick-history lookback:

| Family             | Write semantics                      | Read semantics                      |
|--------------------|--------------------------------------|-------------------------------------|
| `BoundedQueue`     | Back-pressure (spin or fail on full) | FIFO, lock-free                     |
| `EvictingQueue`    | Wait-free (overwrites oldest)        | Lock-free SeqLock optimistic read   |
| `RingBuffer`       | Writer overwrites oldest             | Any-reader lookback (`at(offset)`)  |

## Features

- Header-only, zero runtime dependencies beyond `eph-utils` (in-tree).
- Lock-free `BoundedQueue` with shadow-index optimisation for reduced
  cross-core cache traffic; supports zero-copy visitor writes,
  all-or-nothing batch push, peek, and timed variants.
- Wait-free `EvictingQueue` using per-slot sequence locks; includes an
  explicit `Capacity == 1` specialisation (classic single-slot SeqLock)
  for minimal-footprint latest-value snapshots.
- Byte-oriented wrappers (`BoundedQueueBytes`, `EvictingQueueBytes`)
  carrying payload + optional timestamp envelopes, with both copying and
  zero-copy visitor APIs.
- `RingBuffer<T, Capacity>` for single-writer / any-reader lookback
  (e.g. "price N ticks ago"); power-of-two capacity enforced at compile
  time for bitmask indexing.
- First-class observability: every queue exposes a `Stats` struct with
  `dump()`, `to_json()`, `operator-` (for delta snapshots),
  `throughput()`, and `loss_rate()` (evicting variants). A
  `std::formatter` specialisation is provided for direct
  `std::format` / `std::print` integration.
- Element types are constrained by the `TrivialData<T>` concept
  (trivially copyable + trivially destructible + default initialisable),
  enforced at compile time by C++20 `requires` clauses.

## Requirements

- C++23-capable toolchain (uses `std::format`, `std::span`, concepts,
  `std::expected`-style patterns, `[[likely]]`/`[[unlikely]]`,
  `std::has_single_bit`, `std::construct_at`).
- [xmake](https://xmake.io/) as the build system.
- `eph-utils` sibling sub-project (for `Align<T>` and `cpu_relax()`).
- `spdlog` (pulled via xmake package) — only for internal DEBUG logging
  in `BoundedQueue::stats()` clamping path.
- GoogleTest + Google Benchmark (pulled via xmake packages) for the
  test and benchmark targets.

## Build

From the parent `ephemeral_dev/` workspace:

```bash
# Build the header-only target (installs headers, generates cmake/pc files).
xmake build -g eph-containers

# Build an individual test or benchmark binary.
xmake build test_ring_buffer
xmake build bench_bq_pingpong
```

## Test

```bash
xmake run test_concepts
xmake run test_ring_buffer
xmake run test_bounded_queue
xmake run test_bounded_queue_bytes
xmake run test_evicting_queue
xmake run test_evicting_queue_bytes
```

At the time of writing the test suite comprises **434 tests across 6
binaries** exercising the `TrivialData<T>` concept surface, boundary
conditions (capacity 1/2/1024), SPSC race patterns, batch operations,
peek semantics, and stats correctness. `test_concepts` is a pure
compile-time / `static_assert` suite that validates which types satisfy
`TrivialData` (primitives, aggregates, `std::array`) and which do not
(`std::string`, `std::vector`, types with non-trivial destructors).

## Benchmarks

```bash
xmake build bench_bq_pingpong bench_bq_pushpop bench_bq_throughput bench_bq_batch
xmake build bench_eq_pingpong bench_eq_pushpop bench_eq_throughput
xmake build bench_ring_buffer
```

Benchmarks use Google Benchmark and iterate over a matrix of payload
sizes (8, 64, 512 bytes) and buffer sizes (1, 8, 64, 512 slots) — see
`benchmarks/bench_matrix.hpp`.

## Project layout

```
eph-containers/
├── include/eph/
│   ├── containers.hpp                  # Umbrella header (all public types)
│   └── containers/
│       ├── concepts.hpp                # TrivialData<T> concept
│       ├── ring_buffer.hpp             # RingBuffer<T, Capacity>
│       ├── bounded_queue.hpp           # BoundedQueue<T, Capacity> + Stats
│       ├── bounded_queue_bytes.hpp     # Byte-envelope wrapper
│       ├── evicting_queue.hpp          # EvictingQueue<T, Capacity> (+ <T, 1> spec)
│       └── evicting_queue_bytes.hpp    # Byte-envelope wrapper
├── tests/                              # GoogleTest suites (one per header)
├── benchmarks/                         # Google Benchmark suites
└── xmake.lua                           # Build definition
```

## Usage

### Include the umbrella header

```cpp
#include "eph/containers.hpp"

using namespace eph::containers;
```

### Bounded queue (reliable SPSC FIFO)

```cpp
BoundedQueue<Tick, 1024> q;

// Writer thread
if (!q.try_push(tick)) {
    /* queue full — apply back-pressure or drop at a higher level */
}

// Reader thread
Tick out;
while (q.try_pop(out)) {
    handle(out);
}
```

### Evicting queue (latest-value, wait-free writes)

```cpp
EvictingQueue<Snapshot, 8> q;

// Writer — never blocks, overwrites oldest unread on overflow
q.push(snapshot);

// Reader — always reads the freshest consistent snapshot
if (auto s = q.try_pop_latest(); s.has_value()) {
    process(*s);
}

// Optional single-slot specialisation for minimal-footprint latest-value
EvictingQueue<PriceLevel, 1> last_price;
```

### Byte-oriented queue for variable-length frames

```cpp
BoundedQueueBytes<256, 1024> q;

// Writer
uint8_t buf[256] = { /* ... */ };
(void)q.try_push(std::span(buf, 64));

// Reader
std::array<uint8_t, 256> out{};
if (auto n = q.try_pop(out); n) {
    process_frame(std::span(out.data(), *n));
}
```

### Ring buffer for lookback signals

```cpp
RingBuffer<Tick, 128> history;    // Capacity must be a power of two.
history.push(latest_tick);

// "Price 5 ticks ago"
if (auto t5 = history.at(5); t5.has_value()) {
    double p_prev = t5->price;
}
```

### Stats & monitoring

```cpp
auto snapshot_a = q.stats();
// ... work ...
auto snapshot_b = q.stats();
auto delta      = snapshot_b - snapshot_a;
double ops_per_sec = delta.throughput(elapsed_ns);
std::print("{}\n", snapshot_b);   // std::formatter specialisation
```

## Design notes

- **Cache-line padding** — writer and reader hot zones are placed on
  separate cache lines via `struct alignas(Align<T>)` (from
  `eph-utils`), preventing producer / consumer contention.
- **Shadow indices** — each side keeps a private copy of the peer's
  index and only re-reads the shared atomic when the local cache
  indicates the slow path (full / empty).
- **Power-of-two capacity** — enforced at compile time via
  `std::has_single_bit(Capacity)`; modulo is replaced by a bitmask.
- **Memory ordering** — writers publish with `release`, readers observe
  with `acquire`; relaxed stores are used for writer-local state. On
  ARM64 the `EvictingQueue` write path merges the data→seq(even) fence
  with the seq store so a single `stlr` unlocks the slot (~5 ns vs
  ~15 ns for `dmb ishst + str`).
- **SeqLock invariants** — odd sequence = write-in-progress, even =
  stable; readers retry on torn reads instead of blocking.

## License

No `LICENSE` file is currently provided at the project root. Licensing
should be clarified before external distribution.
