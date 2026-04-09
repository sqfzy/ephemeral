# eph-book

Header-only C++23 library for L2/L3 order-book maintenance and market-microstructure signal computation. Part of the `eph` HFT / market-data toolkit.

All symbols live in namespace `eph::book`. The library is entirely header-only and allocation-free on the hot path (`ArrayBook` and signal calculators). All core mutators are `noexcept`.

## Features

- **`ArrayBook<MaxLevels>`** — fixed-capacity, zero-allocation sorted L2 book backed by `std::array`. Cache-friendly for shallow books (5-20 levels).
- **`MapBook`** — dynamic-depth sorted L3 book backed by `std::map` with `std::greater` on the bid side. Handles 1000+ levels at O(log n) insert/delete.
- **Signal calculators** (`signals.hpp`) — pure function templates for order imbalance, weighted mid / microprice, spread (bps), VWAP, and depth ratio. Generic over any book exposing the expected BBO interface.
- **`BinanceBookAdapter<MaxLevels>`** — bridges Binance WebSocket `bookTicker` and REST depth snapshots into `ArrayBook`. Tracks `last_update_id` for gap detection during reconnect.
- **`ItchBookBuilder<MaxLevels>`** — translates order-level ITCH 5.0 events (AddOrder, Executed, Cancel, Delete, Replace) into an aggregated L2 `ArrayBook`. Maintains an internal order map and per-price accumulators for O(1) incremental updates.

## Requirements

