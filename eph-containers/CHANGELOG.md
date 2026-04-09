# Changelog

All notable changes to **eph-containers** are documented here. The
format follows [Keep a Changelog](https://keepachangelog.com/), and the
project uses [Conventional Commits](https://www.conventionalcommits.org/)
on the history side.

The library has not yet been released under a versioned tag; every
entry below belongs to `[Unreleased]`. Grouping reflects the
user-visible nature of each change, not the underlying commit order.

## [Unreleased]

### Added

- **`RingBuffer<T, Capacity>`** — fixed-size circular buffer for tick
  history / lookback signals (e.g. "price N ticks ago", VWAP windows).
  Single-writer, any-reader; compile-time power-of-two capacity.
- **Batch operations** across all queues:
  - `BoundedQueue::try_push_n` / `try_produce_n` (all-or-nothing),
    `try_pop_n`, `try_consume_n`, and blocking `push_n` / `produce_n`.
  - Batch push on `BoundedQueueBytes` and `EvictingQueueBytes` with
    parallel payload + timestamp arrays.
  - Batch zero-copy visitor `try_produce_n` / `try_consume_n` to avoid
    stack-allocated temporaries on the hot path.
- **Drain helpers** — `try_consume_all` on `BoundedQueue` and
  `BoundedQueueBytes` for best-effort shutdown / periodic drain.
- **Timed (`*_for`) variants** for every operation (push, pop, emplace,
  batch push, batch pop, peek, consume) on both queue families,
  allowing callers to spin with a deadline instead of blocking
  indefinitely or failing immediately.
- **Peek APIs** — `BoundedQueue::try_peek` (value + visitor + optional
  overloads) and `EvictingQueue::try_peek_latest` inspect the head
  without advancing the read cursor; byte variants add
  `try_peek_visit[_wts]`. Useful for pre-inspecting message type
  before routing.
- **`EvictingQueue<T, 1>` specialisation** — classic single-slot
  SeqLock for minimal-footprint latest-value snapshots.
- **Observability layer**:
  - `BoundedQueueStats`, `BoundedQueueBytesStats`, `EvictingQueueStats`,
    `EvictingQueueBytesStats` — standalone (non-nested) POD structs,
    each with `dump()` (multi-line human form), `to_json()`, and
    accompanying `std::formatter` specialisation for direct
    `std::format` / `std::print` integration.
  - `Stats::operator-` for delta snapshots, `throughput(ns)` for
    ops/second, and `loss_rate()` on evicting variants.
  - `EvictingQueue::full()`, `size_approx()`, `overwrite_count_approx()`,
    `write_count()`, `read_count()`.
  - `BoundedQueue::available_write()`, `available_read()`.
- **`TrivialData<T>` concept** now gates every queue element type at
  compile time (trivially copyable + trivially destructible + default
  initialisable).
- **`clear()`** on every queue (`BoundedQueue`, `EvictingQueue`, byte
  variants) for use during reconnection or reinitialisation.
- Convenience umbrella header `eph/containers.hpp` now pulls in
  `RingBuffer`, `concepts`, and all four queue headers (previously
  omitted `ring_buffer.hpp`).

### Changed

- **Wait-free writer merged fence + unlock** in `EvictingQueue` so the
  post-write `seq(even)` store is a plain release store. On ARM64 this
  lowers to a single `stlr` (~5 ns) instead of `dmb ishst + str`
  (~15 ns) without changing semantics.
- **Shadow-index fast path** on `BoundedQueue` — writer caches the last
  observed `head_` (and reader caches the last observed `tail_`) so
  the cross-core acquire load only happens when the local prediction
  indicates full/empty.
- **`BoundedQueue<T, 1>` now valid** — the static assertion was relaxed
  so a single-slot ring is legal (mask = 0, every push maps to index
  0). Used internally for degenerate pingpong tests.
- **`Stats` types extracted** from nested `Queue::Stats` aliases to
  standalone types at namespace scope so that
  `std::formatter<QueueStats>` can be specialised without knowing the
  queue template arguments; `using Queue::Stats = ...` is kept as a
  back-compat alias.
- **Named spdlog loggers** — `BoundedQueue::stats()` now routes its
  clamp-diagnostic through a named
  `spdlog::logger("containers.bounded_queue")` instead of the default
  logger, consistent with the rest of the project.
- **Visitor-pattern peek** — `try_peek(T&)` delegates to the visitor
  overload to keep a single copy-path implementation.
- `EvictingQueueBytesStats::throughput` corrected to compute against
  delta snapshots rather than absolute counts.
- `EvictingQueue<T, 1>::overwrite_count_approx()` now uses the actual
  read count for accuracy, matching the primary-template formula.

### Fixed

- **Data races in stats** — atomic loads for monitoring counters so
  that a non-reader thread can call `stats()` safely.
- **SeqLock torn-read defence** in `EvictingQueueBytes` — `msg.len` is
  clamped to `MaxDataSize` before being handed to the visitor, so a
  garbage length read during a concurrent write cannot cause the
  visitor to overrun `msg.data`.
- **Missing `<memory>` include** (IWYU) for `std::construct_at` in the
  bounded queue.
- Minor: removed unused includes, fixed Doxygen typos, fixed missing
  space before `///` comment in Stats declarations.

### Performance

- Batch latency recording + stats merged with the SeqLock fence in
  `EvictingQueue` writer path.
- `try_peek` / `try_peek_latest` reuse the exact SeqLock validation of
  their consuming counterparts — no additional acquire loads.
- Writer and reader zones occupy their own cache lines
  (`alignas(Align<T>)`) in every queue; `static_assert` guards detect
  accidental padding bloat.

### Testing

- SPSC queue **boundary** test coverage for capacities 1 / 2 / 1024.
- Concurrent SPSC stress tests validating strict FIFO ordering,
  monotonic sequence preservation, and stats invariants.
- Torn-read stress tests for the evicting byte variant that verify
  `safe_len` clamping under concurrent write / read.
- `try_consume_all` drain-helper tests with best-effort semantics.
- Read-count metric tests for `EvictingQueue` (both primary and
  single-slot specialisation).

### Documentation

- Doxygen-style `///` comments on every public function, class, and
  template across the library.
- Inline comments explaining non-obvious memory ordering decisions,
  the shadow-index fast-path, and SeqLock encoding
  (`{global_index << 1 | lock_flag}`).
- Expanded README with full API reference and usage examples.

---

Older "update" / "fix" / miscellaneous commits that predate the
conventional-commits convention have been folded into the relevant
Added / Changed / Fixed sections above rather than listed verbatim.
