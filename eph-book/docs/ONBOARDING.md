# eph-book — Developer Onboarding

This guide is for engineers joining the `eph-book` subproject inside the `ephemeral_dev` monorepo. It assumes you know C++ but not the layout of this codebase.

## Development Environment

### Prerequisites

- **C++23 compiler**: GCC 14 or Clang 18+. The library uses `std::optional`, `std::span`, structured bindings, `[[nodiscard]]`, `[[likely]]/[[unlikely]]`, and `consteval`/`constexpr` idioms.
- **xmake** ≥ 2.9. The monorepo build is entirely driven by xmake.
- The system packages `spdlog`, `benchmark`, and `gtest` (pulled in via `add_requires(...)` in the root `xmake.lua`).
- Optional: Doxygen, if you want to render the `///` comments locally.

### Clone and first build

```bash
git clone <repo-url> ephemeral_dev
cd ephemeral_dev

# Configure (release is default; use -m debug for verbose tracing).
xmake f -m release

# Build the eph-book target (header-only — essentially a no-op).
xmake build eph-book

# Build and run the unit tests.
xmake build test_array_book test_map_book test_signals
xmake run test_array_book
xmake run test_map_book
xmake run test_signals
```

### Verify your environment

Running `test_array_book`, `test_map_book`, and `test_signals` should all produce zero failures. If any fail, your toolchain does not match what eph-book expects — double-check the compiler version and that `spdlog` and `gtest` are available.

## Architecture Overview

`eph-book` is the **order-book layer** of the `eph` market-data stack. It sits between feed parsers (`eph-json`, `eph-itch`) and downstream signal / strategy code.

```
  +--------------+   +--------------+
  |  eph-json    |   |  eph-itch    |   (feed parsers — not linked by eph-book)
  |  (Binance)   |   |  (NASDAQ)    |
  +------+-------+   +-------+------+
         |                   |
         v                   v
  +-----------------+ +------------------+
  | BinanceBook     | | ItchBookBuilder  |   (adapters — in include/eph/book/)
  | Adapter<N>      | | <N>              |
  +--------+--------+ +--------+---------+
           |                   |
           v                   v
        +-----+               +-----+
        |Array| <-- generic -->|Map|
        |Book |   queries     |Book|      (core books)
        +--+--+               +--+--+
           |                     |
           v                     v
         +---------------------------+
         |         signals.hpp       |    (pure-function signal calculators)
         | order_imbalance, vwap,    |
         | weighted_mid, spread_bps  |
         +---------------------------+
```

Key design decisions:

- **Two book implementations, one query interface.** `ArrayBook<N>` is the default for shallow L2 books (5-20 levels) where cache locality dominates. `MapBook` handles deep L3 books (1000+ levels) at the cost of `std::map` allocation per insert. Both expose the same query methods (`best_bid`, `best_ask`, `total_bid_qty`, ...), so signal functions and downstream code can be generic over either.
- **Two update styles, one update API.** `update_bid(price, qty)` / `update_ask(price, qty)` accept both crypto-style explicit L2 snapshots *and* (after external order-to-level aggregation) ITCH-style order-level feeds. The adapter classes `BinanceBookAdapter` and `ItchBookBuilder` own the feed-specific translation.
- **Header-only and allocation-free on the hot path** for `ArrayBook` and signal functions. `MapBook` allocates per insert (map nodes), which is why it exists alongside `ArrayBook` rather than replacing it.
- **Named loggers, compile-time filtered.** Each component uses a lazily-constructed `spdlog` logger (`book.array_book`, `book.map_book`, etc.). Log levels are gated by `SPDLOG_ACTIVE_LEVEL`, defined by the parent project at compile time — TRACE in debug mode, INFO in release. Warnings always include the offending values so they are actionable in production.

### Directory layout

```
eph-book/
├── include/eph/
│   ├── book.hpp                 # Convenience aggregate header.
│   └── book/
│       ├── array_book.hpp       # ArrayBook<N> + PriceLevel.
│       ├── map_book.hpp         # MapBook (std::map-backed).
│       ├── signals.hpp          # Pure signal calculators.
│       ├── binance_adapter.hpp  # BinanceBookAdapter (needs eph-json).
│       └── itch_adapter.hpp     # ItchBookBuilder    (needs eph-itch).
├── tests/                       # GoogleTest unit tests.
│   ├── test_array_book.cpp
│   ├── test_map_book.cpp
│   └── test_signals.cpp
├── benchmarks/                  # Google Benchmark micro-benchmarks.
│   ├── bench_array_book.cpp
│   └── bench_map_book.cpp
└── xmake.lua                    # Header-only target + tests + benchmarks.
```

### Key entry points

- **`ArrayBook::update_bid` / `update_ask`** — the core mutator; all other update paths funnel through here.
- **`ArrayBook::update_side`** (private) — the actual sort-maintenance logic: reject NaN, clamp negative qty, in-place update or shift-insert, evict-worst when full. Read this function to understand the book's invariants.
- **`ItchBookBuilder::process`** — single dispatch for all supported ITCH message types. Each `handle_*` helper updates the internal order map and the per-price accumulator, then pushes the aggregated quantity into `ArrayBook`.
- **`BinanceBookAdapter::load_snapshot`** — the reconnection-recovery path: clears the book and rebuilds it from a REST depth snapshot while capturing `last_update_id`.
- **`eph::book::order_imbalance` / `weighted_mid` / `vwap`** — the signal templates. They work against any book with `best_bid()`, `best_ask()`, `total_bid_qty()`, `total_ask_qty()`.