- C++23 compiler (GCC 14 or Clang 18+).
- [xmake](https://xmake.io/) for building tests and benchmarks.
- Package dependencies (pulled in by the parent `ephemeral_dev` xmake project):
  - `spdlog` — leveled logging, compile-time filtered via `SPDLOG_ACTIVE_LEVEL`.
  - `gtest` — unit tests.
  - `benchmark` — micro-benchmarks.

## Build

From the repository root (`ephemeral_dev/`):

```bash
# Build just the header-only library target (noop — header only).
xmake build eph-book

# Build unit tests.
xmake build test_array_book test_map_book test_signals

# Build benchmarks.
xmake build bench_array_book bench_map_book
```

## Test

```bash
xmake run test_array_book
xmake run test_map_book
xmake run test_signals
```

The test suite covers: sort-order invariants, BBO extraction, mid/spread math, in-place updates, level removal (`qty == 0`), overflow eviction, empty-book edge cases, NaN rejection, deep-book paths (100+ levels), `top_n` extraction, and ArrayBook/MapBook cross-validation on identical input sequences.

## Benchmark

```bash
xmake run bench_array_book
xmake run bench_map_book
```

Benchmarks use a realistic Binance `bookTicker` JSON payload and exercise single-level updates, BBO queries, and a full parse-to-book-update cycle.

## Project Layout

```
eph-book/
├── include/eph/
│   ├── book.hpp                 # Convenience header (pulls ArrayBook + MapBook)
│   └── book/
│       ├── array_book.hpp       # ArrayBook<N> + PriceLevel
│       ├── map_book.hpp         # MapBook (std::map-backed)
│       ├── signals.hpp          # order_imbalance, weighted_mid, vwap, ...
│       ├── binance_adapter.hpp  # BinanceBookAdapter<N> (needs eph-json)
│       └── itch_adapter.hpp     # ItchBookBuilder<N>   (needs eph-itch)
├── tests/                       # GoogleTest unit tests
├── benchmarks/                  # Google Benchmark micro-benchmarks
└── xmake.lua                    # header-only target + tests/benchmarks
```

## Public API at a Glance

### `PriceLevel` (defined in `array_book.hpp`)

```cpp
struct PriceLevel {
    double price = 0.0;  // price in native exchange units
    double qty   = 0.0;  // aggregate quantity at this price
};
```

Canonical level representation shared by `ArrayBook`, `MapBook`, and the signal calculators.

### `ArrayBook<MaxLevels = 20>`

| Method | Returns | Behavior |
|---|---|---|
| `update_bid(price, qty)` / `update_ask(price, qty)` | `void` | Insert/update a level. `qty == 0` removes; NaN prices are rejected; negative `qty` is clamped to 0 with a warning. Full book + worse price is silently dropped. |
| `best_bid()` / `best_ask()` | `optional<PriceLevel>` | Top-of-book, `nullopt` if side empty. |
| `mid_price()` | `optional<double>` | `(best_bid + best_ask) / 2`. |
| `spread()` | `optional<double>` | `best_ask - best_bid` (may be negative if crossed). |
| `bids()` / `asks()` | `span<const PriceLevel>` | Active levels, sorted best-first. Invalidated by any mutating call. |
| `bid_depth()` / `ask_depth()` / `level_count()` | `size_t` | Active level counts. |
| `total_bid_qty()` / `total_ask_qty()` | `double` | Summed across all active levels. |
| `is_crossed()` | `bool` | `best_bid > best_ask + 1e-12`. |
| `is_locked()` | `bool` | `|best_bid - best_ask| <= 1e-12`. |
| `clear()` | `void` | Reset both sides. |
| `max_levels` | `static constexpr size_t` | Exposes the `MaxLevels` template parameter. |

### `MapBook`

Same query surface as `ArrayBook` (`best_bid`, `best_ask`, `mid_price`, `spread`, `is_crossed`, `is_locked`, `bid_depth`, `ask_depth`, `level_count`, `total_bid_qty`, `total_ask_qty`, `clear`), with the following differences:

- **Price quantization**: inputs are snapped to multiples of `1e-9` on insertion to prevent near-duplicate keys from different floating-point rounding paths.
- `bids()` / `asks()` return `std::vector<PriceLevel>` (owning copies) rather than spans — `std::map` is node-based and cannot expose a contiguous span. **Avoid on the hot path.**
- `top_bids(n)` / `top_asks(n)` return a `std::vector<PriceLevel>` containing at most `min(n, depth)` best levels. Prefer these over `bids()` / `asks()` when full depth is not needed.

### Signal Functions (in `signals.hpp`)

All templates accept any book exposing `best_bid()`, `best_ask()`, `total_bid_qty()`, `total_ask_qty()`. All are `noexcept` and allocation-free.

| Function | Returns | Description |
|---|---|---|
| `order_imbalance(book)` | `double` | `(bid_qty - ask_qty) / (bid_qty + ask_qty)`, range `[-1, 1]`. Returns 0.0 if both sides are empty. |
| `weighted_mid(book)` | `optional<double>` | BBO-level size-weighted mid: `(bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)`. |
| `microprice(book)` | `optional<double>` | Currently delegates to `weighted_mid`; semantically reserved for future multi-level extensions. |
| `spread_bps(book)` | `optional<double>` | `(ask - bid) / mid * 10000`; `nullopt` if mid <= 0. |
| `vwap(span<const PriceLevel>)` | `optional<double>` | Sum of `price * qty` over `sum of qty`. `nullopt` if empty or total qty <= 0. |
| `depth_ratio(book)` | `double` | `total_bid_qty / total_ask_qty`. Returns 0.0 if the ask side is empty (no divide-by-zero). |

### `BinanceBookAdapter<MaxLevels = 20>`

| Method | Returns | Behavior |
|---|---|---|
| `update_from_ticker(BookTicker)` | `bool` | Parses the four string price/qty fields and updates the top-of-book on each side. Returns `false` on parse failure. |
| `load_snapshot(DepthSnapshot)` | `size_t` | Clears the book, loads every level from a REST depth response, and stores `last_update_id`. Returns `bid_depth + ask_depth` (clamped by `MaxLevels` per side via ArrayBook eviction). |
| `last_update_id()` | `int64_t` | Sequence ID from the most recent `load_snapshot` (0 if never loaded). Use to drop stale incremental updates (`U <= last_update_id()`). |
| `book()` | `ArrayBook<MaxLevels>&` | Const and mutable access to the underlying book. |

Requires `eph-json` to be on the include path (for `BookTicker` / `DepthSnapshot`). The `eph-book` target does **not** link `eph-json`; callers must include both.

### `ItchBookBuilder<MaxLevels = 20>`

Handles ITCH 5.0 message types `'A'` AddOrder, `'F'` AddOrderMPID, `'E'` OrderExecuted, `'C'` OrderExecutedWithPrice, `'X'` OrderCancel, `'D'` OrderDelete, `'U'` OrderReplace. Other message types are silently ignored.

| Method | Returns | Behavior |
|---|---|---|
| `process(MessageView)` | `bool` | Dispatches to the handler for `msg.msg_type`. Returns `true` if the book was mutated. |
| `book()` | `ArrayBook<MaxLevels>&` | Const and mutable access. |
| `order_count()` | `size_t` | Number of tracked live orders. |
| `clear()` | `void` | Resets tracked orders, per-price accumulators, and the underlying book. |

Internally maintains `order_ref -> {price, remaining_qty, side}` plus per-price `std::map<double, double>` accumulators (one per side) so that add / execute / cancel operations are O(1) against the L2 book. Executed and cancelled shares are clamped to the order's remaining quantity to protect against exchange-side over-execution races (logged at WARN level when clamping fires).

Requires `eph-itch` on the include path.

## Usage

### Basic L2 book with signals

```cpp
#include <eph/book/array_book.hpp>
#include <eph/book/signals.hpp>

eph::book::ArrayBook<20> book;
book.update_bid(50000.0, 1.5);
book.update_ask(50001.0, 0.8);
book.update_bid(49999.0, 2.0);

auto best_bid = book.best_bid();   // PriceLevel{50000.0, 1.5}
auto mid      = book.mid_price();  // 50000.5

book.update_bid(49999.0, 0.0);     // remove the 49999 level

double imbalance = eph::book::order_imbalance(book);
auto   wmid      = eph::book::weighted_mid(book);
auto   spd_bps   = eph::book::spread_bps(book);
auto   bid_vwap  = eph::book::vwap(book.bids());
```

### Deep book with MapBook

```cpp
#include <eph/book/map_book.hpp>
#include <eph/book/signals.hpp>

eph::book::MapBook book;
for (const auto& [px, q] : bids_feed) book.update_bid(px, q);
for (const auto& [px, q] : asks_feed) book.update_ask(px, q);

auto top5_bids = book.top_bids(5);       // std::vector<PriceLevel>
auto top5_asks = book.top_asks(5);

double imbalance = eph::book::order_imbalance(book);  // works on any book type
```

### Binance adapter (reconnection flow)

```cpp
#include <eph/book/binance_adapter.hpp>

eph::book::BinanceBookAdapter<20> adapter;

// 1. REST snapshot for initial state.
if (auto snap = eph::json::binance::parse_depth_response(buf, len)) {
    adapter.load_snapshot(*snap);
    auto base_seq = adapter.last_update_id();
    // drop incoming deltas with U <= base_seq
}

// 2. Incremental BBO updates from the WebSocket bookTicker stream.
if (auto tk = eph::json::binance::BookTicker::from(json_view)) {
    adapter.update_from_ticker(*tk);
}
```

### ITCH 5.0 adapter

```cpp
#include <eph/book/itch_adapter.hpp>

eph::book::ItchBookBuilder<20> builder;
for (const auto& msg : itch_messages) {
    if (builder.process(msg)) {
        // book was modified — recompute signals if needed
    }
}
std::size_t live = builder.order_count();
builder.clear();  // for a new trading session
```

## Logging

Every translation unit uses a named `spdlog` logger under the `book.*` namespace:

| Component | Logger name |
|---|---|
| `ArrayBook` | `book.array_book` |
| `MapBook` | `book.map_book` |
| `BinanceBookAdapter` | `book.binance_adapter` |
| `ItchBookBuilder` | `book.itch_adapter` |

Loggers are lazily constructed via `spdlog::stdout_color_mt` (with a `spdlog::get` fallback if the name is already registered — safe against duplicate-registration on TU re-initialization). Log levels are compile-time filtered via `SPDLOG_ACTIVE_LEVEL`, set by the parent project to `TRACE` in debug builds and `INFO` in release. Warning logs include the actual offending values (e.g., clamped quantities, NaN prices) so they are actionable in production.

## Dependencies

| Dependency | Required by | Notes |
|---|---|---|
| `eph-core` | all components | `parse_number.hpp` is used by `BinanceBookAdapter`. Declared as a public dep in `xmake.lua`. |
| `spdlog` | all components | Public package dep. |
| `eph-json` | `binance_adapter.hpp` only | Not linked by `eph-book`; callers must include it. |
| `eph-itch` | `itch_adapter.hpp` only | Not linked by `eph-book`; callers must include it. |