## Daily Development

### Build

```bash
xmake build eph-book                               # header-only — no-op
xmake build test_array_book test_map_book test_signals
xmake build bench_array_book bench_map_book
```

### Test

```bash
xmake run test_array_book      # L2 array book
xmake run test_map_book        # L3 map book + cross-validation vs ArrayBook
xmake run test_signals         # signal calculators
```

All three tests should finish in well under a second. Per the project test convention, they cover boundary conditions (empty book, NaN prices, single-side-only, full capacity + eviction) in addition to the happy path.

### Benchmark

Before changing any hot path, establish a baseline:

```bash
xmake run bench_array_book   # capture baseline numbers
# ... make your change ...
xmake build bench_array_book
xmake run bench_array_book   # compare
```

The per-project CLAUDE.md rule is: **any performance regression must be investigated and fixed before finalizing a change.**

### Common tasks

**Add a new signal calculator.**
1. Add the function template to `include/eph/book/signals.hpp`, next to existing ones. Keep it `noexcept` and allocation-free.
2. Make it generic over the book type where possible (accept anything with `best_bid()` / `best_ask()` or `total_bid_qty()` / `total_ask_qty()`).
3. Add tests in `tests/test_signals.cpp` exercising both `ArrayBook` and `MapBook` plus empty-book and asymmetric cases.
4. Run the three test binaries and benchmarks.

**Support a new ITCH message type.**
1. Add the `case` in `ItchBookBuilder::process` and a `handle_*` helper below.
2. Re-use `add_qty` / `sub_qty` for price-level updates — they keep the per-price accumulator and the underlying `ArrayBook` in sync.
3. Clamp any "shares to remove" value against the order's `remaining_qty` to guard against over-execution races (follow the pattern in `handle_order_executed`).
4. Add tests in `tests/test_map_book.cpp` (or a new dedicated file) that feed the message and assert both the book state and `order_count()`.

**Debug an order-book divergence.**
Set `SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE` at compile time (the default in debug builds). The `book.*` loggers will emit a TRACE line for every insert/update/remove and a DEBUG line for every book-level mutation. Warnings include the offending price or quantity.

## Code Conventions

### Naming

- Classes: `PascalCase` (`ArrayBook`, `MapBook`, `ItchBookBuilder`).
- Functions and methods: `snake_case` (`update_bid`, `best_ask`, `total_bid_qty`).
- Private members: trailing underscore (`bids_`, `bid_count_`, `book_`).
- Template parameters: `PascalCase` (`MaxLevels`, `Book`).
- Constants: `kPascalCase` (`kEps`, `kTickQuantum`).

### Error handling

This subproject does not throw from the hot path. Error signalling uses:

- `std::optional<T>` — returned when the query cannot produce a value (empty side, degenerate math).
- `bool` return — used by the adapters (`BinanceBookAdapter::update_from_ticker`, `ItchBookBuilder::process`) to indicate "book was modified".
- **Silent no-op** — used when the input is meaningful but has no effect (removing a non-existent level, dropping a level worse than the worst tracked).
- **Warning log + fallback** — used for invariant violations (NaN prices, negative quantities, over-execution). The fallback must keep the book in a consistent state.

### Logging

Every non-trivial function logs at least at TRACE level, and every error/fallback branch logs at WARN or higher. Log messages include the input values that triggered the branch — follow the existing patterns in `ArrayBook::update_side` and the ITCH handlers.

### Commits

The parent repo uses Conventional Commits scoped by module (`feat(book):`, `fix(book):`, `refactor(book):`, `docs(book):`, `build(book):`, `test(book):`). One logical change per commit; each commit must build on its own.

## Troubleshooting

**"`spdlog/spdlog.h` not found"** — the monorepo pulls `spdlog` via `add_requires("spdlog", { optional = true })`. Run `xmake f` once to trigger the dependency fetch, then `xmake build` again.

**Tests fail with link errors referencing `__cxa_call_terminate`** — on Amazon Linux 2023 with GCC 14 this is resolved by the root `xmake.lua` which adds `-Wl,-rpath,/usr/lib/gcc/aarch64-amazon-linux/14`. If you hit it on a different distro, add an equivalent rpath for your GCC runtime.

**A benchmark number regressed** — always re-run the baseline before your change (not the version in a report file) on the same machine with the same build mode. Micro-benchmarks vary by a few percent run-to-run; only investigate regressions that exceed the noise floor.

**`is_crossed()` disagrees between `ArrayBook` and `MapBook`** — as of the most recent fix, both implementations use strict `>` + `1e-12` epsilon. If you see divergence, check whether you are feeding identical (canonicalized) prices into both. `MapBook` quantizes inputs to `1e-9`; `ArrayBook` does not.
